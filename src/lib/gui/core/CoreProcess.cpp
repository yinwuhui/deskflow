/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2024 - 2025 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreProcess.h"

#include "common/ExitCodes.h"
#include "gui/TlsUtility.h"
#include "gui/ipc/CoreIpcClient.h"
#include "gui/ipc/DaemonIpcClient.h"

#if defined(Q_OS_MACOS)
#include "OSXHelpers.h"
#endif

#ifdef Q_OS_LINUX
#include <signal.h>
#include <sys/prctl.h>
#endif

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaEnum>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>
#include <memory>

namespace deskflow::gui {

const int kRetryDelay = 1000;
const auto kLineSplitRegex = QRegularExpression("\r|\n|\r\n");

void CoreProcess::validateExternalFile(const QString &path)
{
  const auto generation = ++m_fileValidationGeneration;
  auto directory = std::make_shared<QTemporaryDir>();
  if (!directory->isValid() || !QFileInfo(path).isReadable()) {
    Q_EMIT fileValidationFinished(path, false, tr("文件不可读，请检查路径与权限。"));
    return;
  }
  const auto snapshotPath = directory->filePath("validation.conf");
  QSettings snapshot(snapshotPath, QSettings::IniFormat);
  for (const auto &key : Settings::validKeys())
    snapshot.setValue(key, Settings::value(key));
  auto &profile = Settings::proxy().get();
  for (const auto &key : profile.allKeys())
    snapshot.setValue(key, profile.value(key));
  snapshot.setValue(Settings::Core::CoreMode, QVariant::fromValue(Settings::CoreMode::Server));
  snapshot.setValue(Settings::Server::ExternalConfig, true);
  snapshot.setValue(Settings::Server::ExternalConfigFile, QFileInfo(path).absoluteFilePath());
  snapshot.sync();
  QFile::setPermissions(snapshotPath, QFile::ReadOwner | QFile::WriteOwner);
  if (snapshot.status() != QSettings::NoError) {
    Q_EMIT fileValidationFinished(path, false, tr("无法准备临时检查配置。"));
    return;
  }
  auto *process = new QProcess(this);
  auto *timeout = new QTimer(process);
  timeout->setSingleShot(true);
  auto delivered = std::make_shared<bool>(false);
  auto finish = [this, process, timeout, directory, delivered, path, generation](bool success, const QString &reason) {
    if (*delivered)
      return;
    *delivered = true;
    timeout->stop();
    if (generation == m_fileValidationGeneration)
      Q_EMIT fileValidationFinished(path, success, reason);
    process->deleteLater();
  };
  connect(process, &QProcess::finished, this, [process, finish](int code, QProcess::ExitStatus state) {
    const auto output = process->readAllStandardOutput().trimmed().split('\n');
    const auto result = QJsonDocument::fromJson(output.isEmpty() ? QByteArray() : output.last()).object();
    const bool success = state == QProcess::NormalExit && code == 0 && result.value("success").toBool();
    finish(
        success, result.value("reason").toString(
                     success ? tr("配置文件检查通过；尚未应用到共享。") : tr("核心未能完成配置检查。")
                 )
    );
  });
  connect(process, &QProcess::errorOccurred, this, [process, finish](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart)
      finish(false, tr("无法启动核心检查程序：%1").arg(process->errorString()));
  });
  connect(timeout, &QTimer::timeout, this, [process, finish] {
    process->kill();
    finish(false, tr("配置检查超时，当前共享保持不变。"));
  });
  process->start(m_appPath, {"server", "--validate-config", "--settings", snapshotPath});
  timeout->start(5000);
}

QString CoreProcess::processModeToString(const Settings::ProcessMode mode)
{
  return QVariant::fromValue(mode).toString().toLower();
}

QString CoreProcess::processStateToString(const CoreProcess::ProcessState state)
{
  return QVariant::fromValue(state).toString().toLower();
}

/**
 * @brief Wraps options that contain spaces in quotes
 *
 * Useful to handle things like paths with spaces (e.g. "C:\Program Files").
 *
 * Can also be used to create a representation of a command that can be pasted
 * into a terminal.
 */
QString CoreProcess::makeQuotedArgs(const QString &app, const QStringList &args)
{
  QStringList command = {app};
  command.append(args);

  static const auto quote = QStringLiteral("\"");
  static const auto space = QStringLiteral(" ");
  QStringList quoted;
  for (const auto &item : std::as_const(command)) {
    auto temp = item.simplified();
    if (const auto wrapped = (temp.startsWith(quote) && temp.endsWith(quote)); temp.contains(space) && !wrapped) {
      temp = QStringLiteral("%1%2%1").arg(quote, temp);
    }
    quoted.append(temp);
  }

  return quoted.join(space);
}

