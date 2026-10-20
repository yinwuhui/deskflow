/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2024 - 2026 Chris Rizzitello <sithord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2024 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QMainWindow>
#include <QProcess>
#include <QRegularExpression>
#include <QSystemTrayIcon>
#include <QUrl>

#include "VersionChecker.h"
#include "config/ServerConfig.h"
#include "core/ApplicationState.h"
#include "core/SourceChangeTransaction.h"
#include "gui/core/CoreProcess.h"
#include "gui/core/NetworkMonitor.h"
#include "net/Fingerprint.h"
#include "net/MdnsBrowser.h"
#include "workspace/WorkspaceUi.h"

#ifdef Q_OS_MACOS
#include "gui/OSXHelpers.h"
#endif

class QAction;
class QMenu;
class QLocalServer;
class QLabel;
class QListWidget;
class QStackedWidget;
class QTreeWidget;
class QTabWidget;
class QPushButton;
class SettingsDialog;
class ClientConfigDialog;
class ServerConfigDialog;

class QVBoxLayout;
namespace deskflow::gui::workspace {
class SettingsWorkspace;
class DeviceWorkspace;
class SetupAssistant;
} // namespace deskflow::gui::workspace
class DeskflowApplication;
class LogDock;
class StatusBar;

namespace Ui {
class MainWindow;
}

namespace deskflow::gui::ipc {
class DaemonIpcClient;
}

class MainWindow : public QMainWindow
{
  using ConnectionState = deskflow::core::ConnectionState;
  using CoreMode = Settings::CoreMode;
  using CoreProcess = deskflow::gui::CoreProcess;
  using NetworkMonitor = deskflow::gui::NetworkMonitor;
  using ProcessState = deskflow::core::ProcessState;

  Q_OBJECT

public:
  enum class LogLevel
  {
    Error,
    Info
  };

public:
  explicit MainWindow(const QString &socketName = {});
  ~MainWindow() override;

  [[nodiscard]] CoreMode coreMode() const
  {
    return m_coreProcess.mode();
  }
  void open();
  ServerConfig &serverConfig()
  {
    return m_serverConfig;
  }

  void hide();

protected:
  void changeEvent(QEvent *e) override;
  bool eventFilter(QObject *obj, QEvent *event) override;

private:
  /**
   * @brief updateText Update all text not in the UI
   */
  void setupWorkspace();
  QWidget *createTrustPage();
  void refreshTrustPage();
  QWidget *createDiagnosticsPage();
  QWidget *createHelpPage();
  QWidget *createLogsPage();
  void navigateSettings(const QString &section);
  void refreshWorkspaceTheme();
  void showSetupAssistant();
  void setupAssistantSignals();
  void applyAddedDeviceLayout();
  void beginSetupAssistant(bool chooseRole);
  void setupSourceChange();
  void refreshWorkspace();
  void restoreWorkspaceState();
  bool persistWorkspaceState();
  bool persistConfiguration();
  QByteArray configurationSnapshot() const;
  void applySavedConfiguration();
  void showTrust();
  void exportDiagnosticReport();
  void exportConfigurationBackup();
  void exportGraphicalConfiguration();
  void exportRoutingConfiguration();
  void regenerateLocalIdentity(int keyLength);
  void restoreConfigurationBackup();
  void verifySelectedInput();
  void requestQuit();
  void updateText();
  void toggleLogVisible(bool visible);

  void settingsChanged(const QString &key = QString());
  void serverConfigSaving();
  void coreProcessError(CoreProcess::Error error);
  void coreConnectionStateChanged(ConnectionState state);
  void coreProcessStateChanged(ProcessState state);

  void trayIconActivated(QSystemTrayIcon::ActivationReason reason);
  void serverConnectionConfigureClient(const QString &clientName);

  void clearSettings();
  void openAboutDialog();
  void openGetNewVersionUrl() const;
  void openSettings();
  void startCore();
  void stopCore();
  bool saveServerConfig();
  void resetCore();

  void showMyFingerprint();
  void updateSecurityIcon(bool visible);
  void updateNetworkInfo();

  void coreModeToggled(bool checked);
  void updateModeControls();
  void updateModeControlLabels();
  std::unique_ptr<Ui::MainWindow> ui;

  void createMenuBar();
  void setupTrayIcon();
  void applyConfig();
  void setTrayIcon();
  void handleUnrecognisedClient(const QString &clientName);
  void handleConnectionRefused(deskflow::core::ConnectionRefusal reason);
  void handlePeerIdentity(const QJsonObject &identity);
  void advanceIdentityQueue();
  void handlePeerFingerprint(const QString &fingerprint);
  void handleMissingKeyboardLayouts(const QString &layouts);
  void closeEvent(QCloseEvent *event) override;
  bool maybeHideToTray();
  void secureSocket(bool secureSocket);
  void connectSlots();
  void handleLogLine(const QString &line);
  void updateFingerprintButton();
  void updateComputerName();
  void saveSettings() const;
  void showConfigureServer(const QString &message);
  void showConfigureClient();
  void restoreWindow();
  void setupControls();
  void showFirstConnectedMessage();
  void updateStatus();
  void showAndActivate();
  void showHostNameEditor();
  void setHostName();
  void daemonIpcClientConnectionFailed();
  void toggleCanRunCore(bool enableButtons);
  void remoteHostChanged(const QString &newRemoteHost);
  void updateIpLabel(const QStringList &addresses);
  void updateTimeoutDelay(int newDelay);
  void setHelpFilePath();
  void showHelpViewer() const;
  void browseServers();

  bool canRunCore() const;

