/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2024 - 2025 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "common/Enums.h"
#include "common/Settings.h"
#include "gui/FileTail.h"
#include "gui/config/ServerConfig.h"

#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QTimer>

namespace deskflow::gui {

namespace ipc {
class CoreIpcClient;
class DaemonIpcClient;
} // namespace ipc

class CoreProcess : public QObject
{
  using ConnectionState = deskflow::core::ConnectionState;
  using ProcessMode = Settings::ProcessMode;
  using ProcessState = deskflow::core::ProcessState;
  Q_OBJECT

public:
  enum class Error
  {
    AddressMissing,
    StartFailed
  };

  struct RuntimeConfiguration
  {
    QString settingsFile, serverFile, source;
    Settings::ProcessMode processMode = Settings::ProcessMode::Desktop;
    Settings::CoreMode role = Settings::CoreMode::None;
  };
  RuntimeConfiguration runtimeConfiguration() const;
  bool restoreRuntimeConfiguration(const RuntimeConfiguration &configuration);
  explicit CoreProcess(const ServerConfig &serverConfig, const QString &executable = {});

  void start(std::optional<ProcessMode> processMode = std::nullopt);
  void stop(std::optional<ProcessMode> processMode = std::nullopt);
  void restart();
  void cleanup();
  void probeExistingService();
  void applyLogLevel();
  void clearSettings();
  void retryDaemon();
  bool prepareConfiguration();
  QString runtimeSettingsFile() const
  {
    return m_runtimeSettings;
  }
  QString configurationError() const
  {
    return m_configurationError;
  }
  QString control(const QString &command, const QString &argument = {});

  // getters
  Settings::CoreMode mode() const
  {
    return m_mode;
  }
  QString secureSocketVersion() const
  {
    return m_secureSocketVersion;
  }
  bool serviceMode() const
  {
    return m_lastProcessMode.value_or(m_runtimeProcessMode) == ProcessMode::Service;
  }
  bool isStarted() const
  {
    return m_processState == ProcessState::Started;
  }
  ProcessState processState() const
  {
    return m_processState;
  }
  ConnectionState connectionState() const
  {
    return m_connectionState;
  }

  void validateExternalFile(const QString &path);

  // setters
  void setAddress(const QString &address)
  {
    m_address = correctedAddress(address);
  }
  void setMode(Settings::CoreMode mode)
  {
    m_mode = mode;
  }

Q_SIGNALS:
  void fileValidationFinished(const QString &path, bool success, const QString &reason);
  void error(deskflow::gui::CoreProcess::Error error);
  void logLine(const QString &line);
  void connectionStateChanged(deskflow::core::ConnectionState state);
  void processStateChanged(deskflow::core::ProcessState state);
  void secureSocket(bool enabled);
  void daemonIpcClientConnectionFailed();
  void connectedClientsChanged(const QStringList &clients);
  void securityLevelChanged(QString securityLevel);
  void unrecognisedClient(const QString &clientName);
  void connectionRefused(deskflow::core::ConnectionRefusal reason);
  void retryIn(int seconds);
  void peerIdentityRequired(const QJsonObject &identity);
  void peerFingerprint(const QString &fingerprint);
  void peerIdentityBlocked(const QString &fingerprint);
  void missingKeyboardLayouts(const QString &layouts);
  void ipcReady();
  void serviceProbeFinished(const QString &state);
  void controlFinished(const QJsonObject &result);

private Q_SLOTS:
  void onProcessFinished(int exitCode, QProcess::ExitStatus);
  void onProcessReadyReadStandardOutput();
  void onProcessReadyReadStandardError();
  void onCoreIpcMessageReceived(const QString &command, const QString &args);
  void daemonIpcClientConnected();

private:
  void startForegroundProcess(const QStringList &args);
  void startProcessFromDaemon();
  void stopForegroundProcess();
  void stopProcessFromDaemon();
  QPair<bool, QString> persistServerConfig() const;
  void setConnectionState(ConnectionState state);
  void setProcessState(ProcessState state);
  bool checkSecureSocket(const QString &line);
  void handleLogLines(const QString &text);
  QString correctedAddress(const QString &address) const;
  void setupDaemonLogTail(const QString &logPath);
  void checkExistingProcess();
  static QString makeQuotedArgs(const QString &app, const QStringList &args);
  static QString processModeToString(const Settings::ProcessMode mode);
  static QString processStateToString(const CoreProcess::ProcessState state);
  static QString wrapIpv6(const QString &address);

  const ServerConfig &m_serverConfig;
  QString m_address;
  ProcessState m_processState = ProcessState::Stopped;
  ConnectionState m_connectionState = ConnectionState::Disconnected;
  Settings::CoreMode m_mode = Settings::CoreMode::None;
  QMutex m_processMutex;
  QString m_secureSocketVersion;
  std::optional<ProcessMode> m_lastProcessMode = std::nullopt;
  QTimer m_retryTimer;
  deskflow::gui::ipc::CoreIpcClient *m_coreIpcClient = nullptr;
  deskflow::gui::ipc::DaemonIpcClient *m_daemonIpcClient = nullptr;
  FileTail *m_daemonFileTail = nullptr;
  QProcess *m_process = nullptr;
  QString m_appPath;
  quint64 m_fileValidationGeneration = 0;
  quint64 m_processGeneration = 0;
  QMetaObject::Connection m_restartConnection;
  quint64 m_restartGeneration = 0;
  QString m_configurationError;
  QString m_runtimeSettings;
  QString m_runtimeConfig;
  ProcessMode m_runtimeProcessMode = ProcessMode::Desktop;
  QSet<QString> m_pendingControls;
  QTimer m_daemonStateTimer;
  QString m_daemonStateRequest;
  QString m_serviceProbe;
  bool m_attachingService = false;
  void watchDaemonState();
  void connectCoreIpc();
};

} // namespace deskflow::gui
