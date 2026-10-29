// SPDX-License-Identifier: MIT
#include "NativeMediaThread.h"
#include "display/supervision/WorkerProcess.h"
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <atomic>
using namespace deskflow::display::infra;

// This window is created locally by the receiver. No peer-selected native handle
// or physical display can be imported or destroyed through this worker.
class ReceiverWindow final : public QWidget
{
public:
  std::shared_ptr<CpuFrameLease> frame;
  std::function<void(uint64_t)> painted;
  uint64_t last = 0;
  bool diagnosticMarker = false;
  void paintEvent(QPaintEvent *) override
  {
    auto lease = frame;
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (!lease)
      return;
    const auto &d = lease->descriptor();
    // QImage copies retain the CPU lease through its cleanup context.
    QImage image(
        static_cast<const uchar *>(lease->data()), d.width, d.height, d.planes[0].stride, QImage::Format_ARGB32,
        [](void *p) { delete static_cast<std::shared_ptr<CpuFrameLease> *>(p); },
        new std::shared_ptr<CpuFrameLease>(lease)
    );
    painter.drawImage(rect(), image);
    painter.end();
    if (windowHandle() && windowHandle()->isExposed() && d.sequence != last) {
      last = d.sequence;
      // This named developer profile uses a known green scene. Only report
      // diagnostic paint evidence after checking actual decoded pixel content.
      if (diagnosticMarker) {
        const auto *pixel = static_cast<const unsigned char *>(lease->data()) + 200 * d.planes[0].stride + 200 * 4;
        if (!(pixel[1] > 100 && pixel[1] > pixel[0] + 40 && pixel[1] > pixel[2] + 40))
          return;
      }
      if (painted)
        painted(last);
    }
  }
};
int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  QString error;
  auto bootstrap = readWorkerBootstrap(error);
  if (!bootstrap || bootstrap->binding.direction != Direction::Receive ||
      bootstrap->binding.purpose != WorkerPurpose::Media || bootstrap->binding.backend != "webrtc")
    return 2;
  const auto binding = bootstrap->binding;
  WorkerConnection connection(std::move(*bootstrap));
  NativeMediaThread media;
  NativeWebRtcConfiguration configuration;
  configuration.binding = binding;
  ReceiverWindow window;
  window.diagnosticMarker = binding.profile == "diagnostic.webrtc.h264.control-map.v1";
  window.setWindowTitle("BMouse extended display receiver");
  window.resize(960, 540);
  bool stopping = false, started = false, configured = false, notify = false, disconnected = false, draining = false,
       mediaDrained = false;
  auto stop = [&] {
    if (stopping)
      return;
    stopping = true;
    if (configuration.permit)
      configuration.permit->revoke();
    window.hide();
    window.frame.reset();
    media.stop();
  };
  auto failure = [&](const QString &code) {
    if (stopping)
      return; // Native failures queued before revoke may arrive during cleanup.
    connection.send(WorkerFailure{code});
    notify = true;
    stop();
  };
  connection.onShutdown = [&] {
    disconnected = true;
    stop();
  };
  window.painted = [&](uint64_t frame) {
    if (!stopping && !connection.send(WorkerPresented{frame, PresentationEvidence::NativePaintConfirmed, true}))
      stop();
  };
  connection.onMessage = [&](const WorkerMessage &message) {
    if (std::holds_alternative<WorkerStop>(message)) {
      notify = true;
      stop();
      return;
    }
    if (stopping)
      return;
    if (const auto *p = std::get_if<WorkerPrepare>(&message))
      configuration.preparation = *p;
    if (const auto *target = std::get_if<WorkerReceiveBinding>(&message)) {
      configuration.localAddress = target->localAddress;
      configuration.streamSourceResource = target->sourceResource;
      configuration.streamTopologyGeneration = target->topologyGeneration;
      // Only the authenticated local supervisor may submit this binding after
      // authorization. IPC proof is not itself proof of remote user consent.
      configuration.permit = MediaWorkPermit::issue(configuration, Clock::now() + std::chrono::hours(24));
      if (!media.prepare(configuration)) {
        failure("ReceiverPrepareFailed");
        return;
      }
      configured = true;
    }
    if (std::holds_alternative<WorkerStart>(message)) {
      if (!configured || !media.start()) {
        failure("ReceiverStartFailed");
        return;
      }
      started = true;
      window.show();
    }
    if (const auto *signal = std::get_if<WorkerMediaSignal>(&message))
      if (!media.apply(*signal))
        failure("ReceiverSignalFailed");
    if (const auto *map = std::get_if<WebRtcFrameCorrelation>(&message))
      if (!media.apply(*map))
        failure("ReceiverCorrelationFailed");
    if (std::holds_alternative<WorkerRefresh>(message))
      if (!media.refresh())
        failure("ReceiverRefreshFailed");
  };
  media.ready = [&] {
    if (!stopping && !connection.send(WorkerReady{}))
      stop();
  };
  media.failed = failure;
  media.drained = [&] { mediaDrained = true; };
  media.signal = [&](const WorkerMediaSignal &value) {
    if (!stopping && !connection.send(value))
      stop();
  };
  media.decoded = [&](std::shared_ptr<CpuFrameLease> frame) {
    if (!stopping) {
      window.frame = std::move(frame);
      window.update();
    }
  };
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, [&] {
    if (stopping) {
      if (mediaDrained) {
        if (disconnected) {
          app.quit();
          return;
        }
        if (!draining) {
          draining = true;
          if (notify && !connection.send(WorkerQuiesced{})) {
            app.quit();
            return;
          }
          connection.disconnectAfterWrites();
        }
      }
      return;
    }
    if (configured && !started &&
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()) >=
            configuration.preparation.startupDeadlineNanoseconds) {
      failure("ReceiverStartupDeadline");
      return;
    }
    if (!started)
      return;
    if (!window.isVisible()) {
      failure("ReceiverTargetClosed");
      return;
    }
  });
  timer.start(5);
  connection.connectToAgent();
  return app.exec();
}