/**
 * @brief If IPv6, ensures the IP is surround in square brackets.
 */
QString CoreProcess::wrapIpv6(const QString &address)
{
  static const auto colon = QStringLiteral(":");
  static const auto openBracket = QStringLiteral("[");
  static const auto closeBracket = QStringLiteral("]");

  if (!address.contains(colon) || address.isEmpty()) {
    return address;
  }

  QString wrapped = address;

  if (!address.startsWith(openBracket)) {
    wrapped.prepend(openBracket);
  }

  if (!address.endsWith(closeBracket)) {
    wrapped.append(closeBracket);
  }

  return wrapped;
}

//
// CoreProcess
//

CoreProcess::CoreProcess(const ServerConfig &serverConfig, const QString &executable)
    : m_serverConfig(serverConfig),
      m_daemonIpcClient{new ipc::DaemonIpcClient(this)}
{
  m_appPath = executable.isEmpty() ? QStringLiteral("%1/%2").arg(QCoreApplication::applicationDirPath(), kCoreBinName)
                                   : executable;
  if (!QFile::exists(m_appPath)) {
    qCritical("core server binary does not exist");
  }

  connect(m_daemonIpcClient, &ipc::DaemonIpcClient::connected, this, &CoreProcess::daemonIpcClientConnected);
  connect(
      m_daemonIpcClient, &ipc::DaemonIpcClient::connectionFailed, this, &CoreProcess::daemonIpcClientConnectionFailed
  );
  connect(m_daemonIpcClient, &ipc::DaemonIpcClient::logPathReceived, this, &CoreProcess::setupDaemonLogTail);

  m_daemonStateTimer.setInterval(250);
  connect(&m_daemonStateTimer, &QTimer::timeout, this, [this] {
    if (m_daemonIpcClient->isConnected())
      m_daemonIpcClient->requestProcessState(m_daemonStateRequest);
  });
  connect(
      m_daemonIpcClient, &ipc::DaemonIpcClient::processStateReceived, this,
      [this](const QString &id, const QString &state) {
        if (id == m_serviceProbe && !m_serviceProbe.isEmpty()) {
          m_serviceProbe.clear();
          if (state == "running") {
            m_lastProcessMode = m_runtimeProcessMode = ProcessMode::Service;
            ++m_processGeneration;
            m_attachingService = true;
            setProcessState(ProcessState::Started);
            connectCoreIpc();
          }
          Q_EMIT serviceProbeFinished(state);
          return;
        }
        if (id != m_daemonStateRequest)
          return;
        if (m_processState == ProcessState::Stopping && state == "stopped") {
          m_daemonStateTimer.stop();
          setProcessState(ProcessState::Stopped);
        } else if (m_processState == ProcessState::Starting && state == "running") {
          m_daemonStateTimer.stop();
          setProcessState(ProcessState::Started);
        }
      }
  );
  connect(&m_retryTimer, &QTimer::timeout, this, [this] {
    if (m_processState == ProcessState::RetryPending) {
      start();
    } else {
      qDebug("retry cancelled, process state is not retry pending");
    }
  });
}

void CoreProcess::onProcessReadyReadStandardOutput()
{
  if (m_process) {
    handleLogLines(m_process->readAllStandardOutput());
  }
}

void CoreProcess::onProcessReadyReadStandardError()
{
  if (m_process) {
    handleLogLines(m_process->readAllStandardError());
  }
}

void CoreProcess::daemonIpcClientConnected()
{
  applyLogLevel();
  m_daemonIpcClient->requestLogPath();
}

void CoreProcess::checkExistingProcess()
{
  qInfo("checking existing core");

  auto *client = new ipc::CoreIpcClient(this);
  connect(client, &ipc::CoreIpcClient::connected, this, [client] {
    qInfo("existing core has matching version, leaving it running");
    client->deleteLater();
  });
  connect(client, &ipc::CoreIpcClient::versionMismatch, this, [client] {
    qInfo("existing core has mismatched version, asking it to stop");
    client->sendStop();
  });
  connect(client, &ipc::CoreIpcClient::serverShutdown, this, [this, client] {
    qInfo("existing core stopped successfully");
    client->deleteLater();
    setProcessState(ProcessState::RetryPending);
    m_retryTimer.setSingleShot(true);
    m_retryTimer.start(kRetryDelay);
  });
  connect(client, &ipc::CoreIpcClient::connectionFailed, this, [client] {
    qCritical("could not contact existing core");
    client->deleteLater();
  });
  client->connectToServer();
}

