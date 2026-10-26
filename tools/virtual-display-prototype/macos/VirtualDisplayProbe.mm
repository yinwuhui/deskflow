// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
// Independent platform experiment. Private API declarations are separately attributed.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/IOKitLib.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <VideoToolbox/VideoToolbox.h>
#import "third_party/DeskPad/CGVirtualDisplayPrivate.h"
#include <atomic>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <cmath>
#include <climits>

static volatile sig_atomic_t interrupted = 0;
static pid_t expectedParent = 0;
static void onSignal(int value) { interrupted = value; }

static CGDirectDisplayID layoutOwner = kCGNullDirectDisplay;
static void processLayoutCommand();

static bool pump(bool (^done)(void), double seconds, bool observeInterrupt = true) {
  NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:seconds];
  while (!done() && (!observeInterrupt || !interrupted) && deadline.timeIntervalSinceNow > 0) {
    if (expectedParent && getppid() != expectedParent) interrupted = SIGTERM;
    if (!interrupted && layoutOwner) processLayoutCommand();
    [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                           beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
  }
  return done();
}

static NSArray<NSNumber *> *activeDisplays() {
  CGDirectDisplayID displays[64];
  uint32_t count = 0;
  if (CGGetActiveDisplayList(64, displays, &count) != kCGErrorSuccess)
    @throw [NSException exceptionWithName:@"ProbeError" reason:@"Cannot enumerate displays" userInfo:nil];
  NSMutableArray *result = [NSMutableArray array];
  for (uint32_t i = 0; i < count; ++i) [result addObject:@(displays[i])];
  return [result sortedArrayUsingSelector:@selector(compare:)];
}

@interface ProbeSink : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
  std::atomic<unsigned> frames;
  std::atomic<bool> dimensionsMatch;
  std::atomic<unsigned> encodedFrames;
  std::atomic<unsigned> pendingEncodes;
  std::atomic<unsigned> droppedRawFrames;
  std::atomic<int> encodeError;
  VTCompressionSessionRef encoder;
  FILE *h264;
  bool framedOutput;
}
@property(nonatomic, strong) NSString *streamError;
@end

@implementation ProbeSink
- (instancetype)init {
  self = [super init];
  if (self) {
    frames.store(0); dimensionsMatch.store(false); encodedFrames.store(0); encodeError.store(0);
    encoder = nullptr; h264 = nullptr; framedOutput = false;
  }
  return self;
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample
        ofType:(SCStreamOutputType)type {
  if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample)) return;
  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
  if (!attachments || CFArrayGetCount(attachments) == 0) return;
  NSDictionary *info = (__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments, 0);
  NSNumber *status = info[SCStreamFrameInfoStatus];
  if (!status || status.integerValue != SCFrameStatusComplete) return;
  CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
  if (!image) return;
  dimensionsMatch.store(CVPixelBufferGetWidth(image) == 1920 && CVPixelBufferGetHeight(image) == 1080);
  const auto index = frames.fetch_add(1);
  if (encoder) {
    unsigned pending = pendingEncodes.load();
    do {
      if (pending >= 2) { droppedRawFrames.fetch_add(1); return; }
    } while (!pendingEncodes.compare_exchange_weak(pending, pending + 1));
    const OSStatus result = VTCompressionSessionEncodeFrame(encoder, image, CMTimeMake(index, 30),
      CMTimeMake(1, 30), nullptr, nullptr, nullptr);
    if (result != noErr) { pendingEncodes.fetch_sub(1); encodeError.store(result); }
  }
}
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
  dispatch_async(dispatch_get_main_queue(), ^{ self.streamError = error.localizedDescription; });
}
@end

static bool writeNAL(std::vector<uint8_t>& output, const uint8_t *bytes, size_t length) {
  const uint8_t start[] = {0, 0, 0, 1};
  constexpr size_t limit = 4 * 1024 * 1024 - 9;
  if (length > limit - 4 || output.size() > limit - 4 - length) return false;
  output.insert(output.end(), start, start + 4);
  output.insert(output.end(), bytes, bytes + length);
  return true;
}

