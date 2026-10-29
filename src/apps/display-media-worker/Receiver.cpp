// SPDX-License-Identifier: MIT
#include "display/media/gstreamer/NativeWebRtcEngine.h"
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
// Media library calls must never block the IPC lease or the GUI event loop.
// Commands have a hard queue limit; decoded delivery retains at most one queued
// lease in addition to the window's current frame.
class ReceiverMedia final : public QObject
{
public:
  std::function<void()> ready, drained;
  std::function<void(const QString &)> failed;
  std::function<void(const WorkerMediaSignal &)> signal;
  std::function<void(std::shared_ptr<CpuFrameLease>)> decoded;
  ReceiverMedia()
  {
    executor = new QObject;
    executor->moveToThread(&thread);
    connect(&thread, &QThread::finished, executor, &QObject::deleteLater);
    thread.start();
    QMetaObject::invokeMethod(executor, [this] {
      engine = std::make_unique<NativeWebRtcEngine>();
      timer = new QTimer(executor);
      connect(timer, &QTimer::timeout, executor, [this] { poll(); });
      timer->start(5);
    }, Qt::QueuedConnection);
  }
  ~ReceiverMedia()
  {
    thread.quit();
    thread.wait();
  }
  bool prepare(NativeWebRtcConfiguration config)
  {
    return post([this, config] {
      QString error;
      if (stopping.load())
        return;
      if (!NativeWebRtcEngine::initializeRuntime(QStringLiteral(DISPLAY_PRIVATE_RUNTIME), error) || stopping.load() ||
          !engine->prepare(config)) {
        if (!stopping.load())
          fail("ReceiverPrepareFailed");
        return;
      }
      deliver([this] {
        if (ready)
          ready();
      });
    });
  }
  bool start()
  {
    return post([this] {
      if (!engine->start())
        fail("ReceiverStartFailed");
      else
        started = true;
    });
  }
  bool apply(WorkerMediaSignal value)
  {
    return post([this, value] {
      if (!engine->applySignal(value))
        fail("ReceiverSignalFailed");
    });
  }
  bool apply(WebRtcFrameCorrelation value)
  {
    return post([this, value] {
      if (!engine->applyCorrelation(value))
        fail("ReceiverCorrelationFailed");
    });
  }
  bool refresh()
  {
    return post([this] {
      if (!engine->refresh())
        fail("ReceiverRefreshFailed");
    });
  }
  void stop()
  {
    stopping.store(true);
  }

private:
  void deliver(std::function<void()> work)
  {
    QMetaObject::invokeMethod(this, std::move(work), Qt::QueuedConnection);
  }
  bool post(std::function<void()> work)
  {
    if (pending.fetch_add(1) >= 72) {
      pending.fetch_sub(1);
      return false;
    }
    QMetaObject::invokeMethod(
        executor,
        [this, work = std::move(work)] {
          pending.fetch_sub(1);
          if (!stopping.load())
            work();
        },
        Qt::QueuedConnection
    );
    return true;
  }
  void fail(const QString &code)
  {
    if (stopping.exchange(true))
      return;
    deliver([this, code] {
      if (failed)
        failed(code);
    });
  }
  void poll()
  {
    if (!engine)
      return;
    if (stopping.load()) {
      if (!stopIssued) {
        stopIssued = true;
        engine->stop();
      }
      if (engine->stopped()) {
        timer->stop();
        engine.reset();
        deliver([this] {
          if (drained)
            drained();
        });
      }
      return;
    }
    if (!started)
      return;
    engine->poll();
    for (const auto &value : engine->takeSignals())
      deliver([this, value] {
        if (signal)
          signal(value);
      });
    if (auto frame = engine->takeDecoded()) {
      if (!framePending.exchange(true))
        deliver([this, frame = std::move(frame)] {
          framePending.store(false);
          if (decoded)
            decoded(frame);
        });
    }
    if (!engine->error().isEmpty())
      fail("ReceiverMediaFailed");
  }
  QThread thread;
  QObject *executor = nullptr;
  QTimer *timer = nullptr;
  std::unique_ptr<NativeWebRtcEngine> engine;
  std::atomic<unsigned> pending{0};
  std::atomic<bool> stopping{false}, framePending{false};
  bool started = false, stopIssued = false;
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
  ReceiverMedia media;
  NativeWebRtcConfiguration configuration;
  configuration.binding = binding;
  ReceiverWindow window;
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
    if (stopping) return; // Native failures queued before revoke may arrive during cleanup.
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
