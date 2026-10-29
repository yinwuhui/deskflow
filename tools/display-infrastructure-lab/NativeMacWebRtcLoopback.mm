// SPDX-License-Identifier: MIT
// Manual diagnostic: real virtual output -> SCK -> standard WebRTC -> native paint.
#include "ControlLoopback.h"
#include "MacResourceThread.h"
#include "display/media/gstreamer/NativeWebRtcEngine.h"
#include "display/platform/macos/MacResourceProvider.h"
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QWidget>
#include <QWindow>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace deskflow::display::infra;
static void require(bool value, const char *why)
{
  if (!value)
    throw std::runtime_error(why);
}
static QString makeId()
{
  return QUuid::createUuid().toString(QUuid::Id128);
}
static bool until(const std::function<bool()> &condition, int ms)
{
  QElapsedTimer t;
  t.start();
  while (t.elapsed() < ms) {
    if (condition())
      return true;
    QCoreApplication::processEvents();
    QThread::msleep(2);
  }
  return condition();
}
static NSWindow *native(QWidget &widget)
{
  require(QGuiApplication::platformName() == "cocoa", "native Cocoa platform required");
  NSView *view = (__bridge NSView *)(reinterpret_cast<void *>(widget.winId()));
  require(view && [view isKindOfClass:[NSView class]] && view.window, "native window required");
  return view.window;
}
static uint32_t screenId(NSWindow *window)
{
  return [window.screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
}
static NSScreen *screen(uint32_t identifier)
{
  for (NSScreen *item in [NSScreen screens])
    if ([item.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == identifier)
      return item;
  return nil;
}
static bool onScreen(NSWindow *window)
{
  NSArray *windows = CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID));
  for (NSDictionary *entry in windows)
    if ([entry[(__bridge NSString *)kCGWindowNumber] integerValue] == window.windowNumber &&
        [entry[(__bridge NSString *)kCGWindowOwnerPID] longLongValue] == QCoreApplication::applicationPid() &&
        [entry[(__bridge NSString *)kCGWindowAlpha] doubleValue] > 0)
      return true;
  return false;
}
@interface BMouseLabScene : NSView
@property(nonatomic) uint64_t frameNumber;
@end
@implementation BMouseLabScene
- (BOOL)isFlipped
{
  return YES;
}
- (void)drawRect:(NSRect)dirtyRect
{
  (void)dirtyRect;
  [[NSColor colorWithSRGBRed:24.0 / 255 green:38.0 / 255 blue:72.0 / 255 alpha:1] setFill];
  NSRectFill(self.bounds);
  [[NSColor colorWithSRGBRed:40.0 / 255 green:180.0 / 255 blue:60.0 / 255 alpha:1] setFill];
  NSRectFill(NSMakeRect(100, 100, 500, 300));
  [[NSColor colorWithSRGBRed:210.0 / 255 green:40.0 / 255 blue:45.0 / 255 alpha:1] setFill];
  NSRectFill(NSMakeRect(800, 100, 500, 300));
  [[NSColor colorWithSRGBRed:240.0 / 255 green:180.0 / 255 blue:20.0 / 255 alpha:1] setFill];
  NSRectFill(NSMakeRect(self.frameNumber % 1400, 700, 300, 180));
}
@end
class Presenter final : public QWidget
{
public:
  std::shared_ptr<CpuFrameLease> frame;
  uint64_t presented = 0, last = 0;
  Presenter()
  {
    setWindowTitle("BMouse native WebRTC display lab");
    resize(960, 540);
  }
  void paintEvent(QPaintEvent *) override
  {
    auto lease = frame;
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (lease) {
      const auto &d = lease->descriptor();
      QImage image(
          static_cast<const uchar *>(lease->data()), d.width, d.height, d.planes[0].stride, QImage::Format_ARGB32
      );
      painter.drawImage(rect(), image);
      painter.end();
      if (isVisible() && windowHandle() && windowHandle()->isExposed() && last != d.sequence) {
        last = d.sequence;
        ++presented;
      }
    }
  }
};
static ResourceRecord resource(const QString &owner, const QString &session, const QString &plan, const QString &kind)
{
  ResourceRecord r;
  r.id = makeId();
  r.owner = owner;
  r.creationIntent = makeId();
  r.session = session;
  r.planHash = plan;
  r.userSession = macUserSessionReference();
  r.bootInstance = macBootInstanceReference();
  r.provider = "macos.cgdisplay.helper.v1";
  r.providerVersion = "1";
  r.kind = kind;
  r.verificationPolicy = "native-helper-and-system-absence.v1";
  return r;
}
static NativeWebRtcConfiguration
media(Direction direction, const QString &session, const QString &owner, const QString &plan, const QString &source)
{
  NativeWebRtcConfiguration c;
  auto &b = c.binding;
  auto &p = c.preparation;
  b.token = {session.toStdString(), 1, 1, 1, owner.toStdString(), makeId().toStdString()};
  b.localUserSession = macUserSessionReference();
  b.buildCompatibility = "native-mac-loopback1";
  b.backend = "webrtc";
  b.profile = "diagnostic.webrtc.h264.control-map.v1";
  b.direction = direction;
  b.planHash = plan;
  b.controlBinding = QString(64, 'c');
  b.localIdentity = QString(64, direction == Direction::Send ? 'a' : 'b');
  b.peerIdentity = QString(64, direction == Direction::Send ? 'b' : 'a');
  p.allowedPeerAddresses = {"127.0.0.1"};
  p.allowLoopbackDiagnostics = true;
  p.startupDeadlineNanoseconds = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>((Clock::now() + std::chrono::seconds(10)).time_since_epoch())
          .count()
  );
  c.localAddress = "127.0.0.1";
  c.streamSourceResource = source;
  c.streamTopologyGeneration = 1;
  return c;
}
int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  try {
    const QString senderArgument = argc == 4 ? QString::fromLocal8Bit(argv[3]) : QString{};
    const bool isolatedSender = senderArgument.startsWith("--sender-worker=");
    const bool cancelBeforeCreate = argc == 4 && QString::fromLocal8Bit(argv[3]) == "--cancel-before-create";
    require(
        argc == 3 || cancelBeforeCreate || isolatedSender,
        "usage: NativeMacWebRtcLoopback <absolute-helper> <private-runtime> [--cancel-before-create]"
    );
    QString error;
    if (!NativeWebRtcEngine::initializeRuntime(QString::fromLocal8Bit(argv[2]), error))
      throw std::runtime_error(error.toStdString());
    const auto before = macDisplayInventory();
    require(before.complete, "initial native inventory");
    const auto owner = makeId(), session = makeId(),
               plan = QString::fromLatin1(
                   QCryptographicHash::hash((owner + session).toUtf8(), QCryptographicHash::Sha256).toHex()
               );
    QTemporaryDir audit;
    require(audit.isValid(), "audit directory");
    audit.setAutoRemove(false);
    std::cerr << "auditLedger=" << audit.path().toStdString() << "/ledger\n";

    auto worker = resource(owner, session, plan, "worker"), output = resource(owner, session, plan, "virtualOutput");
    output.dependencies = {worker.id};
    WorkerBinding binding;
    binding.token = {session.toStdString(), 1, 1, 1, owner.toStdString(), makeId().toStdString()};
    binding.localUserSession = worker.userSession;
    binding.buildCompatibility = "platform-helper1";
    binding.backend = "platform.macos.virtual-output";
    binding.profile = "macos.virtual-output.cgdisplay.sdr1080.v1";
    binding.planHash = plan;
    binding.localIdentity =
        QString::fromLatin1(QCryptographicHash::hash(worker.userSession.toUtf8(), QCryptographicHash::Sha256).toHex());
    binding.purpose = WorkerPurpose::Platform;
    QFile executable(QFileInfo(QString::fromLocal8Bit(argv[1])).canonicalFilePath());
    require(executable.open(QIODevice::ReadOnly), "helper file");
    QCryptographicHash digest(QCryptographicHash::Sha256);
    require(digest.addData(&executable), "helper digest");
    WorkerRegistration registration{
        QFileInfo(executable).canonicalFilePath(), QString::fromLatin1(digest.result().toHex()), binding.backend,
        binding.profile, WorkerPurpose::Platform
    };
    auto mediaQuiesced = std::make_shared<std::atomic<bool>>(true);
    ResourceAuthorization auth{
        owner,
        session,
        plan,
        worker.userSession,
        worker.bootInstance,
        Clock::now() + std::chrono::seconds(10),
        std::make_shared<std::atomic<bool>>(false)
    };
    LabMacResourceThread resources(
        audit.path() + "/ledger", registration, binding, worker, output, auth, mediaQuiesced
    );
    uint64_t guiTicksDuringCreate = 0;
    QTimer guiPulse;
    guiPulse.setInterval(5);
    QObject::connect(&guiPulse, &QTimer::timeout, &app, [&] { ++guiTicksDuringCreate; });
    guiPulse.start();
    if (cancelBeforeCreate)
      require(resources.cleanup(Clock::now() + std::chrono::seconds(5)), "pre-create cancellation post");
    resources.start();
    require(until([&] { return resources.snapshot().initialized; }, 15000), "resource owner startup deadline");
    guiPulse.stop();
    const auto resourceState = resources.snapshot();
    if (cancelBeforeCreate) {
      require(until([&] { return resources.snapshot().cleanupDone; }, 5000), "pre-create cancellation completion");
      require(resources.wait(2000), "cancel resource thread finalization");
      const auto cancelled = resources.snapshot();
      const auto after = macDisplayInventory();
      require(
          cancelled.isolated && cancelled.creation.primaryError == "ResourcePermitRejected" &&
              cancelled.creation.outcome != ResourceOutcome::Active && cancelled.recordCount == 0 &&
              cancelled.ownerClosed && cancelled.cleanup.outcome == ResourceOutcome::Closed,
          "pre-create cancellation created resource intent"
      );
      require(
          after.complete && !macResourceHintPresent(after, owner, output.id), "cancelled output absence unconfirmed"
      );
      QJsonObject report{
          {"scope", "resource thread pre-create cancellation; no capture/media execution"},
          {"cancelBeforeCreate", true},
          {"resourceThreadIsolated", cancelled.isolated},
          {"resourceIntents", int(cancelled.recordCount)},
          {"ownerClosed", cancelled.ownerClosed},
          {"displaysBefore", int(before.displays.size())},
          {"displaysAfter", int(after.displays.size())}
      };
      std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
      audit.setAutoRemove(true);
      return 0;
    }
    if (resourceState.creation.outcome != ResourceOutcome::Active)
      std::cerr << "outputError=" << resourceState.creation.primaryError.toStdString() << "/"
                << resourceState.creation.cleanupError.toStdString()
                << " provider=" << resourceState.providerError.toStdString() << '\n';
    require(resourceState.creation.outcome == ResourceOutcome::Active, "native output create");
    require(resourceState.isolated && guiTicksDuringCreate >= 3, "resource I/O blocked GUI owner");
    auto captureBinding = resourceState.binding;
    require(bool(captureBinding), "native source binding");
    auto send = media(Direction::Send, session, owner, plan, output.id),
         receive = media(Direction::Receive, session, owner, plan, output.id);
    require(captureBinding->fillWorkerSource(send.preparation), "source description");
    const auto displayId = uint32_t(send.preparation.source.primary);
    require(screen(displayId), "created source NSScreen missing");
    NSWindow *scene = [[NSWindow alloc] initWithContentRect:screen(displayId).frame
                                                  styleMask:NSWindowStyleMaskBorderless
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO
                                                     screen:screen(displayId)];
    scene.releasedWhenClosed = NO;
    scene.collectionBehavior =
        NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary;
    scene.title = @"BMouse native output test scene";
    BMouseLabScene *sceneView = [[BMouseLabScene alloc] initWithFrame:NSMakeRect(0, 0, 1920, 1080)];
    scene.contentView = sceneView;
    [scene setFrame:screen(displayId).frame display:YES];
    [scene orderFrontRegardless];
    Presenter presenter;
    presenter.show();
    presenter.raise();
    [NSApp activateIgnoringOtherApps:YES];
    [native(presenter) makeKeyAndOrderFront:nil];
    [scene orderFrontRegardless];
    NSScreen *main = screen(CGMainDisplayID());
    require(main && CGMainDisplayID() != displayId, "separate receiver display required");
    [native(presenter) setFrame:NSMakeRect(main.frame.origin.x + 60, main.frame.origin.y + 60, 960, 540) display:YES];
    require(
        until(
            [&] {
              return screenId(scene) == displayId && screenId(native(presenter)) != displayId &&
                     presenter.windowHandle()->isExposed();
            },
            2000
        ),
        "native source/presenter targets not confirmed"
    );
    const auto previousBinding = resources.snapshot().bindingVersion;
    require(resources.refreshBinding(Clock::now() + std::chrono::seconds(3)), "source refresh post");
    require(
        until([&] { return resources.snapshot().bindingVersion > previousBinding; }, 4500), "source refresh deadline"
    );
    captureBinding = resources.snapshot().binding;
    require(bool(captureBinding) && captureBinding->fillWorkerSource(send.preparation), "final source binding");
    send.streamTopologyGeneration = send.preparation.sourceTopologyGeneration;
    receive.streamTopologyGeneration = send.streamTopologyGeneration;
    send.preparation.startupDeadlineNanoseconds = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            (Clock::now() + std::chrono::seconds(10)).time_since_epoch()
        )
            .count()
    );
    receive.preparation.startupDeadlineNanoseconds = send.preparation.startupDeadlineNanoseconds;
    LabControlLoopback control(send, receive);
    if (isolatedSender) {
      require(captureBinding->claimWorkerSource(send.preparation), "worker source claim");
      require(!captureBinding->fillWorkerSource(receive.preparation), "claimed source reused");
      NativeWebRtcEngine rx;
      require(rx.prepare(receive) && rx.start(), "worker receive prepare/start");
      const auto senderPath = senderArgument.mid(QString("--sender-worker=").size());
      QFile image(senderPath);
      require(image.open(QIODevice::ReadOnly), "sender executable");
      WorkerRegistration mediaRegistration{
          QFileInfo(image).canonicalFilePath(),
          QString::fromLatin1(QCryptographicHash::hash(image.readAll(), QCryptographicHash::Sha256).toHex()),
          send.binding.backend, send.binding.profile
      };
      WorkerProcess sender;
      bool started = false, failed = false, quiesced = false, exited = false;
      QString senderError;
      sender.onEstablished = [&] {
        if (!sender.send(send.preparation) || !sender.send(WorkerSendBinding{send.localAddress}))
          failed = true;
      };
      sender.onMessage = [&](const WorkerMessage &message) {
        if (std::holds_alternative<WorkerReady>(message)) {
          started = sender.send(WorkerStart{});
          if (!started)
            failed = true;
        }
        if (auto failure = std::get_if<WorkerFailure>(&message)) {
          failed = true;
          senderError = failure->code;
        }
        if (std::holds_alternative<WorkerQuiesced>(message))
          quiesced = true;
        control.fromSenderWorker(message, rx);
      };
      sender.onExited = [&](int code, QProcess::ExitStatus status) {
        exited = true;
        if (code != 0 || status != QProcess::NormalExit)
          failed = true;
      };
      mediaQuiesced->store(false);
      require(sender.launch(mediaRegistration, send.binding), "sender worker launch");
      QElapsedTimer run;
      run.start();
      qint64 next = 0;
      uint64_t decoded = 0, markers = 0;
      while (!failed && run.elapsed() < 10000) {
        QCoreApplication::processEvents();
        rx.poll();
        if (started)
          control.toSenderWorker(rx, sender);
        if (run.elapsed() >= next) {
          next = run.elapsed() + 33;
          ++sceneView.frameNumber;
          [sceneView setNeedsDisplay:YES];
          [scene displayIfNeeded];
        }
        if (auto frame = rx.takeDecoded()) {
          const auto &d = frame->descriptor();
          require(
              d.sourceResource == output.id.toStdString() && d.captureClockId == "remote.macos.cm-host-time",
              "worker decoded source identity"
          );
          const auto *pixel = static_cast<const unsigned char *>(frame->data()) + 200 * d.planes[0].stride + 200 * 4;
          if (pixel[1] > 100 && pixel[1] > pixel[0] + 40 && pixel[1] > pixel[2] + 40)
            ++markers;
          presenter.frame = std::move(frame);
          presenter.update();
          ++decoded;
        }
        if (!rx.error().isEmpty()) {
          failed = true;
          senderError = rx.error();
        }
        QThread::msleep(2);
      }
      const auto paint = presenter.presented;
      const bool visible = onScreen(native(presenter));
      sender.stop(StopReason::Revoked);
      require(until([&] { return exited; }, 6500), "sender exit deadline");
      presenter.frame.reset();
      presenter.hide();
      receive.permit->revoke();
      rx.stop();
      require(until([&] { return rx.stopped(); }, 5000), "worker receiver drain");
      [scene orderOut:nil];
      [scene close];
      mediaQuiesced->store(true);
      require(resources.cleanup(Clock::now() + std::chrono::seconds(5)), "worker resource cleanup");
      require(
          until([&] { return resources.snapshot().cleanupDone; }, 7500) && resources.wait(2000),
          "worker resource cleanup deadline"
      );
      const auto after = macDisplayInventory();
      const bool absent =
          after.complete && !macResourceHintPresent(after, owner, output.id) && resources.snapshot().ownerClosed;
      QJsonObject report{
          {"scope", "real Mac output -> isolated capture/encode worker -> local receiver; diagnostic authentication"},
          {"decodedFrames", QString::number(decoded)},
          {"markerFrames", QString::number(markers)},
          {"nativePaintFrames", QString::number(paint)},
          {"windowVisible", visible},
          {"senderQuiesced", quiesced},
          {"senderExited", exited},
          {"senderError", senderError + sender.error()},
          {"outputAbsent", absent},
          {"ownerClosed", resources.snapshot().ownerClosed},
          {"displaysBefore", int(before.displays.size())},
          {"displaysAfter", int(after.displays.size())}
      };
      std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
      require(
          !failed && decoded >= 10 && markers >= 5 && paint >= 5 && visible && quiesced && absent,
          "isolated sender evidence incomplete"
      );
      audit.setAutoRemove(true);
      return 0;
    }
    require(!captureBinding->withMediaPermit(receive.permit), "receiver permit authorized capture");
    auto wrongContext = static_cast<const MediaWorkContext &>(send);
    wrongContext.binding.token.owner = std::string(32, '9');
    auto wrongPermit = MediaWorkPermit::issue(wrongContext, Clock::now() + std::chrono::seconds(30));
    require(bool(wrongPermit) && !captureBinding->withMediaPermit(wrongPermit), "foreign owner authorized capture");
    auto revokedPermit = MediaWorkPermit::issue(send, Clock::now() + std::chrono::seconds(30));
    require(bool(revokedPermit), "revocation fixture permit");
    revokedPermit->revoke();
    require(!captureBinding->withMediaPermit(revokedPermit), "revoked permit authorized capture");
    auto scopedBinding = captureBinding->withMediaPermit(send.permit);
    require(bool(scopedBinding), "capture media scope rejected");
    MacCaptureSource capture;
    mediaQuiesced->store(false, std::memory_order_release);
    if (!capture.start(*scopedBinding, Clock::now() + std::chrono::seconds(8)))
      throw std::runtime_error(capture.error().toStdString());
    NativeWebRtcEngine tx, rx;
    if (!tx.prepare(send) || !rx.prepare(receive))
      throw std::runtime_error((tx.error() + rx.error()).toStdString());
    require(rx.start() && tx.start(), "start native media");
    QElapsedTimer run;
    run.start();
    qint64 next = 0;
    std::shared_ptr<CpuFrameLease> heldCapture;
    uint64_t decoded = 0, markers = 0;
    uint32_t lastPixel = 0, rawPixel = 0;
    uint64_t rawMarkers = 0;
    while (run.elapsed() < 8000) {
      QCoreApplication::processEvents();
      capture.poll();
      tx.poll();
      rx.poll();
      if (run.elapsed() >= next) {
        next = run.elapsed() + 33;
        ++sceneView.frameNumber;
        [sceneView setNeedsDisplay:YES];
        [scene displayIfNeeded];
      }
      control.pump(tx, rx);
      if (auto frame = capture.takeCpuLease()) {
        const auto *raw =
            static_cast<const unsigned char *>(frame->data()) + 200 * frame->descriptor().planes[0].stride + 200 * 4;
        std::memcpy(&rawPixel, raw, 4);
        if (raw[1] > 100 && raw[1] > raw[0] + 40 && raw[1] > raw[2] + 40)
          ++rawMarkers;
        heldCapture = frame;
        if (!tx.submit(std::move(frame)))
          throw std::runtime_error(tx.error().toStdString());
      }
      if (auto frame = rx.takeDecoded()) {
        const auto &d = frame->descriptor();
        require(
            d.sourceResource == output.id.toStdString() && d.captureClockId == "remote.macos.cm-host-time" &&
                d.captureSequence > 0,
            "decoded capture identity/time missing"
        );
        const auto *pixel = static_cast<const unsigned char *>(frame->data()) + 200 * d.planes[0].stride + 200 * 4;
        std::memcpy(&lastPixel, pixel, 4);
        if (pixel[1] > 100 && pixel[1] > pixel[0] + 40 && pixel[1] > pixel[2] + 40)
          ++markers;
        presenter.frame = std::move(frame);
        presenter.update();
        ++decoded;
      }
      if (!capture.error().isEmpty() || !tx.error().isEmpty() || !rx.error().isEmpty()) {
        std::cerr << "captureBeforeFailure rawMarkers=" << rawMarkers << " rawPixel=" << rawPixel
                  << " decoded=" << decoded << " markerPixel=" << lastPixel << '\n';
        throw std::runtime_error((capture.error() + tx.error() + rx.error()).toStdString());
      }
      QThread::msleep(2);
    }
    const bool windowOnScreen = onScreen(native(presenter));
    const auto stats = tx.statistics();
    const auto captured = capture.statistics();
    std::cerr << "nativeEvidence captured=" << captured.published << " decoded=" << decoded << " markers=" << markers
              << " rawMarkers=" << rawMarkers << " rawPixel=" << rawPixel << " markerPixel=" << lastPixel
              << " paint=" << presenter.presented << " onScreen=" << windowOnScreen
              << " profile=" << stats.constrainedBaseline << " pair=" << stats.addressPairApproved
              << " sceneVisible=" << scene.visible << " sceneScreen=" << screenId(scene) << '\n';
    require(
        control.serializedCorrelations() >= decoded && decoded >= 10 && markers >= 5 && presenter.presented >= 5 &&
            windowOnScreen && stats.constrainedBaseline && stats.addressPairApproved,
        "native display evidence incomplete"
    );
    require(bool(heldCapture), "missing capture lease for revoke test");
    send.permit->revoke();
    require(!capture.takeCpuLease() && !capture.readNewest(), "revoked capture exposed frame");
    capture.poll();
    require(capture.error() == "CaptureMediaPermitInactive" && !capture.stopped(), "capture revocation failed");
    heldCapture.reset();
    receive.permit->revoke();
    capture.stop();
    tx.stop();
    rx.stop();
    presenter.frame.reset();
    presenter.repaint();
    [scene orderOut:nil];
    [scene close];
    presenter.hide();
    require(
        until([&] { return capture.stopped() && tx.stopped() && rx.stopped(); }, 5000),
        "native media quiescence unconfirmed"
    );
    mediaQuiesced->store(true, std::memory_order_release);
    require(resources.cleanup(Clock::now() + std::chrono::seconds(5)), "resource cleanup post");
    require(until([&] { return resources.snapshot().cleanupDone; }, 7500), "resource cleanup deadline");
    require(
        resources.snapshot().cleanup.outcome == ResourceOutcome::Closed && resources.snapshot().ownerClosed,
        "native output owner cleanup"
    );
    require(resources.wait(2000), "resource thread finalization");
    const auto after = macDisplayInventory();
    require(
        after.complete && !macResourceHintPresent(after, owner, output.id) && resources.snapshot().ownerClosed,
        "native output absence unconfirmed"
    );
    QJsonObject report{
        {"scope", "real virtual output/capture/standard WebRTC/decoded marker/native paint; local diagnostic, no "
                  "product authorization or cross-machine certification"},
        {"resourceThreadIsolated", resourceState.isolated},
        {"receiverCompatibleProfileLabels", QString::number(rx.statistics().compatibleProfileLabels)},
        {"guiTicksDuringCreate", QString::number(guiTicksDuringCreate)},
        {"serializedCorrelations", QString::number(control.serializedCorrelations())},
        {"capturePermitRevocationVerified", true},
        {"capturedFrames", QString::number(captured.published)},
        {"decodedFrames", QString::number(decoded)},
        {"markerFrames", QString::number(markers)},
        {"nativePaintFrames", QString::number(presenter.presented)},
        {"receiverWindowOnScreen", windowOnScreen},
        {"presentationEvidence", "NativePaintConfirmed"},
        {"constrainedBaseline", stats.constrainedBaseline},
        {"gccUpdates", QString::number(stats.gccUpdates)},
        {"encoderUpdates", QString::number(stats.encoderUpdates)},
        {"sourceCpuBytesAfterStop", QString::number(capture.usage().bytes)},
        {"displaysBefore", int(before.displays.size())},
        {"displaysAfter", int(after.displays.size())},
        {"outputAbsenceVerified", true},
        {"ownerClosed", true}
    };
    std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
    audit.setAutoRemove(true);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