static void compressed(void *context, void *, OSStatus status, VTEncodeInfoFlags flags,
                       CMSampleBufferRef sample) {
  ProbeSink *sink = (__bridge ProbeSink *)context;
  struct Credit {
    std::atomic<unsigned>& pending;
    ~Credit() { pending.fetch_sub(1); }
  } credit{sink->pendingEncodes};
  if (status != noErr) { sink->encodeError.store(status); return; }
  if ((flags & kVTEncodeInfo_FrameDropped) || !sample || !CMSampleBufferDataIsReady(sample)) return;
  CMBlockBufferRef data = CMSampleBufferGetDataBuffer(sample);
  const size_t size = data ? CMBlockBufferGetDataLength(data) : 0;
  if (size == 0 || size > 4 * 1024 * 1024) { sink->encodeError.store(-1); return; }
  std::vector<uint8_t> bytes(size);
  if (CMBlockBufferCopyDataBytes(data, 0, size, bytes.data()) != noErr) { sink->encodeError.store(-2); return; }
  CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
  size_t count = 0;
  int nalLengthSize = 0;
  if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, nullptr, nullptr,
                                                       &count, &nalLengthSize) != noErr || nalLengthSize != 4) {
    sink->encodeError.store(-3); return;
  }
  // Every access unit is independently written under the FILE lock. Parameter
  // sets are repeated for this small offline decoder experiment, not a wire format.
  std::vector<uint8_t> unit;
  bool written = true;
  for (size_t index = 0; index < count; ++index) {
    const uint8_t *parameter = nullptr; size_t length = 0;
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, index, &parameter,
          &length, nullptr, nullptr) != noErr || !writeNAL(unit, parameter, length)) {
      written = false; break;
    }
  }
  size_t offset = 0;
  while (written && offset < bytes.size()) {
    if (bytes.size() - offset < 4) { written = false; break; }
    const uint32_t length = (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset+1]) << 16) |
                            (uint32_t(bytes[offset+2]) << 8) | bytes[offset+3];
    offset += 4;
    if (length == 0 || length > bytes.size() - offset) { written = false; break; }
    written = writeNAL(unit, bytes.data() + offset, length);
    offset += length;
  }
  if (written) {
    flockfile(sink->h264);
    if (sink->framedOutput) {
      const auto size = static_cast<uint32_t>(unit.size());
      const uint8_t header[] = {uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size)};
      written = fwrite(header, 1, 4, sink->h264) == 4;
    }
    written = written && fwrite(unit.data(), 1, unit.size(), sink->h264) == unit.size() && fflush(sink->h264) == 0;
    funlockfile(sink->h264);
  }
  if (written) sink->encodedFrames.fetch_add(1);
  else sink->encodeError.store(-4);
}

static void requireSelectors(Class type, NSArray<NSString *> *selectors) {
  if (!type) @throw [NSException exceptionWithName:@"UnsupportedBackend" reason:@"Private virtual-display class unavailable" userInfo:nil];
  for (NSString *name in selectors) {
    if (![type instancesRespondToSelector:NSSelectorFromString(name)])
      @throw [NSException exceptionWithName:@"UnsupportedBackend" reason:[@"Missing selector: " stringByAppendingString:name] userInfo:nil];
  }
}