void CoreProcess::onProcessFinished(int exitCode, QProcess::ExitStatus)
{
  using enum ProcessState;
  setConnectionState(ConnectionState::Disconnected);

  if (m_retryTimer.isActive()) {
    m_retryTimer.stop();
  }

  if (exitCode != s_exitSuccess) {
    setProcessState(Stopped);
    if (exitCode == s_exitDuplicate) {
      checkExistingProcess();
      return;
    }
    qWarning("desktop process exited with code: %d", exitCode);
    return;
  }

  qDebug("desktop process exited normally");

  if (const auto wasStarted = m_processState == Started; wasStarted) {
    qDebug("desktop process was running, retrying in %d ms", kRetryDelay);
    setProcessState(RetryPending);
    m_retryTimer.setSingleShot(true);
    m_retryTimer.start(kRetryDelay);
  } else {
    setProcessState(Stopped);
  }
}

void CoreProcess::applyLogLevel()
{
  const auto processMode = Settings::value(Settings::Core::ProcessMode).value<Settings::ProcessMode>();
  if (processMode == ProcessMode::Service) {
    qDebug() << "setting daemon log level:" << Settings::logLevelText();
    m_daemonIpcClient->sendLogLevel(Settings::logLevelText());
  }
}

void CoreProcess::startForegroundProcess(const QStringList &args)
{
  using enum ProcessState;

  if (m_processState != Starting) {
    qCritical("not starting core desktop process, unexpected process state");
    return;
  }

  // only make quoted args for printing the command for convenience; so that the
  // core command can be easily copy/pasted to the terminal for testing.
  const auto quoted = makeQuotedArgs(m_appPath, args);
  qInfo("running command: %s", qPrintable(quoted));

#ifdef Q_OS_LINUX
  m_process->setChildProcessModifier([] {
    // the core process becomes orphaned when the gui process exits abruptly (e.g. with kill -9),
    // so ensure the os also kills the core when that happens to the gui.
    prctl(PR_SET_PDEATHSIG, SIGTERM);
  });
#endif

  m_process->start(m_appPath, args);

  if (m_process->waitForStarted()) {
    setProcessState(Started);
  } else {
    setProcessState(Stopped);
    Q_EMIT error(Error::StartFailed);
  }
}

void CoreProcess::watchDaemonState()
{
  m_daemonStateRequest = QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_daemonStateTimer.start();
  const auto request = m_daemonStateRequest;
  QTimer::singleShot(20000, this, [this, request] {
    if (request != m_daemonStateRequest || !m_daemonStateTimer.isActive())
      return;
    m_daemonStateTimer.stop();
    Q_EMIT controlFinished(
        QJsonObject{
            {"command", "service"},
            {"success", false},
            {"reason", tr("The background service did not confirm its process state. Sharing state is unknown.")}
        }
    );
  });
}

void CoreProcess::startProcessFromDaemon()
{
  if (m_processState != ProcessState::Starting) {
    qCritical("not starting core process from daemon, unexpected process state");
    return;
  }

  const auto configFile = m_runtimeSettings.isEmpty() ? Settings::settingsFile() : m_runtimeSettings;
  qInfo("sending start to daemon (config file: %s)", qPrintable(configFile));

  auto sendStart = [this, configFile, generation = m_processGeneration] {
    if (generation != m_processGeneration || m_processState != ProcessState::Starting)
      return;
    m_daemonIpcClient->sendConfigFile(configFile);
    m_daemonIpcClient->sendStartProcess();
    watchDaemonState();
  };

  if (m_daemonIpcClient->isConnected()) {
    sendStart();
  } else {
    connect(
        m_daemonIpcClient, &ipc::DaemonIpcClient::connected, this, sendStart,
        static_cast<Qt::ConnectionType>(Qt::SingleShotConnection | Qt::QueuedConnection)
    );
    m_daemonIpcClient->connectToServer();
  }
}

void CoreProcess::stopForegroundProcess()
{
  if (m_processState != ProcessState::Stopping) {
    qCritical("not stopping core desktop process, unexpected process state");
    return;
  }

  if (!m_process) {
    qCritical("not stopping core desktop process, no process to stop");
    return;
  }

  qInfo("stopping core desktop process");
  if (m_coreIpcClient && m_coreIpcClient->isConnected()) {
    qDebug("sending stop command to core process");
    m_coreIpcClient->sendStop();

    // Fallback: if the core doesn't confirm shutdown via IPC within the
    // timeout, force-terminate it so we never hang in the Stopping state.
    QTimer::singleShot(kRetryDelay, this, [this] {
      if (m_processState != ProcessState::Stopping) {
        return;
      }

      qWarning("core process did not confirm graceful stop in time, forcing terminate");

      if (m_coreIpcClient) {
        m_coreIpcClient->disconnectFromServer();
        m_coreIpcClient->deleteLater();
        m_coreIpcClient = nullptr;
      }

      if (m_process && m_process->state() == QProcess::ProcessState::Running) {
        m_process->terminate();
      }
    });
    return;
  }

  if (m_process->state() == QProcess::ProcessState::Running) {
    qDebug("process is running, terminating");
    m_process->terminate();
  } else {
    qDebug("process is not running, skipping terminate");
  }
}