  /**
   * @brief trustedFingerprintDatabase get the FingerprintDatabase for the trusted clients or trusted servers.
   * @return The path to the trusted fingerprint file
   */
  QString trustedFingerprintDatabase() const;

  /**
   * @brief generateCertificate Generate a new certificate
   * @return true when successful
   */
  bool generateCertificate();

  Fingerprint m_fingerprint;

  void serverClientsChanged(const QStringList &clients);

  inline static const auto m_guiSocketName = QStringLiteral("deskflow-gui");
  inline static const auto m_nameRegEx = QRegularExpression(QStringLiteral("^[\\w\\-_\\.]{0,255}$"));

  deskflow::gui::ApplicationState m_applicationState;
  deskflow::gui::SourceChangeTransaction m_sourceChange;
  CoreProcess::RuntimeConfiguration m_sourceBeforeChange;
  QTimer m_sourceChangeTimer;
  QPushButton *m_cancelSourceChange = nullptr;
  bool m_sourcePhaseStarted = false;
  bool m_verifyingIdentity = false;
  QStringList m_pendingFingerprints;
  QList<QJsonObject> m_pendingIdentities;
  bool m_trustResumeAllowed = false;
  quint64 m_trustGeneration = 0;
  deskflow::gui::workspace::Navigation *m_navigation = nullptr;
  deskflow::gui::workspace::SettingsWorkspace *m_settingsWorkspace = nullptr;
  deskflow::gui::workspace::DeviceWorkspace *m_devicesWorkspace = nullptr;
  QStackedWidget *m_deviceModes = nullptr;
  deskflow::gui::workspace::SetupAssistant *m_setupAssistant = nullptr;
  bool m_identityRegenerating = false;
  bool m_setupStartedCore = false;
  bool m_setupArranging = false;
  QMetaObject::Connection m_setupDiscoveryConnection;
  QString m_layoutReloadRequest;
  CoreProcess::RuntimeConfiguration m_beforeLayoutReload;
  quint64 m_layoutReloadRevision = 0;
  QLabel *m_sessionDetail = nullptr;
  QPushButton *m_addComputer = nullptr;
  QWidget *m_pendingPanel = nullptr;
  QWidget *m_trustContent = nullptr;
  QVBoxLayout *m_trustRows = nullptr;
  QByteArray m_trustDigest;
  bool m_trustRefreshScheduled = false;
  QMap<QString, QLabel *> m_diagnosticLabels;
  QLabel *m_receiverSource = nullptr;
  QStackedWidget *m_diagnosticsPages = nullptr;
  QStackedWidget *m_pages = nullptr;
  QLabel *m_workspaceStatus = nullptr;
  QLabel *m_configStatus = nullptr;
  QLabel *m_diagnosticStatus = nullptr;
  QPushButton *m_applySaved = nullptr;
  QPushButton *m_pauseSharing = nullptr;
  QAction *m_actionPause = nullptr;
  QAction *m_traySummary = nullptr, *m_trayConnections = nullptr, *m_trayProblem = nullptr;
  quint64 m_applyingRevision = 0;
  bool m_paused = false;
  bool m_explicitQuit = false;
  bool m_detachUi = false;
  bool m_discardDraftsOnQuit = false;
  bool m_clearProfilePending = false;
  bool m_quitWhenStopped = false;
  QString m_activeSource;
  QString m_confirmedRuntimeFile;
  QString m_activeInterface;
  bool m_activeExternalConfig = false;
  bool m_activeTlsEnabled = false;
  bool m_activePeerAuth = false;
  bool m_applyingExternalConfig = false;
  QString m_lastOperationError;
  bool m_disconnectNotified = false;
  QString m_readyRequest;
  QString m_pauseRequest;
  QStringList m_connectedDevices;
  VersionChecker m_versionChecker;
  bool m_secureSocket = false;
  bool m_saveOnExit = true;
  bool m_clientErrorVisible = false;
  ServerConfig m_serverConfig;
  deskflow::gui::CoreProcess m_coreProcess;
  QSet<QString> m_ignoredClients;
  bool m_newClientPromptShowing = false;
  bool m_serverConfigDialogVisible = false;
  QSize m_expandedSize = QSize();
  QStringList m_checkedClients;
  QStringList m_checkedServers;
  QSystemTrayIcon *m_trayIcon = nullptr;
  QLocalServer *m_guiDupeChecker = nullptr;
  deskflow::gui::ipc::DaemonIpcClient *m_daemonIpcClient = nullptr;

  LogDock *m_logDock;
  StatusBar *m_statusBar = nullptr;

  // Window Menu
  QMenu *m_menuFile = nullptr;
  QMenu *m_menuEdit = nullptr;
  QMenu *m_menuView = nullptr;
  QMenu *m_menuHelp = nullptr;

  // Window Actions
  QAction *m_actionAbout = nullptr;
  QAction *m_actionMinimize = nullptr;
  QAction *m_actionQuit = nullptr;
  QAction *m_actionTrayQuit = nullptr;
  QAction *m_actionRestore = nullptr;
  QAction *m_actionSettings = nullptr;
  QAction *m_actionStartCore = nullptr;
  QAction *m_actionRestartCore = nullptr;
  QAction *m_actionStopCore = nullptr;
  QAction *m_actionShowHelp = nullptr;

  // Network monitoring
  NetworkMonitor *m_networkMonitor = nullptr;
  QString m_currentIpAddress;

  // mDNS server discovery (client side)
  deskflow::mdns::MdnsBrowser *m_mdnsBrowser = nullptr;

  // Server IP strategy optimization
  QStringList m_serverStartIPs;
  QString m_serverStartSuggestedIP;
  QUrl m_helpPath;
};