// Runs on the native owner's loop, only while the retained CGVirtualDisplay is live.
static NSDictionary *displayTopology(CGDirectDisplayID owned) {
  NSMutableArray *items = [NSMutableArray array];
  NSDictionary *target = nil;
  CGRect primary = CGDisplayBounds(CGMainDisplayID());
  for (NSNumber *number in activeDisplays()) {
    CGDirectDisplayID identifier = number.unsignedIntValue;
    CGRect bounds = CGDisplayBounds(identifier);
    double scale = bounds.size.width > 0 ? CGDisplayPixelsWide(identifier) / bounds.size.width : 1;
    NSDictionary *item = @{@"displayId":number.stringValue, @"x":@(bounds.origin.x-primary.origin.x),
      @"y":@(bounds.origin.y-primary.origin.y), @"width":@(bounds.size.width), @"height":@(bounds.size.height),
      @"pixelWidth":@(CGDisplayPixelsWide(identifier)), @"pixelHeight":@(CGDisplayPixelsHigh(identifier)),
      @"scale":@(scale), @"primary":@(identifier==CGMainDisplayID()), @"owned":@(identifier==owned)};
    [items addObject:item];
    if(identifier==owned)target=item;
  }
  if(!target || CGDisplayIsInMirrorSet(owned) || owned==CGMainDisplayID())return nil;
  return @{@"coordinateSpace":@"primary-relative-logical", @"layout":target, @"displays":items,
           @"capabilities":@{@"position":@YES,@"scale":@NO}};
}
static void processLayoutCommand() {
  static NSString *lastCommand = nil;
  NSString *directory = NSProcessInfo.processInfo.environment[@"DESKFLOW_LAYOUT_DIRECTORY"];
  NSString *token = NSProcessInfo.processInfo.environment[@"DESKFLOW_LAYOUT_TOKEN"];
  if(!directory.length || !token.length)return;
  NSString *path = [directory stringByAppendingPathComponent:@"request.json"];
  NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
  if(!attributes || [attributes fileSize]>4096)return;
  NSData *bytes=[NSData dataWithContentsOfFile:path];
  id object=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil];
  if(![object isKindOfClass:NSDictionary.class])return;
  NSDictionary *request=object;
  if(![request[@"token"] isEqual:token] || ![request[@"id"] isKindOfClass:NSString.class] || [request[@"id"] isEqual:lastCommand])return;
  lastCommand=request[@"id"];
  bool accepted=false;
  NSDictionary *data=displayTopology(layoutOwner);
  if([request[@"operation"] isEqual:@"QueryDisplayLayout"])accepted=data!=nil;
  else if([request[@"operation"] isEqual:@"SetDisplayLayout"] && data){
    id layout=request[@"layout"];
    if([layout isKindOfClass:NSDictionary.class]){
      id x=layout[@"x"],y=layout[@"y"],scale=layout[@"scale"];
      bool valid=[x isKindOfClass:NSNumber.class]&&[y isKindOfClass:NSNumber.class]&&
        std::isfinite([x doubleValue])&&std::isfinite([y doubleValue])&&
        floor([x doubleValue])==[x doubleValue]&&floor([y doubleValue])==[y doubleValue]&&
        fabs([x doubleValue])<=32768&&fabs([y doubleValue])<=32768&&
        (!scale||[scale isEqual:data[@"layout"][@"scale"]]);
      if(valid){
        const CGRect previous=CGDisplayBounds(layoutOwner);
        CGDisplayConfigRef configuration=nullptr;
        if(CGBeginDisplayConfiguration(&configuration)==kCGErrorSuccess){
          const CGRect primary=CGDisplayBounds(CGMainDisplayID());
          if(CGConfigureDisplayOrigin(configuration,layoutOwner,[x intValue]+primary.origin.x,[y intValue]+primary.origin.y)==kCGErrorSuccess)
            accepted=CGCompleteDisplayConfiguration(configuration,kCGConfigureForSession)==kCGErrorSuccess;
          else CGCancelDisplayConfiguration(configuration);
        }
        data=displayTopology(layoutOwner);
        const bool applied=accepted;
        accepted=accepted&&data&&[data[@"layout"][@"x"] isEqual:x]&&[data[@"layout"][@"y"] isEqual:y];
        if(applied&&!accepted&&data){
          // The still-retained owner is the only object eligible for rollback.
          CGDisplayConfigRef rollback=nullptr;
          if(CGBeginDisplayConfiguration(&rollback)==kCGErrorSuccess){
            if(CGConfigureDisplayOrigin(rollback,layoutOwner,previous.origin.x,previous.origin.y)==kCGErrorSuccess)
              CGCompleteDisplayConfiguration(rollback,kCGConfigureForSession);
            else CGCancelDisplayConfiguration(rollback);
          }
          data=displayTopology(layoutOwner);
        }
      }
    }
  }
  NSDictionary *reply=@{@"token":token,@"id":lastCommand,@"accepted":@(accepted),@"data":data?:@{}};
  [[NSJSONSerialization dataWithJSONObject:reply options:0 error:nil]
    writeToFile:[directory stringByAppendingPathComponent:@"response.json"] atomically:YES];
}