void CoreProcess::stopProcessFromDaemon()
{
  if (m_processState != ProcessState::Stopping) {
    qCritical("not stopping core process from daemon, unexpected process state");
    return;
  }

  auto sendStop = [this, generation = m_processGeneration] {
    if (generation != m_processGeneration || m_processState != ProcessState::Stopping)
      return;
    m_daemonIpcClient->sendStopProcess();
    watchDaemonState();
  };

  if (m_daemonIpcClient->isConnected()) {
    sendStop();
  } else {
    connect(
        m_daemonIpcClient, &ipc::DaemonIpcClient::connected, this, sendStop,
        static_cast<Qt::ConnectionType>(Qt::SingleShotConnection | Qt::QueuedConnection)
    );
    m_daemonIpcClient->connectToServer();
  }
}

void CoreProcess::handleLogLines(const QString &text)
{
  const auto lines = text.split(kLineSplitRegex);
  for (const auto &line : lines) {
    if (line.isEmpty()) {
      continue;
    }

#if defined(Q_OS_MACOS)
    // HACK: macOS 10.13.4+ spamming error lines in logs making them
    // impossible to read and debug; giving users a red herring.
    if (line.contains("calling TIS/TSM in non-main thread environment")) {
      continue;
    }

    // the core process is not allowed to show the permission prompt
    // (called "notification permission") and the notification log line is emitted from
    // deep inside cocoa code in the core binary to stdout, so it can't be sent over
    // ipc from the core to the gui and instead the gui has to parse the core output.
    static const QString needle = "OSX Notification: ";
    if (line.contains(needle) && line.contains('|')) {
      const int delimiterPosition = line.indexOf('|');
      const int start = line.indexOf(needle);
      const QString title = line.mid(start + needle.length(), delimiterPosition - start - needle.length());
      const QString body = line.mid(delimiterPosition + 1, line.length() - delimiterPosition);
      if (!showOSXNotification(title, body)) {
        qDebug("osx notification was not shown");
      }
    }
#endif

    Q_EMIT logLine(line);
  }
}

CoreProcess::RuntimeConfiguration CoreProcess::runtimeConfiguration() const
{
  if (m_runtimeSettings.isEmpty())
    return {};
  QSettings settings(m_runtimeSettings, QSettings::IniFormat);
  return {
      m_runtimeSettings, m_runtimeConfig, settings.value(Settings::Client::RemoteHost).toString(), m_runtimeProcessMode,
      m_mode
  };
}
bool CoreProcess::restoreRuntimeConfiguration(const RuntimeConfiguration &configuration)
{
  if (configuration.settingsFile.isEmpty() || !QFileInfo(configuration.settingsFile).isReadable() ||
      configuration.role == Settings::CoreMode::None)
    return false;
  m_runtimeSettings = configuration.settingsFile;
  m_runtimeConfig = configuration.serverFile;
  m_runtimeProcessMode = configuration.processMode;
  m_mode = configuration.role;
  m_address = correctedAddress(configuration.source);
  return true;
}

