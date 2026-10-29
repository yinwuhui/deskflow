// SPDX-License-Identifier: MIT
#include "MacCaptureSource.h"
#include "MacVirtualOutput.h"
#include "display/supervision/MediaWorkPermit.h"
#include "display/supervision/WorkerProcess.h"
#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#include <QCoreApplication>
#include <QThread>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
using namespace deskflow::display::infra;
namespace {
struct CaptureState
{
  std::atomic<bool> accepting{false}, invalidated{false}, startDone{false}, stopDone{true}, stopIssued{false},
      streamStopped{false};
  std::mutex mutex;
  QString failure;
  FrameDescriptor descriptor;
  std::shared_ptr<FrameBudget> budget = std::make_shared<FrameBudget>(3, 32 * 1024 * 1024);
  std::shared_ptr<FramePool> pool;
  SCStream *__strong stream = nil;
  id<SCStreamOutput> __strong sink = nil;
  std::shared_ptr<std::atomic<bool>> live;
  std::shared_ptr<MediaWorkPermit> permit;
  bool permitted() const
  {
    return !permit || permit->active();
  }
  bool valid() const
  {
    return permitted() && accepting.load() && !invalidated.load() && live && live->load();
  }
  std::atomic<uint64_t> samples{0}, complete{0}, published{0}, budgetDrops{0};
  std::atomic<int> lastStatus{-1}, stopError{0};
  uint64_t sequence = 0; // sample queue only
  void fail(const QString &why)
  {
    accepting.store(false);
    std::lock_guard lock(mutex);
    if (failure.isEmpty())
      failure = why;
  }
};
// Called only on the native main thread, after the start completion.
void stopNative(std::shared_ptr<CaptureState> s)
{
  if (s->streamStopped.load()) {
    s->stopDone.store(true);
    return;
  }
  if (!s->stream || !s->startDone.load() || s->stopIssued.exchange(true))
    return;
  [s->stream stopCaptureWithCompletionHandler:^(NSError *error) {
    s->stopError.store(error ? int(error.code) : 0);
    if (error)
      s->fail("CaptureStopFailed");
    if (!error || s->streamStopped.load())
      s->stopDone.store(true);
  }];
}
void topologyChanged(CGDirectDisplayID, CGDisplayChangeSummaryFlags, void *context)
{
  auto *state = static_cast<CaptureState *>(context);
  state->accepting.store(false);
  state->invalidated.store(true);
}
} // namespace
@interface BMouseCaptureSink : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
  std::weak_ptr<CaptureState> state;
}
@end
@implementation BMouseCaptureSink
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
  Q_UNUSED(stream);
  Q_UNUSED(error);
  if (auto current = state.lock()) {
    current->fail("CaptureStreamStopped");
    current->streamStopped.store(true);
    current->stopDone.store(true);
  }
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type
{
  Q_UNUSED(stream);
  auto current = state.lock();
  if (current)
    ++current->samples;
  if (!current || !current->valid() || type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample))
    return;
  NSArray *attachments = (__bridge NSArray *)CMSampleBufferGetSampleAttachmentsArray(sample, false);
  if (!attachments.count)
    return;
  NSNumber *status = attachments[0][SCStreamFrameInfoStatus];
  if (![status isKindOfClass:[NSNumber class]])
    return;
  if (status.integerValue == SCFrameStatusBlank || status.integerValue == SCFrameStatusSuspended) {
    current->fail("CaptureSourceSuspended");
    return;
  }
  current->lastStatus.store(int(status.integerValue));
  if (status.integerValue != SCFrameStatusComplete)
    return;
  ++current->complete;
  CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
  if (!pixels || CVPixelBufferGetPixelFormatType(pixels) != kCVPixelFormatType_32BGRA ||
      CVPixelBufferGetWidth(pixels) != 1920 || CVPixelBufferGetHeight(pixels) != 1080) {
    current->fail("CaptureFormatChanged");
    return;
  }
  const auto time =
      CMTimeConvertScale(CMSampleBufferGetPresentationTimeStamp(sample), 1000000000, kCMTimeRoundingMethod_Default);
  if (!CMTIME_IS_NUMERIC(time) || time.value <= 0 || current->sequence == UINT64_MAX) {
    current->fail("CaptureTimestampInvalid");
    return;
  }
  // A single waiting frame: discard only Published frames, never borrowed ones.
  if (auto stale = current->pool->readNewest()) {
    const auto ticket = stale->ticket;
    stale.reset();
    current->pool->releaseRead(ticket);
  }
  constexpr uint64_t stride = 1920 * 4, bytes = stride * 1080;
  auto ticket = current->pool->reserve(bytes, MemoryDomain::Cpu);
  if (!ticket) {
    ++current->budgetDrops;
    return; // downstream owns the budget; never expand it
  }
  if (CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
    current->pool->abortWrite(*ticket);
    return;
  }
  const auto sourceStride = CVPixelBufferGetBytesPerRow(pixels);
  const auto *source = static_cast<const unsigned char *>(CVPixelBufferGetBaseAddress(pixels));
  if (!source || sourceStride < stride || sourceStride > SIZE_MAX / 1080 ||
      CVPixelBufferGetDataSize(pixels) < sourceStride * 1080) {
    CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
    current->pool->abortWrite(*ticket);
    current->fail("CapturePlaneInvalid");
    return;
  }
  std::shared_ptr<unsigned char[]> owner;
  try {
    owner.reset(new unsigned char[bytes]);
  } catch (...) {
    CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
    current->pool->abortWrite(*ticket);
    current->fail("CaptureAllocationFailed");
    return;
  }
  for (size_t row = 0; row < 1080; ++row)
    std::memcpy(owner.get() + row * stride, source + row * sourceStride, stride);
  CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
  std::shared_ptr<void> allocation(owner, owner.get());
  const void *cpuData = owner.get();
  owner.reset();
  if (!current->pool->adopt(*ticket, {std::move(allocation), bytes, MemoryDomain::Cpu, cpuData})) {
    current->pool->abortWrite(*ticket);
    current->fail("CaptureBufferAdoptionFailed");
    return;
  }
  // Pool is the sole producer owner before publication.
  auto descriptor = current->descriptor;
  descriptor.sequence = ++current->sequence;
  descriptor.captureNanoseconds = uint64_t(time.value);
  descriptor.captureTimeValid = true;
  if (!current->valid()) {
    current->pool->abortWrite(*ticket);
    return;
  }
  if (!current->pool->publish(*ticket, std::move(descriptor))) {
    current->pool->abortWrite(*ticket);
    current->fail("CaptureFrameRejected");
  } else
    ++current->published;
}
@end
namespace deskflow::display::infra {
bool MacCaptureBinding::fillWorkerSource(WorkerPrepare &p) const
{
  if ((m_permit && !m_permit->active()) || !m_live || !m_live->load() || !m_claimed || m_claimed->load())
    return false;
  p.sourceResource = QString::fromStdString(m_resource);
  p.source = {NativeSourceKind::CgDisplay, m_display, 0};
  p.sourceIdentity = m_identity;
  p.sourceContextDigest = m_topologyDigest;
  p.sourceTopologyGeneration = m_generation;
  return true;
}
bool MacCaptureBinding::claimWorkerSource(WorkerPrepare &p) const
{
  if (!fillWorkerSource(p))
    return false;
  bool expected = false;
  return m_claimed->compare_exchange_strong(expected, true);
}
std::optional<MacCaptureBinding> MacCaptureBinding::withMediaPermit(std::shared_ptr<MediaWorkPermit> permit) const
{
  if (m_permit || !m_live || !m_live->load() || !m_claimed || m_claimed->load() || !permit ||
      !permit->allowsCapture(
          m_owner, m_session, m_epoch, m_planHash, m_userSession, QString::fromStdString(m_resource),
          {NativeSourceKind::CgDisplay, m_display, 0}, m_identity, m_topologyDigest, m_generation
      ))
    return {};
  auto copy = *this;
  copy.m_permit = std::move(permit);
  return copy;
}
struct MacCaptureSource::Impl
{
  std::shared_ptr<CaptureState> state = std::make_shared<CaptureState>();
  TimePoint deadline{};
  bool launched = false, registered = false, stopping = false;
};
MacCaptureSource::MacCaptureSource() : m_impl(std::make_unique<Impl>())
{
}
MacCaptureSource::~MacCaptureSource()
{
  stop();
  if (m_impl->registered)
    CGDisplayRemoveReconfigurationCallback(topologyChanged, m_impl->state.get());
}
bool MacCaptureSource::start(
    const WorkerSourceLease &lease, std::shared_ptr<MediaWorkPermit> permit, TimePoint deadline
)
{
  const auto &b = lease.binding();
  const auto &p = lease.preparation();
  const auto inventory = macDisplayInventory();
  bool owned = false;
  for (const auto &d : inventory.displays)
    owned |= d.id == p.source.primary && d.identity == p.sourceIdentity && d.vendor == 0x424d && d.product == 0x0201 &&
             d.serial == macResourceSerial(QString::fromStdString(b.token.owner), p.sourceResource);
  if (!lease.active() || b.localUserSession != macUserSessionReference() || b.direction != Direction::Send ||
      p.source.kind != NativeSourceKind::CgDisplay || !inventory.complete || !owned ||
      inventory.digest != p.sourceContextDigest || !permit ||
      !permit->allowsCapture(
          b.token.owner, b.token.session, b.token.epoch, b.planHash, b.localUserSession, p.sourceResource, p.source,
          p.sourceIdentity, p.sourceContextDigest, p.sourceTopologyGeneration
      )) {
    m_impl->state->fail("CaptureWorkerEvidenceRejected");
    return false;
  }
  MacCaptureBinding binding;
  binding.m_live = lease.m_live;
  binding.m_claimed = lease.m_claimed;
  binding.m_resource = p.sourceResource.toStdString();
  binding.m_session = b.token.session;
  binding.m_owner = b.token.owner;
  binding.m_planHash = b.planHash;
  binding.m_userSession = b.localUserSession;
  binding.m_permit = std::move(permit);
  binding.m_identity = p.sourceIdentity;
  binding.m_topologyDigest = p.sourceContextDigest;
  binding.m_display = uint32_t(p.source.primary);
  binding.m_epoch = b.token.epoch;
  binding.m_generation = p.sourceTopologyGeneration;
  return start(binding, deadline);
}
bool MacCaptureSource::start(const MacCaptureBinding &binding, TimePoint deadline)
{
  auto s = m_impl->state;
  if (![NSThread isMainThread] || m_impl->launched || m_impl->stopping || Clock::now() >= deadline)
    return false;
  m_impl->launched = true;
  if (!QCoreApplication::instance() || !QCoreApplication::instance()->inherits("QGuiApplication")) {
    s->fail("CaptureNativeEventLoopRequired");
    return false;
  }
  deadline = std::min(deadline, Clock::now() + std::chrono::seconds(10));
  m_impl->deadline = deadline;
  s->permit = binding.m_permit;
  if (!s->permitted()) {
    s->fail("CaptureMediaPermitInactive");
    return false;
  }
  bool claimed = false;
  if (!binding.m_live || !binding.m_live->load() || !binding.m_claimed ||
      !binding.m_claimed->compare_exchange_strong(claimed, true)) {
    s->fail("CaptureBindingRevokedOrUsed");
    return false;
  }
  s->live = binding.m_live;
  if (!CGPreflightScreenCaptureAccess()) {
    s->fail("ScreenCapturePermissionRequired");
    return false;
  }
  if (!validId(binding.m_resource, 16) || !validId(binding.m_session, 16) || !binding.m_epoch ||
      !binding.m_generation) {
    s->fail("CaptureBindingInvalid");
    return false;
  }
  if (CGDisplayRegisterReconfigurationCallback(topologyChanged, s.get()) != kCGErrorSuccess) {
    s->fail("CaptureTopologyWatchFailed");
    return false;
  }
  m_impl->registered = true;
  const auto inventory = macDisplayInventory();
  const auto desktop = macDesktopState();
  bool matched = false;
  for (const auto &d : inventory.displays)
    matched |= d.id == binding.m_display && d.identity == binding.m_identity && d.active && !d.mirrored &&
               d.pixelWidth == 1920 && d.pixelHeight == 1080;
  if (!inventory.complete || inventory.digest != binding.m_topologyDigest || !matched || s->invalidated.load() ||
      !desktop.onConsole || !desktop.lockKnown || desktop.locked) {
    s->fail("CaptureSourceChanged");
    return false;
  }
  s->descriptor.session = binding.m_session;
  s->descriptor.captureClockId = "macos.cm-host-time";
  s->descriptor.sourceResource = binding.m_resource;
  s->descriptor.topologyGeneration = binding.m_generation;
  s->descriptor.epoch = binding.m_epoch;
  s->descriptor.width = s->descriptor.logicalWidth = 1920;
  s->descriptor.height = s->descriptor.logicalHeight = 1080;
  s->descriptor.visible = {0, 0, 1920, 1080};
  s->descriptor.planes = {{0, 7680, 8294400}};
  s->descriptor.color = ColorProfile::SdrBt709Full8;
  s->pool = std::make_shared<FramePool>(binding.m_resource, binding.m_epoch, binding.m_session, s->budget);
  s->stopDone.store(false);
  const uint32_t displayId = binding.m_display;
  [SCShareableContent
      getShareableContentExcludingDesktopWindows:YES
                             onScreenWindowsOnly:YES
                               completionHandler:^(SCShareableContent *content, NSError *error) {
                                 dispatch_async(dispatch_get_main_queue(), ^{
                                   if (!s->permitted() || s->invalidated.load() || !s->live->load() ||
                                       Clock::now() >= deadline) {
                                     s->fail("CapturePreparationExpired");
                                     s->stopDone.store(true);
                                     return;
                                   }
                                   SCDisplay *target = nil;
                                   for (SCDisplay *display in content.displays)
                                     if (display.displayID == displayId)
                                       target = display;
                                   if (error || !target) {
                                     s->fail("CaptureTargetMissing");
                                     s->stopDone.store(true);
                                     return;
                                   }
                                   SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:target
                                                                                     excludingWindows:@[]];
                                   SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
                                   config.width = 1920;
                                   config.height = 1080;
                                   config.minimumFrameInterval = CMTimeMake(1, 30);
                                   config.pixelFormat = kCVPixelFormatType_32BGRA;
                                   config.queueDepth = 3;
                                   config.showsCursor = YES;
                                   config.colorSpaceName = kCGColorSpaceITUR_709;
                                   config.colorMatrix = kCVImageBufferYCbCrMatrix_ITU_R_709_2;
                                   if (@available(macOS 13.0, *))
                                     config.capturesAudio = NO;
                                   BMouseCaptureSink *sink = [BMouseCaptureSink new];
                                   sink->state = s;
                                   s->sink = sink;
                                   s->stream = [[SCStream alloc] initWithFilter:filter
                                                                  configuration:config
                                                                       delegate:sink];
                                   NSError *addError = nil;
                                   if (![s->stream
                                              addStreamOutput:sink
                                                         type:SCStreamOutputTypeScreen
                                           sampleHandlerQueue:dispatch_queue_create(
                                                                  "org.busymouse.capture.frames", DISPATCH_QUEUE_SERIAL
                                                              )
                                                        error:&addError]) {
                                     s->fail("CaptureOutputFailed");
                                     s->stream = nil;
                                     s->stopDone.store(true);
                                     return;
                                   }
                                   s->accepting.store(true);
                                   [s->stream startCaptureWithCompletionHandler:^(NSError *startError) {
                                     dispatch_async(dispatch_get_main_queue(), ^{
                                       s->startDone.store(true);
                                       if (startError) {
                                         s->fail("CaptureStartFailed");
                                         s->stopDone.store(true);
                                       } else if (!s->valid())
                                         stopNative(s);
                                     });
                                   }];
                                 });
                               }];
  return true;
}
void MacCaptureSource::poll()
{
  auto s = m_impl->state;
  if (!m_impl->launched || m_impl->stopping)
    return;
  if (!s->permitted())
    s->fail("CaptureMediaPermitInactive");
  if (s->invalidated.load())
    s->fail("CaptureTopologyChanged");
  if (s->live && !s->live->load())
    s->fail("CaptureBindingRevoked");
  if (!s->startDone.load() && Clock::now() >= m_impl->deadline)
    s->fail("CapturePreparationExpired");
  if (!error().isEmpty())
    stop();
}
void MacCaptureSource::stop()
{
  if (m_impl->stopping)
    return;
  m_impl->stopping = true;
  auto s = m_impl->state;
  s->accepting.store(false);
  s->invalidated.store(true);
  if (s->pool)
    s->pool->retire();
  if (s->stream)
    stopNative(s);
  else
    s->stopDone.store(true);
}
bool MacCaptureSource::stopped() const
{
  auto s = m_impl->state;
  return s->stopDone.load() && (!s->pool || s->pool->drained());
}
QString MacCaptureSource::error() const
{
  std::lock_guard lock(m_impl->state->mutex);
  return m_impl->state->failure;
}
std::optional<ReadFrame> MacCaptureSource::readNewest()
{
  auto s = m_impl->state;
  if (!s->valid() || !s->pool)
    return {};
  auto frame = s->pool->readNewest();
  if (frame && !s->valid()) {
    const auto ticket = frame->ticket;
    frame.reset();
    s->pool->releaseRead(ticket);
  }
  return frame;
}
std::shared_ptr<CpuFrameLease> MacCaptureSource::takeCpuLease()
{
  auto s = m_impl->state;
  if (!s->valid() || !s->pool)
    return {};
  auto lease = CpuFrameLease::acquire(s->pool);
  if (!s->valid())
    lease.reset();
  return lease;
}
bool MacCaptureSource::releaseRead(const FrameTicket &ticket)
{
  return m_impl->state->pool && m_impl->state->pool->releaseRead(ticket);
}
MacCaptureStatistics MacCaptureSource::statistics() const
{
  auto s = m_impl->state;
  return {s->samples.load(),    s->complete.load(),  s->published.load(),    s->budgetDrops.load(),
          s->lastStatus.load(), s->stopError.load(), s->streamStopped.load()};
}
FrameBudget::Usage MacCaptureSource::usage() const
{
  return m_impl->state->budget->usage();
}
} // namespace deskflow::display::infra