int main(int argc, const char *argv[]) {
  @autoreleasepool {
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGPIPE, SIG_IGN);
    bool exercise = false, request = false, createOnly = false;
    bool framed = false;
    double duration = 0;
    NSString *h264Path = nil;
    for (int i = 1; i < argc; ++i) {
      NSString *arg = @(argv[i]);
      if ([arg isEqualToString:@"--exercise"]) exercise = true;
      else if ([arg isEqualToString:@"--request-capture-permission"]) request = true;
      else if ([arg isEqualToString:@"--create-only"]) createOnly = true;
      else if ([arg isEqualToString:@"--h264-output"] && i+1 < argc) h264Path = @(argv[++i]);
      else if ([arg isEqualToString:@"--framed-h264-stdout"]) framed = true;
      else if ([arg isEqualToString:@"--parent-pid"] && i+1 < argc) {
        char *end = nullptr;
        const long parent = strtol(argv[++i], &end, 10);
        if (!end || *end || parent < 1 || parent > INT_MAX) {
          fprintf(stderr, "Parent PID out of range\n"); return 2;
        }
        expectedParent = static_cast<pid_t>(parent);
      }
      else if ([arg isEqualToString:@"--duration"] && i+1 < argc) {
        char *end = nullptr;
        duration = strtod(argv[++i], &end);
        if (!end || *end || !std::isfinite(duration) || duration < 0 || duration > 3600) {
          fprintf(stderr, "Duration must be 0..3600 seconds\n"); return 2;
        }
      }
      else { fprintf(stderr, "Usage: macos-display-probe [--exercise | --create-only] [--request-capture-permission] [--h264-output NEW_FILE]\n"); return 2; }
    }
    if (exercise && createOnly) { fprintf(stderr, "Choose --exercise or --create-only\n"); return 2; }
    if ((h264Path || framed) && !exercise) { fprintf(stderr, "Encoding requires --exercise\n"); return 2; }
    if (expectedParent && !framed) { fprintf(stderr, "Parent monitoring requires framed streaming\n"); return 2; }
    if (framed && h264Path) { fprintf(stderr, "Choose file or framed stdout\n"); return 2; }
    NSMutableDictionary *report = [@{@"schemaVersion": @1, @"platform": @"macOS",
      @"osVersion": NSProcessInfo.processInfo.operatingSystemVersionString,
      @"privateApi": @YES, @"certified": @NO, @"created": @NO, @"extended": @NO,
      @"captured": @NO, @"cleanupVerified": @NO} mutableCopy];
    NSArray *before = activeDisplays();
    report[@"displaysBefore"] = before;
    __strong CGVirtualDisplay *display = nil;
    __strong SCStream *capture = nil;
    ProbeSink *sink = [[ProbeSink alloc] init];
    bool started = false;
    @try {
      Class descriptorClass = NSClassFromString(@"CGVirtualDisplayDescriptor");
      Class displayClass = NSClassFromString(@"CGVirtualDisplay");
      Class settingsClass = NSClassFromString(@"CGVirtualDisplaySettings");
      Class modeClass = NSClassFromString(@"CGVirtualDisplayMode");
      // Creation support is independent of the receiver's desktop readiness.
      report[@"privateApiAvailable"] = @NO;
      @try {
        requireSelectors(descriptorClass, @[@"setDispatchQueue:", @"setName:", @"setMaxPixelsWide:",
          @"setMaxPixelsHigh:", @"setSizeInMillimeters:", @"setVendorID:", @"setProductID:", @"setSerialNum:"]);
        requireSelectors(displayClass, @[@"initWithDescriptor:", @"applySettings:", @"displayID"]);
        requireSelectors(settingsClass, @[@"setModes:", @"setHiDPI:"]);
        requireSelectors(modeClass, @[@"initWithWidth:height:refreshRate:"]);
        report[@"privateApiAvailable"] = @YES;
      } @catch (NSException *error) {
        if (exercise || createOnly) @throw error;
      }
      bool permission = CGPreflightScreenCaptureAccess();
      if (request && !permission) permission = CGRequestScreenCaptureAccess();
      report[@"capturePermission"] = @(permission);
      if (exercise && !permission)
        @throw [NSException exceptionWithName:@"PermissionRequired" reason:@"Allow screen recording for VirtualDisplayProbe in System Settings, then run again" userInfo:nil];
      NSDictionary *desktopSession = CFBridgingRelease(CGSessionCopyCurrentDictionary());
      const bool onConsole = [desktopSession[(__bridge NSString *)kCGSessionOnConsoleKey] boolValue];
      // Optional WindowServer field; absence is not proof that the session is unlocked.
      NSNumber *locked = desktopSession[@"CGSSessionScreenIsLocked"];
      // macOS 27 may omit the Quartz lock field. IOConsoleLocked remains a
      // typed system lock signal. Any positive lock signal wins; absence of
      // both signals remains unknown and must never authorize streaming.
      io_registry_entry_t registry = IORegistryGetRootEntry(kIOMainPortDefault);
      CFTypeRef consoleLocked = registry ? IORegistryEntryCreateCFProperty(
          registry, CFSTR("IOConsoleLocked"), kCFAllocatorDefault, 0) : nullptr;
      const bool registryKnown = consoleLocked && CFGetTypeID(consoleLocked) == CFBooleanGetTypeID();
      if (registryKnown) {
        locked = @([locked boolValue] || CFBooleanGetValue((CFBooleanRef)consoleLocked));
      }
      if (consoleLocked) CFRelease(consoleLocked);
      if (registry) IOObjectRelease(registry);
      report[@"screenLockSource"] = registryKnown ? @"IOConsoleLocked" : locked ? @"QuartzSession" : @"unknown";
      report[@"sessionOnConsole"] = @(onConsole);
      report[@"screenLockKnown"] = @(locked != nil);
      if (locked) report[@"screenLocked"] = locked;
      if (expectedParent && getppid() != expectedParent)
        @throw [NSException exceptionWithName:@"SessionUnavailable" reason:@"Parent session already exited" userInfo:nil];
      if ((exercise || createOnly) && (!onConsole || !locked || [locked boolValue] || before.count == 0))
        @throw [NSException exceptionWithName:@"SessionUnavailable" reason:@"An active, unlocked console desktop with a display is required for this probe" userInfo:nil];
      if (exercise || createOnly) {
        CGVirtualDisplayDescriptor *descriptor = [[descriptorClass alloc] init];
        [descriptor setDispatchQueue:dispatch_get_main_queue()];
        descriptor.name = @"Deskflow temporary virtual-display probe";
        descriptor.maxPixelsWide = 1920;
        descriptor.maxPixelsHigh = 1080;
        descriptor.sizeInMillimeters = CGSizeMake(508, 286);
        descriptor.vendorID = 0x4446;
        descriptor.productID = 0x0001;
        descriptor.serialNum = arc4random_uniform(0x7ffffffe) + 1;
        display = [[displayClass alloc] initWithDescriptor:descriptor];
        if (!display) @throw [NSException exceptionWithName:@"CreateFailed" reason:@"Virtual display creation failed" userInfo:nil];
        CGVirtualDisplaySettings *settings = [[settingsClass alloc] init];
        settings.hiDPI = 0;
        settings.modes = @[[[modeClass alloc] initWithWidth:1920 height:1080 refreshRate:30]];
        if (![display applySettings:settings])
          @throw [NSException exceptionWithName:@"ConfigureFailed" reason:@"Virtual display rejected mode" userInfo:nil];
        CGDirectDisplayID identifier = display.displayID;
        report[@"displayId"] = @(identifier);
        bool ready = pump(^bool{ return [activeDisplays() containsObject:@(identifier)]; }, 5);
        report[@"created"] = @(ready);
        report[@"extended"] = @(ready && !CGDisplayIsInMirrorSet(identifier));
        report[@"displaysDuring"] = activeDisplays();
        if (!ready) @throw [NSException exceptionWithName:@"TopologyTimeout" reason:@"Virtual display did not become active" userInfo:nil];
        layoutOwner=identifier;
        if (framed) {
          fprintf(stderr, "{\"event\":\"displayCreated\",\"virtualDisplayId\":\"%u\"}\n", identifier);
          fflush(stderr);
        }
        if (exercise) {
          __block SCShareableContent *content = nil;
          __block NSString *failure = nil;
          __block bool received = false;
          [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:NO
            completionHandler:^(SCShareableContent *value, NSError *error) {
              dispatch_async(dispatch_get_main_queue(), ^{ content = value; failure = error.localizedDescription; received = true; });
            }];
          if (!pump(^bool{ return received; }, 10) || failure)
            @throw [NSException exceptionWithName:@"CaptureDiscoveryFailed" reason:failure ?: @"ScreenCaptureKit timed out" userInfo:nil];
          SCDisplay *target = nil;
          for (SCDisplay *item in content.displays) if (item.displayID == identifier) target = item;
          if (!target) @throw [NSException exceptionWithName:@"CaptureTargetMissing" reason:@"Virtual display not listed by ScreenCaptureKit" userInfo:nil];
          SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:target excludingWindows:@[]];
          SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
          config.width = 1920; config.height = 1080;
          config.minimumFrameInterval = CMTimeMake(1, 30);
          config.pixelFormat = kCVPixelFormatType_32BGRA;
          config.queueDepth = 3;
          config.showsCursor = YES;
          config.capturesAudio = NO;
          if (h264Path || framed) {
            if (framed) {
              sink->h264 = stdout;
              sink->framedOutput = true;
            } else {
              const int fd = open(h264Path.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL, 0600);
              if (fd < 0) @throw [NSException exceptionWithName:@"OutputFailed" reason:@(strerror(errno)) userInfo:nil];
              sink->h264 = fdopen(fd, "wb");
              if (!sink->h264) { close(fd); @throw [NSException exceptionWithName:@"OutputFailed" reason:@"Cannot open output stream" userInfo:nil]; }
            }
            NSDictionary *spec = @{(__bridge NSString *)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @YES};
            OSStatus status = VTCompressionSessionCreate(nullptr, 1920, 1080, kCMVideoCodecType_H264,
              (__bridge CFDictionaryRef)spec, nullptr, nullptr, compressed, (__bridge void *)sink, &sink->encoder);
            if (status != noErr) @throw [NSException exceptionWithName:@"EncoderFailed" reason:[NSString stringWithFormat:@"VT create: %d", (int)status] userInfo:nil];
            NSDictionary *properties = @{
              (__bridge NSString *)kVTCompressionPropertyKey_RealTime: @YES,
              (__bridge NSString *)kVTCompressionPropertyKey_AllowFrameReordering: @NO,
              (__bridge NSString *)kVTCompressionPropertyKey_ProfileLevel: (__bridge NSString *)kVTProfileLevel_H264_Baseline_AutoLevel,
              (__bridge NSString *)kVTCompressionPropertyKey_ExpectedFrameRate: @30,
              (__bridge NSString *)kVTCompressionPropertyKey_MaxKeyFrameInterval: @30,
              (__bridge NSString *)kVTCompressionPropertyKey_AverageBitRate: @4000000,
              (__bridge NSString *)kVTCompressionPropertyKey_DataRateLimits: @[@500000, @1]};
            report[@"targetBitrate"] = @4000000;
            report[@"maximumBytesPerSecond"] = @500000;
            status = VTSessionSetProperties(sink->encoder, (__bridge CFDictionaryRef)properties);
            if (status == noErr) status = VTCompressionSessionPrepareToEncodeFrames(sink->encoder);
            if (status != noErr) @throw [NSException exceptionWithName:@"EncoderFailed" reason:[NSString stringWithFormat:@"VT configure: %d", (int)status] userInfo:nil];
            CFTypeRef hardware = nullptr;
            if (VTSessionCopyProperty(sink->encoder, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
                  nullptr, &hardware) == noErr && hardware) {
              report[@"hardwareEncoder"] = (__bridge id)hardware;
              CFRelease(hardware);
            }
          }
          capture = [[SCStream alloc] initWithFilter:filter configuration:config delegate:sink];
          NSError *error = nil;
          if (![capture addStreamOutput:sink type:SCStreamOutputTypeScreen
                     sampleHandlerQueue:dispatch_queue_create("org.deskflow.probe.capture", DISPATCH_QUEUE_SERIAL) error:&error])
            @throw [NSException exceptionWithName:@"CaptureOutputFailed" reason:error.localizedDescription userInfo:nil];
          __block bool startDone = false;
          __block NSString *startError = nil;
          [capture startCaptureWithCompletionHandler:^(NSError *error) {
            dispatch_async(dispatch_get_main_queue(), ^{ startError = error.localizedDescription; startDone = true; });
          }];
          started = true;
          if (!pump(^bool{ return startDone; }, 10) || startError)
            @throw [NSException exceptionWithName:@"CaptureStartFailed" reason:startError ?: @"Capture start timeout" userInfo:nil];
          if (duration == 0 && framed && expectedParent) {
            while (!interrupted && sink.streamError == nil && sink->encodeError.load() == 0)
              pump(^bool{ return sink.streamError != nil || sink->encodeError.load() != 0; }, 1);
          }
          else if (duration > 0) pump(^bool{ return sink.streamError != nil || sink->encodeError.load() != 0; }, duration);
          else pump(^bool{ return sink->frames.load() >= 1 || sink.streamError != nil; }, 10);
          report[@"frames"] = @(sink->frames.load());
          report[@"droppedRawFrames"] = @(sink->droppedRawFrames.load());
          report[@"captured"] = @(sink->frames.load() > 0 && sink->dimensionsMatch.load());
          if (sink.streamError) report[@"captureError"] = sink.streamError;
        } else {
          pump(^bool{ return false; }, 1);
        }
      }
    } @catch (NSException *error) {
      layoutOwner=kCGNullDirectDisplay;
      report[@"errorCode"] = error.name;
      report[@"error"] = error.reason ?: @"Unknown error";
    } @finally {
      layoutOwner=kCGNullDirectDisplay;
      if (capture && started) {
        __block bool stopDone = false;
        __block NSString *stopError = nil;
        [capture stopCaptureWithCompletionHandler:^(NSError *error) {
          dispatch_async(dispatch_get_main_queue(), ^{ stopError = error.localizedDescription; stopDone = true; });
        }];
        report[@"captureStopped"] = @(pump(^bool{ return stopDone; }, 5, false) && !stopError);
        if (stopError) report[@"captureError"] = stopError;
      }
      capture = nil;
      if (sink->encoder) {
        const OSStatus status = VTCompressionSessionCompleteFrames(sink->encoder, kCMTimeInvalid);
        if (status != noErr) sink->encodeError.store(status);
        VTCompressionSessionInvalidate(sink->encoder);
        CFRelease(sink->encoder);
        sink->encoder = nullptr;
      }
      if (sink->h264) {
        if (fclose(sink->h264) != 0) sink->encodeError.store(-5);
        sink->h264 = nullptr;
        report[@"h264Output"] = framed ? @"framed-stdout" : h264Path;
        report[@"encodedFrames"] = @(sink->encodedFrames.load());
        report[@"encoderStatus"] = @(sink->encodeError.load());
      }
      display = nil;
      layoutOwner=kCGNullDirectDisplay;
      bool cleaned = pump(^bool{ return [activeDisplays() isEqualToArray:before]; }, 5, false);
      report[@"cleanupVerified"] = @(cleaned);
      report[@"displaysAfter"] = activeDisplays();
      report[@"interrupted"] = @(interrupted != 0);
    }
    bool passed = !report[@"error"] && !report[@"captureError"] && !interrupted;
    if (exercise || createOnly)
      passed = passed && [report[@"created"] boolValue] && [report[@"extended"] boolValue] && [report[@"cleanupVerified"] boolValue];
    if (exercise) passed = passed && [report[@"captured"] boolValue] && [report[@"captureStopped"] boolValue];
    if (h264Path || framed) passed = passed && sink->encodedFrames.load() > 0 && sink->encodeError.load() == 0;
    report[@"passed"] = @(passed);
    report[@"scope"] = exercise ? ((h264Path || framed) ? @"Virtual output, target-only capture, local H.264 encoding and cleanup; no network session" :
      @"Virtual output, target-only capture and cleanup; no encoding or networking") :
      (createOnly ? @"Virtual output and cleanup only; capture not exercised" : @"Read-only API and permission discovery");
    NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
    FILE *diagnostics = framed ? stderr : stdout;
    fwrite(json.bytes, 1, json.length, diagnostics);
    fputc('\n', diagnostics);
    return passed ? 0 : 1;
  }
}