bool CoreProcess::prepareConfiguration()
{
  m_configurationError.clear();
  if (!m_serverConfig.loadError().isEmpty()) {
    m_configurationError = m_serverConfig.loadError();
    return false;
  }
  // Freeze both files before stopping the old core. The profile directory is
  // unchanged so certificate/trust defaults retain their original scope.
  if (Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>() == Settings::CoreMode::None)
    return false;
  if (Settings::value(Settings::Security::TlsEnabled).toBool()) {
    const auto certificate = Settings::value(Settings::Security::Certificate).toString();
    if (!TlsUtility::isCertValid(certificate)) {
      if (QFile::exists(certificate) || !TlsUtility::generateCertificate())
        return false;
    }
  }
  Settings::save();
  Settings::proxy().sync();
  if (Settings::proxy().get().status() != QSettings::NoError)
    return false;
  const auto stem = Settings::settingsPath() + "/.runtime-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto settingsPath = stem + ".conf";
  const auto graphPath = stem + ".server.conf";
  QByteArray graph;
  if (Settings::value(Settings::Server::ExternalConfig).toBool()) {
    QFile source(Settings::value(Settings::Server::ExternalConfigFile).toString());
    if (!source.open(QIODevice::ReadOnly))
      return false;
    graph = source.readAll();
  } else {
    QString text;
    QTextStream out(&text);
    out << m_serverConfig;
    out.flush();
    graph = text.toUtf8();
  }
  QSaveFile graphFile(graphPath);
  if (!graphFile.open(QIODevice::WriteOnly))
    return false;
  graphFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
  if (graphFile.write(graph) != graph.size() || !graphFile.commit())
    return false;
  QSettings snapshot(settingsPath, QSettings::IniFormat);
  snapshot.setAtomicSyncRequired(true);
  for (const auto &key : Settings::validKeys())
    snapshot.setValue(key, Settings::value(key));
  auto &profile = Settings::proxy().get();
  for (const auto &key : profile.allKeys())
    snapshot.setValue(key, profile.value(key));
  snapshot.setValue(Settings::Server::ExternalConfig, true);
  snapshot.setValue(Settings::Server::ExternalConfigFile, graphPath);
  snapshot.sync();
  if (snapshot.status() != QSettings::NoError) {
    QFile::remove(graphPath);
    QFile::remove(settingsPath);
    return false;
  }
  QFile::setPermissions(settingsPath, QFile::ReadOwner | QFile::WriteOwner);
  QProcess validator;
  const auto role = Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>() == Settings::CoreMode::Server
                        ? "server"
                        : "client";
  validator.start(m_appPath, {role, "--validate-config", "--settings", settingsPath});
  if (!validator.waitForFinished(5000) || validator.exitStatus() != QProcess::NormalExit || validator.exitCode() != 0) {
    validator.kill();
    validator.waitForFinished(1000);
    const auto output = validator.readAllStandardOutput();
    const auto lines = output.trimmed().split('\n');
    if (!lines.isEmpty())
      m_configurationError = QJsonDocument::fromJson(lines.last()).object().value("reason").toString();
    if (m_configurationError.isEmpty())
      m_configurationError =
          tr("The core could not validate the configuration. Check the executable and file permissions.");
    qWarning() << "Configuration validation failed:" << m_configurationError;
    QFile::remove(settingsPath);
    QFile::remove(graphPath);
    return false;
  }
  m_runtimeSettings = settingsPath;
  m_runtimeConfig = graphPath;
  m_runtimeProcessMode = Settings::value(Settings::Core::ProcessMode).value<ProcessMode>();
  return true;
}

void CoreProcess::start(std::optional<ProcessMode> processModeOption)
{
  if (!m_serviceProbe.isEmpty()) {
    qWarning("not starting core while background service state is being checked");
    return;
  }
  if (m_processState != ProcessState::Stopped && m_processState != ProcessState::RetryPending) {
    qCritical("not starting core process, previous process is not stopped");
    return;
  }

  if (m_mode == Settings::CoreMode::None) {
    qCritical("not starting core process, no core mode set");
    return;
  }

  QMutexLocker locker(&m_processMutex);

  const auto currentMode = Settings::value(Settings::Core::ProcessMode).value<ProcessMode>();
  const auto processMode = processModeOption.value_or(m_runtimeSettings.isEmpty() ? currentMode : m_runtimeProcessMode);
  const auto coreMode = QVariant::fromValue(m_mode).toString().toLower();

  qInfo().noquote() << QString("starting %1 process (%2 mode)").arg(coreMode, processModeToString(processMode));

  ++m_processGeneration;
  m_pendingControls.clear();
  setProcessState(ProcessState::Starting);

#ifdef Q_OS_MACOS
  requestOSXNotificationPermission();
#endif

  setConnectionState(ConnectionState::Connecting);

  if (processMode == ProcessMode::Desktop) {
    m_process = new QProcess(this);
    connect(m_process, &QProcess::finished, this, &CoreProcess::onProcessFinished, Qt::UniqueConnection);
    connect(
        m_process, &QProcess::readyReadStandardOutput, this, &CoreProcess::onProcessReadyReadStandardOutput,
        Qt::UniqueConnection
    );
    connect(
        m_process, &QProcess::readyReadStandardError, this, &CoreProcess::onProcessReadyReadStandardError,
        Qt::UniqueConnection
    );
  }

  QStringList args = {coreMode};
  if (!m_runtimeSettings.isEmpty())
    args << "--settings" << m_runtimeSettings;

  if (m_mode == Settings::CoreMode::Server) {
    const auto [hasNeededPermissions, configFilename] = persistServerConfig();
    if (configFilename.isEmpty()) {
      qCritical("not starting core process, no server config file");
      setProcessState(ProcessState::Stopped);
      setConnectionState(ConnectionState::Disconnected);
      Q_EMIT error(Error::StartFailed);
      return;
    }
    if (!hasNeededPermissions) {
      setProcessState(ProcessState::Stopped);
      setConnectionState(ConnectionState::Disconnected);
      Q_EMIT error(Error::StartFailed);
      return;
    }
    qInfo("core config file: %s", qPrintable(configFilename));
  }

  qDebug().noquote() << "log level:" << Settings::logLevelText();

  if (Settings::value(Settings::Log::ToFile).toBool()) {
    const auto logFile = Settings::value(Settings::Log::File).toString();
    QDir(QFileInfo(logFile).absolutePath()).mkpath(".");
    qInfo().noquote() << "log file:" << logFile;
  }

  // Connect only for this generation. An older delayed startup must not
  // replace the IPC connection belonging to a newer source.
  const auto generation = m_processGeneration;
  auto attach = [this, generation] {
    QTimer::singleShot(kRetryDelay, this, [this, generation] {
      if (generation == m_processGeneration && m_processState == ProcessState::Started)
        connectCoreIpc();
    });
  };
  if (processMode == ProcessMode::Desktop) {
    // Desktop startForegroundProcess synchronously reports Started.
  } else {
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(
        this, &CoreProcess::processStateChanged, this,
        [this, generation, attach, connection](ProcessState state) {
          if (generation != m_processGeneration || state == ProcessState::Stopped) {
            disconnect(*connection);
            return;
          }
          if (state == ProcessState::Started) {
            disconnect(*connection);
            attach();
          }
        }
    );
  }

  if (processMode == ProcessMode::Desktop) {
    startForegroundProcess(args);
    if (m_processState == ProcessState::Started)
      attach();
  } else if (processMode == ProcessMode::Service) {
    startProcessFromDaemon();
  }

  m_lastProcessMode = processMode;
}

