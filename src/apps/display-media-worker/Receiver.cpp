// SPDX-License-Identifier: MIT
#include "display/media/gstreamer/NativeWebRtcEngine.h"
#include "display/supervision/WorkerProcess.h"
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QTimer>
#include <QWidget>
#include <QWindow>
using namespace deskflow::display::infra;

// This window is created locally by the receiver. No peer-selected native handle
// or physical display can be imported or destroyed through this worker.
class ReceiverWindow final : public QWidget
{
public:
  std::shared_ptr<CpuFrameLease> frame;
  std::function<void(uint64_t)> painted;
  uint64_t last = 0;
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
  NativeWebRtcEngine engine;
  NativeWebRtcConfiguration configuration;
  configuration.binding = binding;
  ReceiverWindow window;
  window.setWindowTitle("BMouse extended display receiver");
  window.resize(960, 540);
  bool stopping = false, started = false, configured = false, notify = false, disconnected = false, draining = false;
  auto stop = [&] {
    if (stopping)
      return;
    stopping = true;
    if (configuration.permit)
      configuration.permit->revoke();
    window.hide();
    window.frame.reset();
    engine.stop();
  };
  auto failure = [&](const QString &code) {
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
      if (!NativeWebRtcEngine::initializeRuntime(QStringLiteral(DISPLAY_PRIVATE_RUNTIME), error) ||
          !engine.prepare(configuration)) {
        failure("ReceiverPrepareFailed");
        return;
      }
      configured = true;
      if (!connection.send(WorkerReady{}))
        stop();
    }
    if (std::holds_alternative<WorkerStart>(message)) {
      if (!configured || !engine.start()) {
        failure("ReceiverStartFailed");
        return;
      }
      started = true;
      window.show();
    }
    if (const auto *signal = std::get_if<WorkerMediaSignal>(&message))
      if (!engine.applySignal(*signal))
        failure("ReceiverSignalFailed");
    if (const auto *map = std::get_if<WebRtcFrameCorrelation>(&message))
      if (!engine.applyCorrelation(*map))
        failure("ReceiverCorrelationFailed");
    if (std::holds_alternative<WorkerRefresh>(message))
      if (!engine.refresh())
        failure("ReceiverRefreshFailed");
  };
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, [&] {
    if (stopping) {
      if (engine.stopped()) {
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
    engine.poll();
    for (const auto &signal : engine.takeSignals())
      if (!connection.send(signal)) {
        stop();
        return;
      }
    if (auto frame = engine.takeDecoded()) {
      window.frame = std::move(frame);
      window.update();
    }
    if (!engine.error().isEmpty())
      failure("ReceiverMediaFailed");
  });
  timer.start(5);
  connection.connectToAgent();
  return app.exec();
}
