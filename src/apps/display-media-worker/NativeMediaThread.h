// SPDX-License-Identifier: MIT
#pragma once
#include "display/media/gstreamer/NativeWebRtcEngine.h"
#include <QObject>
#include <QThread>
#include <QTimer>
#include <atomic>
using namespace deskflow::display::infra;
// Media library calls must never block the IPC lease or the GUI event loop.
// Commands have a hard queue limit; decoded delivery retains at most one queued
// lease in addition to the window's current frame.
class NativeMediaThread final : public QObject
{
public:
  std::function<void()> ready, drained;
  std::function<void(const QString &)> failed;
  std::function<void(const WorkerMediaSignal &)> signal;
  std::function<void(const WebRtcFrameCorrelation &)> correlation;
  std::function<void(std::shared_ptr<CpuFrameLease>)> decoded;
  NativeMediaThread()
  {
    executor = new QObject;
    executor->moveToThread(&thread);
    connect(&thread, &QThread::finished, executor, &QObject::deleteLater);
    thread.start();
    QMetaObject::invokeMethod(
        executor,
        [this] {
          engine = std::make_unique<NativeWebRtcEngine>();
          timer = new QTimer(executor);
          connect(timer, &QTimer::timeout, executor, [this] { poll(); });
          timer->start(5);
        },
        Qt::QueuedConnection
    );
  }
  ~NativeMediaThread()
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
  bool submit(std::shared_ptr<CpuFrameLease> frame)
  {
    if (inputPending.exchange(true))
      return true; // Drop raw input, never encoded data.
    if (!post([this, frame = std::move(frame)] {
          inputPending.store(false);
          if (!engine->submit(frame))
            fail("NativeSubmitFailed");
        })) {
      inputPending.store(false);
      return false;
    }
    return true;
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
    for (const auto &value : engine->takeCorrelations())
      deliver([this, value] {
        if (correlation)
          correlation(value);
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
      fail("NativeMediaThreadFailed");
  }
  QThread thread;
  QObject *executor = nullptr;
  QTimer *timer = nullptr;
  std::unique_ptr<NativeWebRtcEngine> engine;
  std::atomic<unsigned> pending{0};
  std::atomic<bool> stopping{false}, framePending{false}, inputPending{false};
  bool started = false, stopIssued = false;
};