void CoreProcess::probeExistingService()
{
  if (m_processState != ProcessState::Stopped || !m_serviceProbe.isEmpty())
    return;
  m_serviceProbe = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto id = m_serviceProbe;
  auto query = [this, id] {
    if (m_serviceProbe == id)
      m_daemonIpcClient->requestProcessState(id);
  };
  if (m_daemonIpcClient->isConnected())
    query();
  else {
    connect(m_daemonIpcClient, &ipc::DaemonIpcClient::connected, this, query, Qt::SingleShotConnection);
    m_daemonIpcClient->connectToServer();
  }
  QTimer::singleShot(5000, this, [this, id] {
    if (m_serviceProbe == id) {
      m_serviceProbe.clear();
      Q_EMIT serviceProbeFinished("unknown");
    }
  });
}

void CoreProcess::connectCoreIpc()
{
  if (m_coreIpcClient) {
    m_coreIpcClient->disconnectFromServer();
    m_coreIpcClient->deleteLater();
  }
  m_coreIpcClient = new ipc::CoreIpcClient(this);
  connect(
      m_coreIpcClient, &ipc::CoreIpcClient::commandReceived, this,
      [this, generation = m_processGeneration](const QString &command, const QString &args) {
        if (generation == m_processGeneration)
          onCoreIpcMessageReceived(command, args);
      },
      Qt::QueuedConnection
  );
  connect(m_coreIpcClient, &ipc::CoreIpcClient::connected, this, [this] { Q_EMIT ipcReady(); });
  connect(m_coreIpcClient, &ipc::CoreIpcClient::connectionFailed, this, [] {
    qWarning("failed to establish core ipc connection");
  });
  connect(m_coreIpcClient, &ipc::CoreIpcClient::serverShutdown, this, [this, client = m_coreIpcClient] {
    client->deleteLater();
    if (m_coreIpcClient == client)
      m_coreIpcClient = nullptr;
  });
  m_coreIpcClient->connectToServer();
}

