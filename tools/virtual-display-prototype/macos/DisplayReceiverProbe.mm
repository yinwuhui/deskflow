// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
// Bounded BGRA window sink. FFmpeg decodes locally; network remains H.264/TLS.
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

static volatile sig_atomic_t interrupted = 0;
static void stopSignal(int value) { interrupted = value; }
static std::atomic<unsigned> submitted{0};
static std::atomic<bool> failed{false};
static constexpr size_t width = 1920, height = 1080, frameBytes = width * height * 4;

@interface ReceiverDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSWindow *window;
@end
@implementation ReceiverDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { return YES; }
- (void)windowWillClose:(NSNotification *)notification { [NSApp terminate:nil]; }
@end

static void freePixels(void *, const void *data, size_t) { free(const_cast<void *>(data)); }

int main() {
  @autoreleasepool {
    signal(SIGINT, stopSignal); signal(SIGTERM, stopSignal); signal(SIGPIPE, SIG_IGN);
    NSApplication *app = NSApplication.sharedApplication;
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    ReceiverDelegate *delegate = [ReceiverDelegate new];
    app.delegate = delegate;
    delegate.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 960, 540)
      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
      backing:NSBackingStoreBuffered defer:NO];
    delegate.window.title = @"Deskflow virtual display (native experimental receiver)";
    delegate.window.delegate = delegate;
    delegate.window.releasedWhenClosed = NO;
    delegate.window.contentView.wantsLayer = YES;
    delegate.window.contentView.layer.contentsGravity = kCAGravityResizeAspect;
    delegate.window.contentView.layer.backgroundColor = NSColor.blackColor.CGColor;
    [delegate.window center];
    [delegate.window makeKeyAndOrderFront:nil];
    [app activateIgnoringOtherApps:YES];
    dispatch_semaphore_t credit = dispatch_semaphore_create(1);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      while (!interrupted) {
        // At most one frame awaits the main thread and one is being read.
        dispatch_semaphore_wait(credit, DISPATCH_TIME_FOREVER);
        void *pixels = malloc(frameBytes);
        if (!pixels) { failed.store(true); dispatch_semaphore_signal(credit); break; }
        size_t offset = 0;
        while (offset < frameBytes && !interrupted) {
          const ssize_t count = read(STDIN_FILENO, static_cast<char *>(pixels) + offset, frameBytes - offset);
          if (count <= 0) break;
          offset += static_cast<size_t>(count);
        }
        if (offset != frameBytes) {
          free(pixels); dispatch_semaphore_signal(credit);
          if (offset != 0) failed.store(true);
          break;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
          CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, pixels, frameBytes, freePixels);
          CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
          CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, color,
            static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) | static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst), provider, nullptr, false, kCGRenderingIntentDefault);
          if (image) {
            [CATransaction begin]; [CATransaction setDisableActions:YES];
            delegate.window.contentView.layer.contents = (__bridge id)image;
            [CATransaction commit];
            submitted.fetch_add(1); CGImageRelease(image);
          } else { failed.store(true); }
          CGColorSpaceRelease(color); CGDataProviderRelease(provider);
          dispatch_semaphore_signal(credit);
        });
      }
      // FIFO main-queue ordering applies the final frame before shutdown.
      dispatch_async(dispatch_get_main_queue(), ^{
        fprintf(stderr, "{\"event\":\"nativeViewerStopped\",\"submittedFrames\":%u,\"presentationVerified\":false,\"passed\":%s}\n",
          submitted.load(), !failed.load() && submitted.load() > 0 ? "true" : "false");
        [NSApp stop:nil];
        NSEvent *event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
          modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
        [NSApp postEvent:event atStart:NO];
      });
    });
    [app run];
    return failed.load() || submitted.load() == 0 ? 1 : 0;
  }
}
