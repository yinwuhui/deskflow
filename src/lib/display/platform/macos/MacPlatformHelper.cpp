// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "MacVirtualOutput.h"
#include "display/supervision/WorkerProcess.h"
#include <QCoreApplication>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <cstdlib>
#include <mutex>
using namespace deskflow::display::infra;
namespace {
struct State
{
  std::mutex mutex;
  std::optional<WorkerPrepareOutput> prepare;
  std::optional<WorkerOutputObserved> observed;
  std::optional<WorkerFailure> failure;
  std::atomic<bool> revoked{false}, quiesced{false};
  QString resourceId;
  bool start = false, ready = false, close = false, configure = false, outputReady = false;
};
} // namespace
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  if (argc != 2 || QString(argv[1]) != "--display-worker")
    return 2;
  QString error;
  auto boot = readWorkerBootstrap(error);
  if (!boot || boot->binding.purpose != WorkerPurpose::Platform ||
      boot->binding.backend != "platform.macos.virtual-output" ||
      boot->binding.profile != "macos.virtual-output.cgdisplay.sdr1080.v1" ||
      boot->binding.localUserSession != macUserSessionReference())
    return 3;
  const auto owner = QString::fromStdString(boot->binding.token.owner);
  auto state = std::make_shared<State>();
  // Qt control I/O lives independently of the main-thread native API. A blocked
  // WindowServer call cannot keep an orphan alive after parent EOF/lease expiry.
  QThread io;
  auto *connection = new WorkerConnection(std::move(*boot));
  connection->onShutdown = [state] { std::_Exit(state->quiesced.load(std::memory_order_acquire) ? 0 : 74); };
  connection->onMessage = [state](const WorkerMessage &message) {
    if (const auto *p = std::get_if<WorkerPrepareOutput>(&message)) {
      std::lock_guard lock(state->mutex);
      state->prepare = *p;
      state->resourceId = p->resourceId;
    } else if (std::holds_alternative<WorkerStart>(message)) {
      std::lock_guard lock(state->mutex);
      state->start = true;
    } else if (std::holds_alternative<WorkerConfigureOutput>(message)) {
      std::lock_guard lock(state->mutex);
      state->configure = true;
    } else if (std::holds_alternative<WorkerStop>(message))
      state->revoked.store(true, std::memory_order_release);
    else {
      std::lock_guard lock(state->mutex);
      state->failure = WorkerFailure{"UnsupportedPlatformCommand"};
      state->revoked.store(true, std::memory_order_release);
    }
  };
  auto *responses = new QTimer(connection);
  responses->setInterval(10);
  QObject::connect(responses, &QTimer::timeout, connection, [state, connection] {
    std::optional<WorkerFailure> failure;
    std::optional<WorkerOutputObserved> observed;
    bool ready = false, close = false, outputReady = false;
    {
      std::lock_guard lock(state->mutex);
      failure = std::move(state->failure);
      state->failure.reset();
      observed = std::move(state->observed);
      state->observed.reset();
      ready = state->ready;
      state->ready = false;
      close = state->close;
      state->close = false;
      outputReady = state->outputReady;
      state->outputReady = false;
    }
    if (failure)
      connection->send(*failure);
    if (ready && !state->revoked.load(std::memory_order_acquire))
      connection->send(WorkerReady{});
    if (observed)
      connection->send(*observed); // A late claim is delivered for owner cleanup.
    if (outputReady && !state->revoked.load(std::memory_order_acquire)) {
      QString id;
      {
        std::lock_guard lock(state->mutex);
        id = state->resourceId;
      }
      connection->send(WorkerOutputReady{id});
    }
    if (close) {
      connection->send(WorkerQuiesced{});
      connection->disconnectAfterWrites();
    }
  });
  connection->moveToThread(&io);
  QObject::connect(&io, &QThread::started, connection, [connection, responses] {
    responses->start();
    connection->connectToAgent();
  });
  io.start();
  MacVirtualOutput output;
  QTimer native;
  native.setInterval(10);
  native.setTimerType(Qt::PreciseTimer);
  bool prepared = false, started = false, reported = false, stopping = false, configureRequested = false,
       layoutReady = false;
  TimePoint preparationDeadline{}, stopDeadline{}, nextHealth{};
  WorkerPrepareOutput preparation;
  auto fail = [&](const QString &code) {
    std::lock_guard lock(state->mutex);
    if (!state->failure)
      state->failure = WorkerFailure{code};
    state->revoked.store(true, std::memory_order_release);
  };
  QObject::connect(&native, &QTimer::timeout, &app, [&] {
    const auto now = Clock::now();
    {
      std::lock_guard lock(state->mutex);
      if (state->prepare) {
        preparation = *state->prepare;
        state->prepare.reset();
        prepared = true;
        preparationDeadline = TimePoint(
            std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(preparation.startupDeadlineNanoseconds)
            )
        );
        if (output.supported())
          state->ready = true;
        else {
          state->failure = WorkerFailure{"NativeOutputNotSupported"};
          state->revoked.store(true, std::memory_order_release);
        }
      }
      if (state->configure) {
        configureRequested = true;
        state->configure = false;
      }
      if (state->start && !started) {
        state->start = false;
        started = true;
      }
    }
    if (state->revoked.load(std::memory_order_acquire)) {
      if (!stopping) {
        stopping = true;
        output.destroy();
        stopDeadline = now + std::chrono::seconds(3);
      }
      if (output.released()) {
        native.stop();
        state->quiesced.store(true, std::memory_order_release);
        std::lock_guard lock(state->mutex);
        state->close = true;
      } else if (now >= stopDeadline)
        std::_Exit(75);
      return;
    }
    if (prepared && now >= preparationDeadline && !layoutReady) {
      fail("NativeOutputStartupDeadline");
      return;
    }
    if (started && !reported) {
      // create() is issued once; polling does not issue additional native creates.
      static bool issued = false;
      if (!issued) {
        issued = true;
        QString reason;
        if (!output.create(preparation, owner, reason)) {
          fail(reason);
          return;
        }
      }
      const auto observed = output.observation();
      if (observed) {
        reported = true;
        std::lock_guard lock(state->mutex);
        state->observed = *observed;
        nextHealth = now + std::chrono::milliseconds(250);
      }
    }
    if (reported && configureRequested && !layoutReady && output.configureLayout()) {
      layoutReady = true;
      nextHealth = now + std::chrono::milliseconds(250);
      std::lock_guard lock(state->mutex);
      state->outputReady = true;
    }
    if (layoutReady && now >= nextHealth) {
      nextHealth = now + std::chrono::milliseconds(250);
      const auto desktop = macDesktopState();
      if (!desktop.onConsole || !desktop.lockKnown || desktop.locked || !output.healthy())
        fail("NativeOutputSessionLost");
    }
  });
  native.start();
  const auto result = app.exec();
  // Normally the I/O thread exits the one-session process after flushing Quiesced.
  state->revoked.store(true);
  output.destroy();
  io.quit();
  io.wait(1000);
  return result;
}