void CoreProcess::stop(std::optional<ProcessMode> processModeOption)
{
  QMutexLocker locker(&m_processMutex);
  ++m_restartGeneration;
  disconnect(m_restartConnection);
  m_retryTimer.stop();

  const auto currentMode = Settings::value(Settings::Core::ProcessMode).value<ProcessMode>();
  const auto processMode = processModeOption.value_or(m_lastProcessMode.value_or(currentMode));

  ++m_processGeneration;
  m_pendingControls.clear();
  m_serviceProbe.clear();
  m_attachingService = false;
  qInfo("stopping core process (%s mode)", qPrintable(processModeToString(processMode)));

  if (m_coreIpcClient && processMode != ProcessMode::Desktop) {
    m_coreIpcClient->disconnectFromServer();
    m_coreIpcClient->deleteLater();
    m_coreIpcClient = nullptr;
  }

  if (m_processState == ProcessState::Starting) {
    qDebug("core process is starting, cancelling");
    if (processMode == ProcessMode::Service) {
      setProcessState(ProcessState::Stopping);
      stopProcessFromDaemon();
    } else if (m_process && m_process->state() != QProcess::NotRunning) {
      setProcessState(ProcessState::Stopping);
      stopForegroundProcess();
    } else
      setProcessState(ProcessState::Stopped);
  } else if (m_processState == ProcessState::RetryPending && processMode == ProcessMode::Desktop &&
             (!m_process || m_process->state() == QProcess::NotRunning)) {
    setProcessState(ProcessState::Stopped);
  } else if (m_processState != ProcessState::Stopped) {
    setProcessState(ProcessState::Stopping);

    if (processMode == ProcessMode::Service) {
      stopProcessFromDaemon();
    } else if (processMode == ProcessMode::Desktop) {
      stopForegroundProcess();
    }

  } else {
    qWarning("core process already stopped");
  }

  setConnectionState(ConnectionState::Disconnected);
}

void CoreProcess::restart()
{
  // stop() cancels any older queued restart. A later explicit Stop must also
  // invalidate this continuation, including the interval after process exit.
  stop();
  const auto generation = m_restartGeneration;
  const auto resume = [this, generation] {
    QTimer::singleShot(0, this, [this, generation] {
      if (generation == m_restartGeneration && m_processState == ProcessState::Stopped)
        start();
    });
  };
  if (m_processState == ProcessState::Stopped) {
    resume();
  } else {
    m_restartConnection = connect(this, &CoreProcess::processStateChanged, this, [this, resume](ProcessState state) {
      if (state != ProcessState::Stopped)
        return;
      disconnect(m_restartConnection);
      resume();
    });
  }
}

void CoreProcess::cleanup()
{
  qInfo("cleaning up core process");

  const auto isDesktop = !serviceMode();
  const auto isRunning = m_processState == ProcessState::Started;
  if (isDesktop && isRunning) {
    stop();
  }
}

QPair<bool, QString> CoreProcess::persistServerConfig() const
{
  if (!m_runtimeConfig.isEmpty())
    return {QFileInfo(m_runtimeConfig).isReadable(), m_runtimeConfig};
  if (Settings::value(Settings::Server::ExternalConfig).toBool()) {
    return {Settings::isServerConfigFileReadable(), Settings::value(Settings::Server::ExternalConfigFile).toString()};
  }

  const auto configFilePath = Settings::defaultValue(Settings::Server::ExternalConfigFile).toString();
  QFile configFile(configFilePath);
  if (!configFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    qWarning() << "failed to open core config file for write:" << configFilePath;
    return {false, configFile.fileName()};
  }

  m_serverConfig.save(configFile);
  configFile.close();
  return {Settings::isServerConfigFileReadable(), configFile.fileName()};
}

void CoreProcess::setConnectionState(ConnectionState state)
{
  if (m_connectionState == state) {
    return;
  }

  m_connectionState = state;
  Q_EMIT connectionStateChanged(state);
}

void CoreProcess::setProcessState(ProcessState state)
{
  if (m_processState == state) {
    return;
  }

  qDebug(
      "core process state changed: %s -> %s", //
      qPrintable(processStateToString(m_processState)), qPrintable(processStateToString(state))
  );
  m_processState = state;
  Q_EMIT processStateChanged(state);
}

