// SPDX-License-Identifier: MIT
#include "NativeMediaThread.h"
#include "display/platform/macos/MacCaptureSource.h"
#include "display/supervision/WorkerProcess.h"
#include <QApplication>
using namespace deskflow::display::infra;
int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  QString error;
  auto bootstrap = readWorkerBootstrap(error);
  if (!bootstrap || bootstrap->binding.direction != Direction::Send || bootstrap->binding.backend != "webrtc" ||
      bootstrap->binding.purpose != WorkerPurpose::Media)
    return 2;
  NativeWebRtcConfiguration config;
  config.binding = bootstrap->binding;
  WorkerConnection connection(std::move(*bootstrap));
  MacCaptureSource capture;
  NativeMediaThread media;
  std::optional<WorkerSourceLease> source;
  bool stopping = false, started = false, configured = false, mediaDone = false, disconnected = false, notify = false,
       draining = false;
  auto stop = [&] {
    if (stopping)
      return;
    stopping = true;
    if (config.permit)
      config.permit->revoke();
    capture.stop();
    media.stop();
  };
  auto failure = [&](const QString &code) {
    if (stopping)
      return;
    connection.send(WorkerFailure{code});
    notify = true;
    stop();
  };
  connection.onShutdown = [&] {
    disconnected = true;
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
    if (auto preparation = std::get_if<WorkerPrepare>(&message)) {
      config.preparation = *preparation;
      source = connection.takeSourceLease();
    }
    if (auto endpoint = std::get_if<WorkerSendBinding>(&message)) {
      config.localAddress = endpoint->localAddress;
      config.streamSourceResource = config.preparation.sourceResource;
      config.streamTopologyGeneration = config.preparation.sourceTopologyGeneration;
      config.permit = MediaWorkPermit::issue(config, Clock::now() + std::chrono::hours(24));
      if (!source || !source->active() || !config.permit || !media.prepare(config)) {
        failure("SenderPrepareFailed");
        return;
      }
      configured = true;
    }
    if (std::holds_alternative<WorkerStart>(message)) {
      const auto deadline = TimePoint(
          std::chrono::duration_cast<Clock::duration>(
              std::chrono::nanoseconds(config.preparation.startupDeadlineNanoseconds)
          )
      );
      if (!configured || !source || !capture.start(*source, config.permit, deadline) || !media.start()) {
        failure("SenderSourceStartFailed");
        return;
      }
      started = true;
    }
    if (auto signal = std::get_if<WorkerMediaSignal>(&message))
      if (!media.apply(*signal))
        failure("SenderSignalQueueFailed");
    if (std::holds_alternative<WorkerRefresh>(message))
      if (!media.refresh())
        failure("SenderRefreshQueueFailed");
  };
  media.ready = [&] {
    if (!stopping && !connection.send(WorkerReady{}))
      stop();
  };
  media.failed = failure;
  media.drained = [&] { mediaDone = true; };
  media.signal = [&](const WorkerMediaSignal &signal) {
    if (!stopping && !connection.send(signal))
      stop();
  };
  media.correlation = [&](const WebRtcFrameCorrelation &map) {
    if (!stopping && !connection.send(map))
      stop();
  };
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, [&] {
    if (stopping) {
      capture.poll();
      if (mediaDone && capture.stopped()) {
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
    if (!started)
      return;
    capture.poll();
    if (!capture.error().isEmpty()) {
      failure("SenderCaptureFailed");
      return;
    }
    if (auto frame = capture.takeCpuLease())
      if (!media.submit(std::move(frame)))
        failure("SenderInputQueueFailed");
  });
  timer.start(5);
  connection.connectToAgent();
  return app.exec();
}
