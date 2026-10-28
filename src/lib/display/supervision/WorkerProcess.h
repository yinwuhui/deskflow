// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "NativeProcessIdentity.h"
#include "WorkerProtocol.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <memory>

namespace deskflow::display::infra {
struct WorkerRegistration
{
  QString executable, sha256, backend, profile;
  WorkerPurpose purpose = WorkerPurpose::Media;
};
struct WorkerBootstrap
{
  WorkerBinding binding;
  QString endpoint;
  uint64_t parentPid = 0;
  QByteArray secret;
};
bool validWorkerExecutable(const WorkerRegistration &);
// Configure, then start this process without changing its environment/arguments.
// Linux executes the validated descriptor. Other platforms retain path execution.
bool prepareWorkerProcess(QProcess &, const WorkerRegistration &, const QStringList &arguments);
// Only read from inherited stdin, never from a network message or arbitrary file.
std::optional<WorkerBootstrap> readWorkerBootstrap(QString &error);

class WorkerProcess : public QObject
{
public:
  explicit WorkerProcess(QObject *parent = nullptr);
  ~WorkerProcess() override;
  bool launch(const WorkerRegistration &, const WorkerBinding &);
  bool send(const WorkerMessage &);
  void stop(StopReason);
  void terminate();
  bool running() const
  {
    return m_process.state() != QProcess::NotRunning;
  }
  qint64 processId() const
  {
    return m_process.processId();
  }
  std::optional<NativeProcessIdentity> nativeIdentity() const
  {
    return m_nativeIdentity;
  }
  QString error() const
  {
    return m_error;
  }
  std::function<void()> onEstablished;
  std::function<void(const WorkerMessage &)> onMessage;
  // A process exit is evidence of process termination, never resource absence.
  std::function<void(int, QProcess::ExitStatus)> onExited;

private:
  void accept();
  void read();
  void poll();
  void fail(const QString &);
  void terminateOwnedProcess();
  QProcess m_process;
  std::optional<NativeProcessIdentity> m_nativeIdentity;
  QLocalServer m_server;
  QLocalSocket *m_socket = nullptr;
  QTemporaryDir m_directory;
  QTimer m_timer;
  std::unique_ptr<WorkerProtocol> m_protocol;
  QString m_error;
  QByteArray m_bootstrap;
  TimePoint m_stopDeadline{};
  bool m_launched = false, m_established = false, m_expectedExit = false, m_exitNotified = false;
#ifdef Q_OS_WIN
  void *m_job = nullptr;
#endif
};

// Native worker-side connection; EOF/lease failure calls onShutdown exactly once.
// Backend must stop capture/network and release native objects on this callback.
class WorkerConnection : public QObject
{
public:
  explicit WorkerConnection(WorkerBootstrap, QObject *parent = nullptr);
  ~WorkerConnection() override;
  void connectToAgent();
  void disconnectAfterWrites();
  bool send(const WorkerMessage &);
  QString error() const
  {
    return m_error;
  }
  std::function<void()> onEstablished;
  std::function<void(const WorkerMessage &)> onMessage;
  std::function<void()> onShutdown;

private:
  void read();
  void poll();
  void fail(const QString &);
  QLocalSocket m_socket;
  QTimer m_timer;
  std::unique_ptr<WorkerProtocol> m_protocol;
  QString m_endpoint, m_error;
  uint64_t m_parentPid;
  bool m_connected = false, m_established = false, m_shuttingDown = false;
};
} // namespace deskflow::display::infra