void CoreProcess::onCoreIpcMessageReceived(const QString &command, const QString &args)
{
  if (command == "controlResult") {
    const auto result =
        QJsonDocument::fromJson(QByteArray::fromBase64(args.toLatin1(), QByteArray::Base64UrlEncoding)).object();
    if (m_pendingControls.remove(result.value("id").toString())) {
      if (result.value("command").toString() == "ready" && result.value("success").toBool()) {
        if (m_attachingService) {
          m_attachingService = false;
          m_mode = static_cast<Settings::CoreMode>(result.value("role").toInt());
          m_runtimeSettings = result.value("settingsFile").toString();
          QSettings runtime(m_runtimeSettings, QSettings::IniFormat);
          m_runtimeConfig = runtime.value(Settings::Server::ExternalConfigFile).toString();
          m_address = correctedAddress(result.value("source").toString());
        }
        Q_EMIT controlFinished(result);
        onCoreIpcMessageReceived("connectionState", result.value("connectionState").toString());
        if (result.contains("connectedClients")) {
          QStringList peers;
          for (const auto &peer : result.value("connectedClients").toArray())
            peers.append(peer.toString());
          Q_EMIT connectedClientsChanged(peers);
        }
      } else
        Q_EMIT controlFinished(result);
    }
    return;
  }
  if (command == "connectionState") {
    const auto metaEnum = QMetaEnum::fromType<ConnectionState>();
    bool ok = false;
    const auto state = static_cast<ConnectionState>(metaEnum.keyToValue(args.toUtf8().constData(), &ok));
    if (!ok) {
      qWarning("core ipc got unknown connection state: %s", args.toUtf8().constData());
      return;
    }
    setConnectionState(state);
  } else if (command == "connectedClients") {
    const auto clients = args.isEmpty() ? QStringList() : args.split(",");
    Q_EMIT connectedClientsChanged(clients);
  } else if (command == "secureSocket") {
    Q_EMIT secureSocket(true);
    if (args != m_secureSocketVersion) {
      m_secureSocketVersion = args;
      Q_EMIT securityLevelChanged(args);
    }
  } else if (command == "unrecognisedClient") {
    Q_EMIT unrecognisedClient(args);
  } else if (command == "connectionRefused") {
    const auto metaEnum = QMetaEnum::fromType<deskflow::core::ConnectionRefusal>();
    bool ok = false;
    const auto reason =
        static_cast<deskflow::core::ConnectionRefusal>(metaEnum.keyToValue(args.toUtf8().constData(), &ok));
    if (ok) {
      Q_EMIT connectionRefused(reason);
    } else {
      qWarning("core ipc got unknown connection refusal: %s", args.toUtf8().constData());
    }
  } else if (command == "retryIn") {
    Q_EMIT retryIn(args.toInt());
  } else if (command == "identityRevoked") {
    Q_EMIT peerIdentityBlocked(args);
  } else if (command == "peerIdentity") {
    Q_EMIT peerIdentityRequired(QJsonDocument::fromJson(args.toUtf8()).object());
  } else if (command == "peerFingerprint") {
    Q_EMIT peerFingerprint(args);
  } else if (command == "missingKeyboardLayouts") {
    Q_EMIT missingKeyboardLayouts(args);
  }
}

QString CoreProcess::control(const QString &command, const QString &argument)
{
  const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_pendingControls.insert(id);
  if (m_coreIpcClient && m_coreIpcClient->isConnected()) {
    m_coreIpcClient->sendControl(id, command, argument);
  }
  QTimer::singleShot(5000, this, [this, id, command, argument] {
    if (!m_pendingControls.remove(id))
      return;
    Q_EMIT controlFinished(
        QJsonObject{
            {"id", id},
            {"command", command},
            {"argument", argument},
            {"success", false},
            {"reason", tr("The core did not confirm the operation. Check the connection and retry.")}
        }
    );
  });
  return id;
}

bool CoreProcess::checkSecureSocket(const QString &line)
{
  static const QString tlsCheckString = "network encryption protocol: ";
  const auto index = line.indexOf(tlsCheckString, 0, Qt::CaseInsensitive);
  if (index == -1) {
    return false;
  }

  Q_EMIT secureSocket(true);
  if (const auto ssv = line.mid(index + tlsCheckString.size()); ssv != m_secureSocketVersion) {
    m_secureSocketVersion = ssv;
    Q_EMIT securityLevelChanged(ssv);
  }

  return true;
}

QString CoreProcess::correctedAddress(const QString &address) const
{
  return wrapIpv6(address.simplified());
}

void CoreProcess::setupDaemonLogTail(const QString &logPath)
{
  qDebug() << "daemon log path:" << logPath;

  if (QFileInfo logFile(logPath); !logFile.isFile()) {
    auto file = QFile(logPath);
    if (!file.open(QFile::ReadWrite)) {
      qCritical() << "daemon log path file can not be written:" << logPath;
      return;
    }
    file.write(""); // Create an empty file
  }

  if (m_daemonFileTail) {
    m_daemonFileTail->setWatchedFile(logPath);
  } else {
    m_daemonFileTail = new FileTail(logPath, this);
    connect(m_daemonFileTail, &FileTail::newLine, this, &CoreProcess::handleLogLines);
  }
}

void CoreProcess::clearSettings()
{
  const auto processMode = Settings::value(Settings::Core::ProcessMode).value<ProcessMode>();
  if (processMode == ProcessMode::Desktop) {
    qDebug("no core settings to clear in desktop mode");
    return;
  }

  if (processMode != ProcessMode::Service) {
    qCritical("not clearing core settings, unexpected process mode");
    return;
  }

  qInfo("clearing core settings through daemon");
  m_daemonIpcClient->sendClearSettings();
}

void CoreProcess::retryDaemon()
{
  m_daemonIpcClient->connectToServer();
}

} // namespace deskflow::gui
