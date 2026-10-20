/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2024 - 2026 Chris Rizzitello <sithord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2024 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "workspace/DeviceWorkspace.h"
#include "workspace/SettingsWorkspace.h"
#include "workspace/SetupAssistant.h"

#include "Diagnostic.h"
#include "StyleUtils.h"

#include "dialogs/AboutDialog.h"
#include "dialogs/ClientConfigDialog.h"
#include "dialogs/FingerprintDialog.h"
#include "dialogs/HelpDialog.h"
#include "dialogs/ServerBrowserDialog.h"
#include "dialogs/ServerConfigDialog.h"
#include "dialogs/SettingsDialog.h"

#include "common/I18N.h"
#include "common/PlatformInfo.h"
#include "common/Settings.h"
#include "common/UrlConstants.h"
#include "common/VersionInfo.h"
#include "gui/Messages.h"
#include "gui/TlsUtility.h"
#include "gui/core/CoreProcess.h"
#include "gui/ipc/DaemonIpcClient.h"
#include "gui/widgets/LogDock.h"
#include "net/FingerprintDatabase.h"
#include "net/PeerIdentityStore.h"
#include "widgets/StatusBar.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileDialog>
#include <QInputDialog>
#include <QListWidget>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScreen>
#include <QScrollBar>
#include <QTabWidget>

#include <memory>

#if defined(Q_OS_MACOS)
#include <ApplicationServices/ApplicationServices.h>
#endif

using namespace deskflow::gui;

MainWindow::MainWindow(const QString &socketName)
    : ui{std::make_unique<Ui::MainWindow>()},
      m_coreProcess(m_serverConfig),
      m_trayIcon{new QSystemTrayIcon(this)},
      m_guiDupeChecker{new QLocalServer(this)},
      m_daemonIpcClient{new ipc::DaemonIpcClient(this)},
      m_logDock{new LogDock(this)},
      m_statusBar{new StatusBar(this)},
      m_menuFile{new QMenu(this)},
      m_menuEdit{new QMenu(this)},
      m_menuView{new QMenu(this)},
      m_menuHelp{new QMenu(this)},
      m_actionAbout{new QAction(this)},
      m_actionMinimize{new QAction(this)},
      m_actionQuit{new QAction(this)},
      m_actionTrayQuit{new QAction(this)},
      m_actionRestore{new QAction(this)},
      m_actionSettings{new QAction(this)},
      m_actionStartCore{new QAction(this)},
      m_actionRestartCore{new QAction(this)},
      m_actionStopCore{new QAction(this)},
      m_actionShowHelp{new QAction(this)},
      m_networkMonitor{new NetworkMonitor(this)}
{
  ui->setupUi(this);

  setWindowIcon(QIcon::fromTheme(kRevFqdnName));

  addDockWidget(Qt::BottomDockWidgetArea, m_logDock);

  // Setup Actions
  m_actionAbout->setMenuRole(QAction::AboutRole);
  m_actionAbout->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::HelpAbout));

  m_actionMinimize->setIcon(QIcon::fromTheme(QStringLiteral("window-minimize-pip")));
  m_actionRestore->setIcon(QIcon::fromTheme(QStringLiteral("window-restore-pip")));

  if (!deskflow::platform::isWindows()) {
    m_actionQuit->setShortcut(QKeySequence::Quit);
    m_actionTrayQuit->setShortcut(QKeySequence::Quit);
  }

  m_actionQuit->setIcon(QIcon::fromTheme("application-exit"));
  m_actionQuit->setMenuRole(QAction::QuitRole);

  m_actionTrayQuit->setIcon(QIcon::fromTheme("application-exit"));
  m_actionTrayQuit->setMenuRole(QAction::NoRole);

  m_actionSettings->setIcon(QIcon::fromTheme(QStringLiteral("configure")));
  m_actionSettings->setMenuRole(QAction::PreferencesRole);
  m_actionSettings->setShortcut(QKeySequence::Preferences);

  m_actionStartCore->setIcon(QIcon::fromTheme(QStringLiteral("system-run")));
  m_actionStartCore->setMenuRole(QAction::NoRole);

  m_actionRestartCore->setVisible(false);
  m_actionRestartCore->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
  m_actionRestartCore->setMenuRole(QAction::NoRole);

  m_actionStopCore->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::ProcessStop));
  m_actionStopCore->setMenuRole(QAction::NoRole);

  m_actionShowHelp->setIcon(QIcon::fromTheme(QStringLiteral("question")));
  m_actionShowHelp->setMenuRole(QAction::NoRole);
  m_actionShowHelp->setShortcut(QKeySequence::HelpContents);

  // Setup the Instance Checking
  // In case of a previous crash remove first
  const auto guiSocket = socketName.isEmpty() ? m_guiSocketName : socketName;
  QLocalServer::removeServer(guiSocket);
  m_guiDupeChecker->listen(guiSocket);

  createMenuBar();
  setupControls();
  updateText();
  connectSlots();
  setupTrayIcon();
  updateComputerName();
  setHelpFilePath();

  qDebug().noquote() << "active settings path:" << Settings::settingsPath();

  if (TlsUtility::isEnabled() && TlsUtility::isCertValid())
    m_fingerprint = {QCryptographicHash::Sha256, TlsUtility::certFingerprint()};

  m_activeSource = Settings::value(Settings::Client::RemoteHost).toString();
  m_activeExternalConfig = Settings::value(Settings::Server::ExternalConfig).toBool();
  m_coreProcess.setMode(Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>());
  applyConfig();
  m_statusBar->setSecurityIcon(TlsUtility::isEnabled());
  restoreWindow();
  setupWorkspace();

#ifdef Q_OS_MACOS
  // Native quit is explicit; only closing the window can hide it.
  installQuitHandler([this] {
    requestQuit();
    return false;
  });
#endif
}
MainWindow::~MainWindow()
{
  // Stop network monitoring
  if (m_networkMonitor) {
    m_networkMonitor->stopMonitoring();
  }

  m_guiDupeChecker->close();
  m_coreProcess.cleanup();
}

void MainWindow::restoreWindow()
{
  auto windowGeometry = Settings::value(Settings::Gui::WindowGeometry).toRect();
  const auto totalGeometry = QGuiApplication::primaryScreen()->availableGeometry();
  if (!windowGeometry.isValid()) {
    adjustSize();
    windowGeometry = geometry();
  } else {
    setGeometry(windowGeometry);
  }
  m_expandedSize = geometry().size();

  if (!totalGeometry.contains(windowGeometry)) {
    QRect screenGeometry = QGuiApplication::primaryScreen()->geometry();
    move(screenGeometry.center() - rect().center());
  }

  if (m_pages) {
    setMinimumSize(800, 600);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
  }
}

void MainWindow::setupControls()
{
  secureSocket(false);

  ui->btnConfigureServer->setIcon(QIcon::fromTheme(QStringLiteral("configure")));
  ui->btnConfigureClient->setIcon(QIcon::fromTheme(QStringLiteral("configure")));

  if (!Settings::value(Settings::Gui::LogExpanded).toBool()) {
    m_logDock->hide();
  }

  ui->serverOptions->setVisible(false);
  ui->clientOptions->setVisible(false);

  const auto coreMode = Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>();
  ui->rbModeClient->setChecked(coreMode == Settings::CoreMode::Client);
  ui->rbModeServer->setChecked(coreMode == Settings::CoreMode::Server);

  ui->lineEditName->setValidator(new QRegularExpressionValidator(m_nameRegEx, this));
  ui->lineEditName->setVisible(false);
  ui->lineEditName->installEventFilter(this);

  if (deskflow::platform::isMac()) {
    ui->rbModeServer->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->rbModeClient->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->btnSaveServerConfig->setFixedWidth(ui->btnSaveServerConfig->height());
  } else {
    ui->btnSaveServerConfig->setIconSize(QSize(22, 22));
  }
  setStatusBar(m_statusBar);
}

//////////////////////////////////////////////////////////////////////////////
// Begin slots
//////////////////////////////////////////////////////////////////////////////

// remember: using queued connection allows the render loop to run before
// executing the slot. the default is to instantly call the slot when the
// signal is emitted from the thread that owns the receiver's object.
void MainWindow::connectSlots()
{
  connect(Settings::instance(), &Settings::serverSettingsChanged, this, &MainWindow::serverConfigSaving);
  connect(Settings::instance(), &Settings::settingsChanged, this, &MainWindow::settingsChanged);

  connect(&m_coreProcess, &CoreProcess::error, this, &MainWindow::coreProcessError);
  connect(&m_coreProcess, &CoreProcess::logLine, this, &MainWindow::handleLogLine);
  connect(
      &m_coreProcess, &CoreProcess::processStateChanged, this, &MainWindow::coreProcessStateChanged,
      Qt::QueuedConnection
  );
  connect(&m_coreProcess, &CoreProcess::connectionStateChanged, this, &MainWindow::coreConnectionStateChanged);
  connect(&m_coreProcess, &CoreProcess::secureSocket, this, &MainWindow::secureSocket);
  connect(
      &m_coreProcess, &CoreProcess::daemonIpcClientConnectionFailed, this, &MainWindow::daemonIpcClientConnectionFailed
  );
  connect(&m_coreProcess, &CoreProcess::securityLevelChanged, m_statusBar, &StatusBar::setSecurityLevel);

  connect(m_actionAbout, &QAction::triggered, this, &MainWindow::openAboutDialog);
  connect(m_actionMinimize, &QAction::triggered, this, &MainWindow::hide);

  connect(m_actionQuit, &QAction::triggered, this, &MainWindow::requestQuit);
  connect(m_actionTrayQuit, &QAction::triggered, this, &MainWindow::requestQuit);
  connect(m_actionRestore, &QAction::triggered, this, &MainWindow::showAndActivate);
  connect(m_actionSettings, &QAction::triggered, this, &MainWindow::openSettings);
  connect(m_actionStartCore, &QAction::triggered, this, &MainWindow::startCore);
  connect(m_actionRestartCore, &QAction::triggered, this, &MainWindow::resetCore);
  connect(m_actionStopCore, &QAction::triggered, this, &MainWindow::stopCore);
  connect(m_actionShowHelp, &QAction::triggered, this, &MainWindow::showHelpViewer);

  // Mac os tray will only show a menu
  if (!deskflow::platform::isMac())
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &MainWindow::trayIconActivated);

  connect(&m_coreProcess, &CoreProcess::connectedClientsChanged, this, &MainWindow::serverClientsChanged);
  connect(&m_coreProcess, &CoreProcess::unrecognisedClient, this, &MainWindow::handleUnrecognisedClient);
  connect(&m_coreProcess, &CoreProcess::connectionRefused, this, &MainWindow::handleConnectionRefused);
  connect(&m_coreProcess, &CoreProcess::retryIn, this, &MainWindow::updateTimeoutDelay);
  connect(&m_coreProcess, &CoreProcess::peerFingerprint, this, &MainWindow::handlePeerFingerprint);
  connect(&m_coreProcess, &CoreProcess::peerIdentityRequired, this, &MainWindow::handlePeerIdentity);
  connect(&m_coreProcess, &CoreProcess::missingKeyboardLayouts, this, &MainWindow::handleMissingKeyboardLayouts);

  if (Settings::value(Settings::Gui::AutoStartCore).toBool()) {
    connect(ui->btnToggleCore, &QPushButton::clicked, m_actionStopCore, &QAction::trigger, Qt::UniqueConnection);
  } else {
    connect(ui->btnToggleCore, &QPushButton::clicked, m_actionStartCore, &QAction::trigger, Qt::UniqueConnection);
  }

  connect(ui->btnRestartCore, &QPushButton::clicked, this, &MainWindow::resetCore);

  connect(ui->lineHostname, &QLineEdit::returnPressed, ui->btnRestartCore, &QPushButton::click);
  connect(ui->lineHostname, &QLineEdit::textChanged, this, &MainWindow::remoteHostChanged);
  connect(ui->btnBrowseServers, &QPushButton::clicked, this, &MainWindow::browseServers);

  connect(ui->btnSaveServerConfig, &QPushButton::clicked, this, &MainWindow::saveServerConfig);
  connect(ui->btnConfigureServer, &QPushButton::clicked, this, [this] { showConfigureServer(""); });
  connect(ui->btnConfigureClient, &QPushButton::clicked, this, [this] { showConfigureClient(); });
  connect(ui->lblComputerName, &QLabel::linkActivated, this, &MainWindow::openSettings);

  connect(ui->rbModeServer, &QRadioButton::toggled, this, &MainWindow::coreModeToggled);
  connect(ui->rbModeClient, &QRadioButton::toggled, this, &MainWindow::coreModeToggled);

  connect(m_logDock->toggleViewAction(), &QAction::toggled, this, &MainWindow::toggleLogVisible);

  connect(m_statusBar, &StatusBar::requestShowMyFingerprints, this, &MainWindow::showMyFingerprint);
  connect(m_statusBar, &StatusBar::requestUpdateVersion, this, &MainWindow::openGetNewVersionUrl);
  connect(&m_versionChecker, &VersionChecker::updateFound, m_statusBar, &StatusBar::updateFound);

  connect(m_guiDupeChecker, &QLocalServer::newConnection, this, &MainWindow::showAndActivate);

  connect(ui->btnEditName, &QPushButton::clicked, this, &MainWindow::showHostNameEditor);

  connect(ui->lineEditName, &QLineEdit::editingFinished, this, &MainWindow::setHostName);

  connect(m_networkMonitor, &NetworkMonitor::ipAddressesChanged, this, &MainWindow::updateIpLabel);
}

void MainWindow::toggleLogVisible(bool visible)
{
  // When the main window is hidden e.g. close to tray / minimized), this also triggers the log visibility toggle,
  // but we don't want to hide the log in this case since we would need to un-hide it when the window is shown again.
  if (!isVisible() || isMinimized()) {
    qDebug() << "not toggling log, window not visible";
    return;
  }

  Settings::setValue(Settings::Gui::LogExpanded, visible);
  setMinimumSize(800, 600);
  setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
}

void MainWindow::settingsChanged(const QString &key)
{
  if (key == Settings::Core::Language)
    I18N::setLanguage(Settings::value(key).toString());
  if (key == Settings::Core::ComputerName)
    updateComputerName();
  // Runtime security/log changes are applied explicitly, never by a save signal.
  if (key.startsWith("gui/"))
    applyConfig();
}

void MainWindow::serverConfigSaving()
{
  m_serverConfig.commit();
}

void MainWindow::trayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
  if (reason != QSystemTrayIcon::Trigger)
    return;
  isVisible() ? hide() : showAndActivate();
}

void MainWindow::coreProcessError(CoreProcess::Error error)
{
  if (m_sourceChange.active()) {
    m_sourceChange.failed(m_sourceChange.attempt(), m_sourceChange.expectedSource(), tr("核心启动失败。"));
    return;
  }
  if (m_applyingRevision) {
    m_applicationState.finishApply(m_applyingRevision, false, tr("启动失败，配置未确认生效。请查看诊断。"));
    m_applyingRevision = 0;
  }
  if (error == CoreProcess::Error::AddressMissing) {
    QMessageBox::warning(
        this, tr("Address missing"), tr("Please enter the hostname or IP address of the other computer.")
    );
  } else if (error == CoreProcess::Error::StartFailed) {
    show();
    auto message = tr("The Core executable could not be started.\n"
                      "Please check if you have sufficient permissions to run %1.")
                       .arg(kCoreBinName);

    if (Settings::value(Settings::Core::CoreMode) == Settings::CoreMode::Server) {
      const auto mode =
          Settings::value(Settings::Server::ExternalConfigFile).toBool() ? tr("read") : tr("read and write");
      message.append(tr("\nAdditionally, check you are able to %1 the server config file: %2")
                         .arg(mode, Settings::serverConfigFile()));
    }
    QMessageBox::warning(this, kAppName, message);
  }
}

void MainWindow::startCore()
{
  if (m_identityRegenerating)
    return;
  ++m_trustGeneration;
  m_pendingIdentities.clear();
  m_pendingFingerprints.clear();
  if (!persistConfiguration())
    return;
  m_trustResumeAllowed = true;
  m_checkedServers.clear();
  m_checkedClients.clear();
  // Save current IP state when server starts
  if (m_coreProcess.mode() == CoreMode::Server && Settings::value(Settings::Core::Interface).toString().isEmpty()) {
    m_serverStartIPs = NetworkMonitor::validAddresses();
    m_serverStartSuggestedIP = m_serverStartIPs.isEmpty() ? "" : m_serverStartIPs.first();
  }

  if (!m_coreProcess.prepareConfiguration()) {
    QMessageBox::warning(
        this, tr("配置准备失败"),
        tr("无法准备运行配置。原连接未受影响，请检查配置目录权限。\n%1").arg(m_coreProcess.configurationError())
    );
    return;
  }
  m_coreProcess.setMode(Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>());
  m_coreProcess.setAddress(Settings::value(Settings::Client::RemoteHost).toString());
#ifdef Q_OS_MACOS
  if (!AXIsProcessTrusted()) {
    if (m_navigation)
      m_navigation->setCurrentRow(2);
    QMessageBox::warning(this, tr("需要输入权限"), tr("请在系统设置允许辅助功能访问后，返回重新检测。"));
    return;
  }
#endif
  m_fingerprint = {QCryptographicHash::Sha256, TlsUtility::certFingerprint()};
  updateFingerprintButton();
  m_applicationState.persisted(configurationSnapshot());
  m_activeSource = Settings::value(Settings::Client::RemoteHost).toString();
  m_applyingExternalConfig = Settings::value(Settings::Server::ExternalConfig).toBool();
  if (!m_applyingRevision)
    m_applyingRevision = m_applicationState.beginApply();
  m_actionStartCore->setVisible(false);
  m_actionRestartCore->setVisible(true);
  m_coreProcess.start();
  const auto revision = m_applyingRevision;
  QTimer::singleShot(15000, this, [this, revision] {
    if (revision && revision == m_applyingRevision && !m_sourceChange.active() && !m_verifyingIdentity) {
      m_applicationState.finishApply(revision, false, tr("核心尚未确认启动配置，请检查诊断。"));
      m_applyingRevision = 0;
      persistWorkspaceState();
    }
  });
}

void MainWindow::stopCore()
{
  ++m_trustGeneration;
  m_pendingIdentities.clear();
  m_pendingFingerprints.clear();
  m_setupStartedCore = false;
  m_trustResumeAllowed = false;
  m_sourceChange.abort(tr("已取消来源切换并请求停止全部共享。"));
  if (m_applyingRevision) {
    m_applicationState.finishApply(m_applyingRevision, false, tr("应用已取消，已请求停止共享。"));
    m_applyingRevision = 0;
    persistWorkspaceState();
  }
  if (m_setupAssistant && m_setupAssistant->active())
    m_setupAssistant->cancel();
  qDebug() << "stopping core process";
  m_coreProcess.stop();
  m_actionStartCore->setVisible(true);
  m_actionRestartCore->setVisible(false);
}

void MainWindow::clearSettings()
{
  if (m_identityRegenerating) {
    QMessageBox::information(this, tr("身份更新中"), tr("请等待本机身份生成结束，再清除配置。"));
    return;
  }
  if (!m_clearProfilePending) {
    const auto files = diagnostic::managedProfileFiles();
    QMessageBox preview(this);
    preview.setWindowTitle(tr("清除当前配置"));
    preview.setText(
        tr("将清除当前应用的设备、规则、信任和身份等配置。外部配置文件和系统服务不会被删除。建议先导出配置备份。")
    );
    preview.setDetailedText(files.join("\n"));
    auto *backup = preview.addButton(tr("先导出备份"), QMessageBox::ActionRole);
    auto *clear = preview.addButton(tr("继续清除"), QMessageBox::DestructiveRole);
    auto *cancel = preview.addButton(QMessageBox::Cancel);
    preview.setDefaultButton(cancel);
    preview.exec();
    if (preview.clickedButton() == backup) {
      exportConfigurationBackup();
      return;
    }
    if (preview.clickedButton() != clear)
      return;
    bool accepted = false;
    const auto confirmation = QInputDialog::getText(
        this, tr("确认清除"), tr("输入 CLEAR 确认清除上述应用文件"), QLineEdit::Normal, {}, &accepted
    );
    if (!accepted || confirmation != "CLEAR")
      return;
    m_trustResumeAllowed = false;
    m_sourceChange.abort(tr("为清除配置已取消来源切换。"));
    m_clearProfilePending = true;
  }
  if (m_coreProcess.processState() != ProcessState::Stopped) {
    m_coreProcess.stop();
    QTimer::singleShot(22000, this, [this] {
      if (!m_clearProfilePending)
        return;
      m_clearProfilePending = false;
      QMessageBox::warning(this, tr("未执行清除"), tr("共享停止结果未确认，配置文件保持保留。"));
    });
    return;
  }
  m_networkMonitor->stopMonitoring();
  QStringList failed;
  m_saveOnExit = false;
  if (!diagnostic::clearSettings(false, &failed)) {
    m_clearProfilePending = false;
    QMessageBox::warning(this, tr("清除未完成"), tr("以下文件未能清除，请检查权限后重试：\n%1").arg(failed.join("\n")));
    return;
  }
  m_guiDupeChecker->close();
  QLocalServer::removeServer(m_guiSocketName);
  QProcess::startDetached(QCoreApplication::applicationFilePath(), QCoreApplication::arguments().mid(1));
  QApplication::quit();
}

bool MainWindow::saveServerConfig()
{
  QString fileName = QFileDialog::getSaveFileName(this, tr("Save server configuration as..."));

  if (!fileName.isEmpty() && !m_serverConfig.save(fileName)) {
    QMessageBox::warning(this, tr("Save failed"), tr("Could not save server configuration to file."));
    return true;
  }

  return false;
}

void MainWindow::openAboutDialog()
{
  AboutDialog about(this);
  about.exec();
}

void MainWindow::openGetNewVersionUrl() const
{
  QDesktopServices::openUrl(QUrl(kUrlDownload));
}

void MainWindow::openSettings()
{
  if (m_navigation)
    m_navigation->setCurrentRow(1);
}

void MainWindow::resetCore()
{
  applySavedConfiguration();
}

void MainWindow::showMyFingerprint()
{
  FingerprintDialog fingerprintDialog(this, m_fingerprint);
  fingerprintDialog.exec();
}

void MainWindow::coreModeToggled(bool checked)
{
  // this method is called when rbClient or rbServer toggles
  // with both being in the same group one must be turned on if the other is turned off
  // only react to toggle on to avoid calling everything twice when the user switches modes
  if (!checked)
    return;

  Settings::CoreMode mode = Settings::CoreMode::None;

  if (ui->rbModeServer->isChecked())
    mode = Settings::CoreMode::Server;
  if (ui->rbModeClient->isChecked())
    mode = Settings::CoreMode::Client;

  qDebug() << QStringLiteral("change mode to: %1").arg(QVariant::fromValue(mode).toString());

  Settings::setValue(Settings::Core::CoreMode, mode);
  persistConfiguration();
  updateModeControls();
}

void MainWindow::updateModeControls()
{
  const auto mode = Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>();
  const bool isServer = mode == Settings::CoreMode::Server;
  const bool isClient = mode == Settings::CoreMode::Client;

  ui->serverOptions->setVisible(isServer);
  ui->clientOptions->setVisible(isClient);
  ui->lblNoMode->setVisible(!isServer && !isClient);
  toggleCanRunCore(canRunCore());

  ui->lblIpAddresses->setVisible(
      (isClient && !Settings::value(Settings::Core::Interface).toString().isEmpty()) || isServer
  );

  if (ui->lblIpAddresses->isVisible())
    updateNetworkInfo();

  if (isServer) {
    m_networkMonitor->startMonitoring();
  } else {
    m_networkMonitor->stopMonitoring();
  }

  if (isServer || isClient)
    updateModeControlLabels();
}

void MainWindow::updateModeControlLabels()
{
  const bool isServer = m_coreProcess.mode() == CoreMode::Server;
  const bool isStarted = m_coreProcess.isStarted();

  QString startText;
  QString stopText;
  QIcon startIcon;
  QIcon stopIcon;

  if (isServer) {
    startText = tr("Start");
    stopText = tr("Stop");
    startIcon = QIcon::fromTheme(QStringLiteral("system-run"));
    stopIcon = QIcon::fromTheme(QIcon::ThemeIcon::ProcessStop);
  } else {
    startText = tr("Connect");
    stopText = tr("Disconnect");
    startIcon = QIcon::fromTheme(QStringLiteral("network-connect"));
    stopIcon = QIcon::fromTheme(QStringLiteral("network-disconnect"));
  }

  m_actionStartCore->setText(startText);
  m_actionStartCore->setIcon(startIcon);
  m_actionStopCore->setText(stopText);
  m_actionStopCore->setIcon(stopIcon);

  if (isStarted) {
    ui->btnToggleCore->setText(stopText);
    ui->btnToggleCore->setIcon(stopIcon);
  } else {
    ui->btnToggleCore->setText(startText);
    ui->btnToggleCore->setIcon(startIcon);
  }
}

void MainWindow::updateSecurityIcon(bool visible)
{
  m_statusBar->setSecurityIconVisible(visible);
  if (!visible)
    return;
  m_statusBar->setSecurityIcon(TlsUtility::isEnabled());
}

void MainWindow::updateNetworkInfo()
{
  const auto mode = m_coreProcess.mode();
  if (mode == CoreMode::None) {
    return;
  }

  if (mode == CoreMode::Server)
    updateIpLabel(NetworkMonitor::validAddresses());
  else
    updateIpLabel({Settings::value(Settings::Core::Interface).toString()});
}

void MainWindow::serverConnectionConfigureClient(const QString &clientName)
{
  m_serverConfigDialogVisible = true;
  m_setupArranging = true;
  m_navigation->setCurrentRow(0);
  m_deviceModes->setCurrentIndex(0);
  m_devicesWorkspace->addDevice(clientName);
  if (m_devicesWorkspace->hasUnsavedChanges()) {
    if (m_devicesWorkspace->saveLayout()) {
      applyAddedDeviceLayout();
      m_setupArranging = false;
      m_setupAssistant->waitForPeer();
    }
  } else
    m_setupArranging = false;
  refreshWorkspace();
  m_serverConfigDialogVisible = false;
}

//////////////////////////////////////////////////////////////////////////////
// End slots
//////////////////////////////////////////////////////////////////////////////

void MainWindow::open()
{
  if (!QSystemTrayIcon::isSystemTrayAvailable() || !Settings::value(Settings::Gui::Autohide).toBool())
    showAndActivate();
  else if (deskflow::platform::isMac())
    // macOS to call hide after this function ends
    QTimer::singleShot(1, this, &MainWindow::hide);
  else
    hide();

  // if a critical error was shown just before the main window (i.e. on app
  // load), it will be hidden behind the main window. therefore we need to raise
  // it up in front of the main window.
  // HACK: because the `onShown` event happens just as the window is shown, the
  // message box has a chance of being raised under the main window. to solve
  // this we delay the error dialog raise by a split second. this seems a bit
  // hacky and fragile, so maybe there's a better approach.
  const auto kCriticalDialogDelay = 100;
  QTimer::singleShot(kCriticalDialogDelay, this, &messages::raiseCriticalDialog);

  if (!Settings::value(Settings::Gui::AutoUpdateCheck).isValid()) {
    Settings::setValue(Settings::Gui::AutoUpdateCheck, false);
    Settings::save(false);
  }

  if (Settings::value(Settings::Gui::AutoUpdateCheck).toBool()) {
    m_versionChecker.checkLatest();
  } else {
    qDebug() << "skipping check for new version, disabled";
  }

  const bool autoConnect = ui->rbModeClient->isChecked() && Settings::value(Settings::Client::AutoConnect).toBool();
  const bool autoStart = Settings::value(Settings::Gui::AutoStartCore).toBool() || autoConnect;
#ifdef Q_OS_WIN
  if (Settings::value(Settings::Core::ProcessMode).value<Settings::ProcessMode>() == Settings::ProcessMode::Service) {
    connect(
        &m_coreProcess, &CoreProcess::serviceProbeFinished, this,
        [this, autoStart](const QString &state) {
          if (state == "stopped" && autoStart)
            startCore();
          else if (state == "unknown") {
            m_lastOperationError = tr("后台服务状态未确认，未自动重启共享。请检查诊断后明确启动。");
            refreshWorkspace();
          }
        },
        Qt::SingleShotConnection
    );
    m_coreProcess.probeExistingService();
    return;
  }
#endif
  if (Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>() == CoreMode::None) {
    showSetupAssistant();
    return;
  }
  if (autoStart) {
    if (ui->rbModeClient->isChecked() && ui->lineHostname->text().isEmpty())
      return;
    startCore();
  }
}

void MainWindow::createMenuBar()
{
  m_menuFile->addAction(m_actionStartCore);
  m_menuFile->addAction(m_actionRestartCore);
  m_menuFile->addAction(m_actionStopCore);
  m_menuFile->addSeparator();
  m_menuFile->addAction(m_actionQuit);

  m_menuEdit->addAction(m_actionSettings);

  m_menuView->addAction(m_logDock->toggleViewAction());

  m_menuHelp->addAction(m_actionAbout);
  m_menuHelp->addAction(m_actionShowHelp);

  auto menuBar = new QMenuBar(this);
  menuBar->addMenu(m_menuFile);
  menuBar->addMenu(m_menuEdit);
  menuBar->addMenu(m_menuView);
  menuBar->addMenu(m_menuHelp);

  setMenuBar(menuBar);
}

void MainWindow::setupTrayIcon()
{
  auto trayMenu = new QMenu(this);
  trayMenu->addActions(
      {m_actionStartCore, m_actionRestartCore, m_actionStopCore, m_actionMinimize, m_actionRestore, m_actionTrayQuit}
  );
  trayMenu->insertSeparator(m_actionMinimize);
  trayMenu->insertSeparator(m_actionTrayQuit);
  m_trayIcon->setContextMenu(trayMenu);

  setTrayIcon();
  m_trayIcon->show();
}

void MainWindow::applyConfig()
{
  if (Settings::value(Settings::Gui::ShowVersionInTitle).toBool()) {
    setWindowTitle(QStringLiteral("%1 - %2").arg(kAppName, kDisplayVersion));
  } else {
    setWindowTitle(kAppName);
  }

  if (const auto host = Settings::value(Settings::Client::RemoteHost).toString(); !host.isEmpty())
    ui->lineHostname->setText(host);

  updateFingerprintButton();
  setTrayIcon();

  if (const auto ip = Settings::value(Settings::Core::Interface).toString(); !ip.isEmpty()) {
    m_serverStartIPs = {ip};
    m_serverStartSuggestedIP = ip;
  }

  updateModeControls();
}

void MainWindow::saveSettings() const
{
  if (ui->rbModeClient->isChecked()) {
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Client);
  } else if (ui->rbModeServer->isChecked()) {
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Server);
  }
  if (!ui->lineHostname->text().isEmpty())
    Settings::setValue(Settings::Client::RemoteHost, ui->lineHostname->text());
  Settings::save();
}

void MainWindow::setTrayIcon()
{
  static const auto fallbackPath = QStringLiteral(":/icons/%1-%2/apps/64/%3");

  QString themeIcon = kRevFqdnName;
  if (!Settings::value(Settings::Gui::SymbolicTrayIcon).toBool()) {
    if (deskflow::platform::isMac())
      m_trayIcon->setIcon(QIcon::fromTheme(themeIcon));
    else
      m_trayIcon->setIcon(QIcon(fallbackPath.arg(kAppId, QStringLiteral("dark"), themeIcon)));
    return;
  }

  themeIcon.append(QStringLiteral("-symbolic"));

  if (deskflow::platform::isWindows()) {
    QSettings settings(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat
    );
    const QString theme = settings.value(QStringLiteral("SystemUsesLightTheme"), 1).toBool() ? QStringLiteral("light")
                                                                                             : QStringLiteral("dark");
    m_trayIcon->setIcon(QIcon(fallbackPath.arg(kAppId, theme, themeIcon)));
    return;
  }

  auto icon = QIcon::fromTheme(themeIcon, QIcon(fallbackPath.arg(kAppId, iconMode(), themeIcon)));
  icon.setIsMask(true);
  m_trayIcon->setIcon(icon);
}

void MainWindow::handleLogLine(const QString &line)
{
  m_logDock->appendLine(line);
}

void MainWindow::handleUnrecognisedClient(const QString &clientName)
{
  if (m_coreProcess.mode() != CoreMode::Server || m_ignoredClients.contains(clientName) ||
      m_serverConfigDialogVisible || m_serverConfig.computerExists(clientName))
    return;
  if (m_activeExternalConfig) {
    m_lastOperationError = tr("有电脑请求连接，但当前排列由外部文件管理。请先把连接标识加入文件并明确应用。");
    refreshWorkspace();
    return;
  }
  if (!m_setupAssistant->active()) {
    if (m_devicesWorkspace->hasUnsavedChanges() || m_settingsWorkspace->hasUnsavedChanges()) {
      m_lastOperationError = tr("有新电脑请求连接，请先保存或取消当前草稿，再继续添加电脑。");
      refreshWorkspace();
      return;
    }
    beginSetupAssistant(false);
  }
  if (m_setupAssistant->showIncomingDevice(clientName)) {
    m_setupArranging = false;
    refreshWorkspace();
    if (!isVisible() && m_trayIcon->isVisible() && QSystemTrayIcon::supportsMessages())
      m_trayIcon->showMessage(tr("新电脑等待设置"), tr("请打开 Deskflow，在工作区中核对身份并保留位置。"));
  }
}

void MainWindow::handleConnectionRefused(deskflow::core::ConnectionRefusal reason)
{
  if (reason != deskflow::core::ConnectionRefusal::AlreadyConnected)
    return;

  if (m_sourceChange.active()) {
    m_sourceChange.failed(
        m_sourceChange.attempt(), m_sourceChange.expectedSource(), tr("来源拒绝连接：此电脑标识已有连接。")
    );
    return;
  }
  if (!isVisible() || m_clientErrorVisible)
    return;

  m_clientErrorVisible = true;
  showAndActivate();

  const auto address = Settings::value(Settings::Client::RemoteHost).toString();
  QMessageBox::warning(
      this, tr("%1 Connection Error").arg(kAppName),
      tr("<p>Failed to connect to the server '%1'.</p>"
         "<p>A Client with your name is already connected to the server.</p>"
         "Please ensure that you're using a unique name and that only a "
         "single instance of the client process is running.</p>")
          .arg(address)
  );

  m_clientErrorVisible = false;
}

void MainWindow::handleMissingKeyboardLayouts(const QString &layouts)
{
  if (Settings::value(Settings::Gui::IgnoreMissingKeyboardLayouts).toBool())
    return;

  QMessageBox msgBox(this);
  msgBox.setIcon(QMessageBox::Warning);
  msgBox.setWindowTitle(tr("Missing Keyboard Layouts"));
  msgBox.setText(tr("<p>Keyboard layout support requires matching layouts on all computers. "
                    "The following layouts from the other computer are not installed on this computer:</p>"
                    "<p><b>%1</b></p>"
                    "<p>Please install them to enable support for these layouts.</p>")
                     .arg(layouts));

  auto *checkBox = new QCheckBox(tr("Don't show this again"), &msgBox);
  msgBox.setCheckBox(checkBox);
  msgBox.exec();

  if (checkBox->isChecked()) {
    Settings::setValue(Settings::Gui::IgnoreMissingKeyboardLayouts, true);
  }
}

void MainWindow::advanceIdentityQueue()
{
  QTimer::singleShot(0, this, [this] {
    if (m_setupAssistant->identityPending())
      return;
    if (!m_pendingIdentities.isEmpty())
      handlePeerIdentity(m_pendingIdentities.takeFirst());
    else if (!m_pendingFingerprints.isEmpty())
      handlePeerFingerprint(m_pendingFingerprints.takeFirst());
  });
}

void MainWindow::handlePeerIdentity(const QJsonObject &identity)
{
  const auto role = identity.value("role").toString();
  const auto key = identity.value("key").toString();
  const auto identifier = identity.value("identifier").toString();
  const auto hash = identity.value("fingerprint").toString().toLatin1();
  const auto previous = identity.value("previous").toString().toLatin1();
  if ((role != "client" && role != "server") || key.isEmpty() || hash.size() != 64 ||
      QByteArray::fromHex(hash).toHex() != hash)
    return;
  if (!identity.value("available").toBool()) {
    m_lastOperationError = tr("设备身份记录不可读，已阻止连接。请检查配置文件与权限，不能以重新确认覆盖损坏记录。");
    refreshWorkspace();
    return;
  }
  if (m_setupAssistant->identityPending()) {
    if (m_pendingIdentities.size() < 16 && !m_pendingIdentities.contains(identity))
      m_pendingIdentities.append(identity);
    return;
  }
  PeerIdentityStore store(Settings::peerIdentityDb());
  if (store.check(role, key, hash) == PeerIdentityStore::Status::Match)
    return;
  const bool receiver = role == "server";
  if (!m_setupAssistant->active()) {
    m_setupStartedCore = false;
    m_setupAssistant->begin(
        !receiver, false, m_connectedDevices, m_activeSource, Settings::value(Settings::Core::Port).toInt()
    );
  }
  if (receiver) {
    m_verifyingIdentity = true;
    m_sourceChangeTimer.stop();
    m_coreProcess.stop();
  }
  const auto source = m_activeSource;
  const auto generation = m_trustGeneration;
  m_setupAssistant->requestIdentity(
      {QCryptographicHash::Sha256, QByteArray::fromHex(hash)}, m_fingerprint,
      [this, role, key, identifier, hash, previous, receiver, source, generation](bool accepted) {
        m_verifyingIdentity = false;
        if (m_sourceChange.active())
          m_sourceChangeTimer.start(45000);
        bool saved = false;
        if (accepted && Settings::isWritable()) {
          const Fingerprint fp{QCryptographicHash::Sha256, QByteArray::fromHex(hash)};
          FingerprintDatabase revoked;
          revoked.read(Settings::tlsRevokedDb());
          FingerprintDatabase trusted;
          trusted.read(receiver ? Settings::tlsTrustedServersDb() : Settings::tlsTrustedClientsDb());
          PeerIdentityStore store(Settings::peerIdentityDb());
          saved = !revoked.isTrusted(fp) && trusted.isTrusted(fp) &&
                  store.accept({role, key, identifier, {}, hash}, previous);
          if (!saved)
            m_lastOperationError =
                tr("设备身份绑定未能保存，连接保持阻断。记录可能已变化或配置不可写，请重新发起核对。");
        }
        if (saved && receiver && m_trustResumeAllowed && source == m_activeSource) {
          m_sourcePhaseStarted = false;
          auto resume = [this, source, generation] {
            if (m_trustResumeAllowed && source == m_activeSource && generation == m_trustGeneration)
              m_coreProcess.start();
          };
          if (m_coreProcess.processState() == ProcessState::Stopped)
            QTimer::singleShot(0, this, resume);
          else {
            auto connection = std::make_shared<QMetaObject::Connection>();
            *connection = connect(
                &m_coreProcess, &CoreProcess::processStateChanged, this,
                [this, connection, resume](ProcessState state) {
                  if (state != ProcessState::Stopped)
                    return;
                  disconnect(*connection);
                  QTimer::singleShot(0, this, resume);
                }
            );
          }
        } else if (!saved && receiver && m_sourceChange.active())
          m_sourceChange.failed(
              m_sourceChange.attempt(), m_sourceChange.expectedSource(),
              accepted ? tr("设备身份绑定保存失败。") : tr("未接受来源设备身份。")
          );
        m_trustDigest.clear();
        refreshWorkspace();
        advanceIdentityQueue();
        return saved;
      }
  );
  m_setupAssistant->setIdentityContext(identifier, previous);
  m_setupArranging = false;
  m_navigation->setCurrentRow(0);
  m_deviceModes->setCurrentIndex(2);
}

void MainWindow::handlePeerFingerprint(const QString &fingerprint)
{
  const auto sha256Text = QString(fingerprint).remove(':');
  const Fingerprint sha256 = {QCryptographicHash::Sha256, QByteArray::fromHex(sha256Text.toLatin1())};

  if (sha256.data.size() != 32 || sha256Text.size() != 64)
    return;
  if (m_setupAssistant->identityPending()) {
    if (m_pendingFingerprints.size() < 16 && !m_pendingFingerprints.contains(fingerprint))
      m_pendingFingerprints.append(fingerprint);
    return;
  }
  const bool isClient = m_coreProcess.mode() == CoreMode::Client;
  if ((isClient && m_checkedServers.contains(sha256Text)) || (!isClient && m_checkedClients.contains(sha256Text))) {
    qDebug("ignoring fingerprint, already handled");
    return;
  }

  FingerprintDatabase revoked;
  revoked.read(Settings::tlsRevokedDb());
  if (revoked.isTrusted(sha256)) {
    m_lastOperationError = tr("此身份已撤销。请在设备信任中主动重新核对后再连接。");
    refreshWorkspace();
    return;
  }
  FingerprintDatabase db;
  db.read(trustedFingerprintDatabase());

  if (db.isTrusted(sha256)) {
    qDebug("fingerprint is trusted");
    return;
  }

  if (isClient) {
    m_verifyingIdentity = true;
    m_sourceChangeTimer.stop();
    m_checkedServers.append(sha256Text);
    m_coreProcess.stop();
  } else {
    m_checkedClients.append(sha256Text);
  }

  const auto databasePath = trustedFingerprintDatabase();
  const auto source = m_activeSource;
  const auto generation = m_trustGeneration;
  if (!m_setupAssistant->active()) {
    m_setupStartedCore = false;
    m_setupAssistant->begin(
        !isClient, false, m_connectedDevices, source, Settings::value(Settings::Core::Port).toInt()
    );
  }
  m_setupAssistant->requestIdentity(
      sha256, m_fingerprint,
      [this, isClient, sha256, sha256Text, databasePath, source, generation](bool accepted) {
        m_verifyingIdentity = false;
        if (m_sourceChange.active())
          m_sourceChangeTimer.start(45000);
        bool saved = false;
        if (accepted) {
          FingerprintDatabase current;
          current.read(databasePath);
          current.addTrusted(sha256);
          saved = current.write(databasePath);
          if (!saved) {
            m_lastOperationError = tr("无法保存核对后的身份，未允许本次连接。请检查配置路径和权限。");
            if (isClient)
              m_coreProcess.stop();
          }
        }
        if (isClient)
          m_checkedServers.removeAll(sha256Text);
        else
          m_checkedClients.removeAll(sha256Text);
        if (saved && isClient && m_trustResumeAllowed && source == m_activeSource) {
          m_sourcePhaseStarted = false;
          auto resume = [this, source, generation] {
            if (m_trustResumeAllowed && source == m_activeSource && generation == m_trustGeneration)
              m_coreProcess.start();
          };
          if (m_coreProcess.processState() == ProcessState::Stopped)
            QTimer::singleShot(0, this, resume);
          else {
            auto connection = std::make_shared<QMetaObject::Connection>();
            *connection = connect(
                &m_coreProcess, &CoreProcess::processStateChanged, this,
                [this, connection, resume](ProcessState state) {
                  if (state != ProcessState::Stopped)
                    return;
                  disconnect(*connection);
                  QTimer::singleShot(0, this, resume);
                }
            );
          }
        } else if (isClient && !saved && m_sourceChange.active()) {
          m_sourceChange.failed(
              m_sourceChange.attempt(), m_sourceChange.expectedSource(),
              accepted ? tr("无法保存核对后的身份。") : tr("未接受来源身份。")
          );
        }
        m_trustDigest.clear();
        refreshWorkspace();
        advanceIdentityQueue();
        return saved;
      }
  );
  m_navigation->setCurrentRow(0);
  m_deviceModes->setCurrentIndex(2);
}

bool MainWindow::maybeHideToTray()
{
  if (!Settings::value(Settings::Gui::CloseToTray).toBool() || !QSystemTrayIcon::isSystemTrayAvailable() ||
      !m_trayIcon->isVisible()) {
    return false;
  }

  if (Settings::value(Settings::Gui::CloseReminder).toBool()) {
    messages::showCloseReminder(this);
    Settings::setValue(Settings::Gui::CloseReminder, false);
  }
  Settings::setValue(Settings::Gui::WindowGeometry, geometry());
  qDebug() << "hiding to tray";
  hide();
  return true;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
  if (m_identityRegenerating) {
    m_explicitQuit = false;
    event->ignore();
    QMessageBox::information(this, tr("身份更新中"), tr("请等待本机身份生成结束后再退出。"));
    return;
  }
  if (!m_explicitQuit && event->spontaneous() && maybeHideToTray()) {
    event->ignore();
    return;
  }

  if (!m_discardDraftsOnQuit && m_settingsWorkspace &&
      (m_settingsWorkspace->hasUnsavedChanges() || m_devicesWorkspace->hasUnsavedChanges()) &&
      !qGuiApp->isSavingSession()) {
    if (QMessageBox::question(
            this, tr("未保存的修改"), tr("页面还有草稿。放弃草稿并退出，还是返回继续编辑？"),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel
        ) != QMessageBox::Discard) {
      m_explicitQuit = false;
      event->ignore();
      return;
    }
    m_discardDraftsOnQuit = true;
  }
  if (m_coreProcess.serviceMode() && m_coreProcess.isStarted() && !m_quitWhenStopped && !m_detachUi &&
      !qGuiApp->isSavingSession()) {
    QMessageBox choice(this);
    choice.setWindowTitle(tr("退出 Deskflow"));
    choice.setText(tr("后台服务正在共享输入。请选择退出方式。"));
    auto *stop = choice.addButton(tr("停止共享并退出"), QMessageBox::AcceptRole);
    auto *detach = choice.addButton(tr("仅退出界面（共享继续）"), QMessageBox::DestructiveRole);
    auto *cancel = choice.addButton(QMessageBox::Cancel);
    choice.setDefaultButton(cancel);
    choice.exec();
    if (choice.clickedButton() == detach)
      m_detachUi = true;
    else if (choice.clickedButton() != stop) {
      m_explicitQuit = false;
      m_discardDraftsOnQuit = false;
      event->ignore();
      return;
    }
  }
  if (!m_detachUi && m_coreProcess.processState() != ProcessState::Stopped && !m_quitWhenStopped) {
    event->ignore();
    m_trustResumeAllowed = false;
    m_sourceChange.abort(tr("为退出应用已取消来源切换。"));
    m_quitWhenStopped = true;
    m_coreProcess.stop();
    QTimer::singleShot(22000, this, [this] {
      if (!m_quitWhenStopped)
        return;
      m_quitWhenStopped = false;
      m_explicitQuit = false;
      m_discardDraftsOnQuit = false;
      showAndActivate();
      QMessageBox::warning(this, tr("停止未确认"), tr("无法确认共享已停止。窗口保持打开，请检查诊断并重试。"));
    });
    return;
  }
  if (!m_detachUi && m_coreProcess.processState() != ProcessState::Stopped) {
    event->ignore();
    return;
  }
  if (m_saveOnExit) {
    Settings::setValue(Settings::Gui::WindowGeometry, geometry());
    Settings::setValue(Settings::Gui::AutoStartCore, m_coreProcess.isStarted());
  }
  qDebug() << "quitting application";

  // any connected dock view acitons will be triggered
  // disconnect them before accepting the event
  disconnect(m_logDock->toggleViewAction(), &QAction::toggled, nullptr, nullptr);

  event->accept();
  QApplication::quit();
}

void MainWindow::showFirstConnectedMessage()
{
  if (Settings::value(Settings::Gui::ShownFirstConnectedMessage).toBool())
    return;
  Settings::setValue(Settings::Gui::ShownFirstConnectedMessage, true);
  messages::showFirstConnectedMessage(this);
}

void MainWindow::updateStatus()
{
  using enum ProcessState;
  const auto connection = m_coreProcess.connectionState();
  const auto process = m_coreProcess.processState();
  const bool isServer = (m_coreProcess.mode() == CoreMode::Server);
  if (process == Stopped || process == Started) {
    updateNetworkInfo();
    ui->btnEditName->setVisible(process == Stopped);
  }
  m_statusBar->setStatus(connection, process, isServer);
}

void MainWindow::coreProcessStateChanged(ProcessState state)
{
  using enum ProcessState;
  if (state != Started)
    m_pauseRequest.clear();
  updateStatus();
  if (m_sourceChange.active()) {
    if (state == Started)
      m_sourcePhaseStarted = true;
    else if (state == Stopped && m_sourcePhaseStarted && !m_verifyingIdentity) {
      m_sourcePhaseStarted = false;
      m_sourceChange.failed(m_sourceChange.attempt(), m_sourceChange.expectedSource(), tr("会话核心已停止。"));
    }
  }
  if (state == Started) {
    qDebug() << "recording that core has started";
    Settings::setValue(Settings::Gui::AutoStartCore, true);
    if (m_coreProcess.mode() == CoreMode::Server &&
        !Settings::value(Settings::Gui::ShownServerFirstStartMessage).toBool()) {
      qDebug() << "starting core server for first time";
      messages::showFirstServerStartMessage(this);
      Settings::setValue(Settings::Gui::ShownServerFirstStartMessage, true);
    }
  }

  if (state == Started || state == Starting || state == RetryPending) {
    disconnect(ui->btnToggleCore, &QPushButton::clicked, m_actionStartCore, &QAction::trigger);
    connect(ui->btnToggleCore, &QPushButton::clicked, m_actionStopCore, &QAction::trigger, Qt::UniqueConnection);

    ui->btnRestartCore->setEnabled(true);
    m_actionStartCore->setVisible(false);
    m_actionRestartCore->setVisible(true);
    m_actionStopCore->setEnabled(true);

    if (state == Starting) {
      saveSettings();
    }

  } else {
    disconnect(ui->btnToggleCore, &QPushButton::clicked, m_actionStopCore, &QAction::trigger);
    connect(ui->btnToggleCore, &QPushButton::clicked, m_actionStartCore, &QAction::trigger, Qt::UniqueConnection);

    ui->btnRestartCore->setEnabled(false);
    m_actionStartCore->setVisible(true);
    m_actionRestartCore->setVisible(false);
    m_actionStopCore->setEnabled(false);
  }
  if (state == Stopped && m_clearProfilePending) {
    QTimer::singleShot(0, this, &MainWindow::clearSettings);
  }
  if (state == Stopped && m_quitWhenStopped) {
    m_quitWhenStopped = false;
    QTimer::singleShot(0, this, &MainWindow::requestQuit);
  }
  if (state == Stopped) {
    m_connectedDevices.clear();
    m_applicationState.disconnectAll();
  }
  refreshWorkspace();
  updateModeControlLabels();
}

void MainWindow::coreConnectionStateChanged(ConnectionState state)
{
  qDebug() << "core connection state changed:" << static_cast<int>(state);
  if (state == ConnectionState::Connected) {
    m_disconnectNotified = false;
    m_lastOperationError.clear();
  } else if (!m_disconnectNotified && !m_applicationState.sessions().isEmpty() &&
             m_coreProcess.processState() == ProcessState::Started) {
    bool hadConnection = false;
    for (const auto &session : m_applicationState.sessions())
      hadConnection = hadConnection || session.connected;
    if (hadConnection) {
      m_disconnectNotified = true;
      m_lastOperationError = tr("连接已中断，请检查网络、权限和设备身份。安全要求保持不变。");
      if (!isVisible() && m_trayIcon->isVisible() && QSystemTrayIcon::supportsMessages())
        m_trayIcon->showMessage(tr("Deskflow 连接中断"), m_lastOperationError, QSystemTrayIcon::Warning);
    }
  }

  if (m_coreProcess.mode() == CoreMode::Client) {
    const auto name = m_activeSource;
    if (!name.isEmpty()) {
      auto session = m_applicationState.sessions().value(name);
      if (session.attempt.isNull() || (!session.connected && state == ConnectionState::Connected)) {
        m_applicationState.beginSession(name);
        session = m_applicationState.sessions().value(name);
      }
      m_applicationState.sessionConnected(name, session.attempt, state == ConnectionState::Connected);
      if (m_setupAssistant) {
        if (state == ConnectionState::Connected)
          m_setupAssistant->observeConnected(name);
        else
          m_setupAssistant->observeDisconnected(name);
      }
    }
  }
  if (m_sourceChange.active() && !m_verifyingIdentity)
    m_sourceChange.connectionObserved(m_sourceChange.attempt(), m_activeSource, state == ConnectionState::Connected);
  updateStatus();
  refreshWorkspace();

  // always assume connection is not secure when connection changes
  // to anything except connected. the only way the padlock shows is
  // when the correct TLS version string is detected.
  if (state != ConnectionState::Connected) {
    secureSocket(false);
  } else if (isVisible()) {
    showFirstConnectedMessage();
  }
}

void MainWindow::updateFingerprintButton()
{
  m_statusBar->setBtnFingerprintVisible(TlsUtility::isEnabled() && !m_fingerprint.data.isEmpty());
}

void MainWindow::hide()
{
#ifdef Q_OS_MACOS
  macOSNativeHide();
#else
  QMainWindow::hide();
#endif
  m_actionRestore->setVisible(true);
  m_actionMinimize->setVisible(false);
}

void MainWindow::changeEvent(QEvent *e)
{
  QMainWindow::changeEvent(e);
  if (e->type() == QEvent::PaletteChange || e->type() == QEvent::ApplicationPaletteChange) {
    if (m_navigation)
      refreshWorkspaceTheme();
    updateIconTheme();
    setWindowIcon(QIcon::fromTheme(kRevFqdnName));
    setTrayIcon();
  } else if (e->type() == QEvent::LanguageChange) {
    ui->retranslateUi(this);
    updateModeControlLabels();
    updateNetworkInfo();
    updateStatus();
    serverClientsChanged(m_connectedDevices);
    updateText();
  }
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
  if (obj != ui->lineEditName || event->type() != QEvent::KeyPress)
    return false;
  if (const auto keyEvent = static_cast<QKeyEvent *>(event); keyEvent->key() != Qt::Key_Escape)
    return false;
  ui->lineEditName->hide();
  ui->lblComputerName->show();
  ui->btnEditName->show();
  ui->lineEditName->setText(Settings::value(Settings::Core::ComputerName).toString());
  toggleCanRunCore(canRunCore());
  return true;
}

void MainWindow::updateText()
{
  m_menuFile->setTitle(tr("&File"));
  m_menuEdit->setTitle(tr("&Edit"));
  m_menuView->setTitle(tr("&View"));
  m_menuHelp->setTitle(tr("&Help"));

  m_actionMinimize->setText(tr("&Minimize to tray"));
  m_actionQuit->setText(tr("&Quit"));
  m_actionTrayQuit->setText(tr("&Quit"));
  //: %1 will be the replaced with the appname
  m_actionRestore->setText(tr("&Open %1").arg(kAppName));
  m_actionSettings->setText(tr("&Preferences"));
  m_actionStartCore->setText(tr("&Start"));
  m_actionRestartCore->setText(tr("Rest&art"));
  m_actionStopCore->setText(tr("S&top"));
  //: %1 will be the replaced with the appname
  m_actionAbout->setText(tr("About %1...").arg(kAppName));

  m_actionShowHelp->setText(tr("View &Help"));

  //: start / restart core shortcut
  m_actionStartCore->setShortcut(QKeySequence(tr("Ctrl+S")));
  m_actionRestartCore->setShortcut(QKeySequence(tr("Ctrl+S")));

  //: stop core shortcut
  m_actionStopCore->setShortcut(QKeySequence(tr("Ctrl+T")));

  if (deskflow::platform::isWindows()) {
    //: Quit shortcut
    m_actionQuit->setShortcut(QKeySequence(tr("Ctrl+Q")));
    m_actionTrayQuit->setShortcut(QKeySequence(tr("Ctrl+Q")));
  }
}

void MainWindow::showConfigureServer(const QString &message)
{
  if (!m_devicesWorkspace)
    return;
  m_navigation->setCurrentRow(0);
  m_devicesWorkspace->beginEditing();
  if (!message.isEmpty())
    m_lastOperationError = message;
  refreshWorkspace();
}

void MainWindow::showConfigureClient()
{
  navigateSettings("receiver");
}

void MainWindow::secureSocket(bool secureSocket)
{
  m_secureSocket = secureSocket;
  updateSecurityIcon(m_statusBar->securityIconVisible());
}

void MainWindow::updateComputerName()
{
  const auto computerName = Settings::value(Settings::Core::ComputerName).toString();
  ui->lblComputerName->setText(computerName);
  ui->lineEditName->setText(computerName);
  m_serverConfig.updateServerName();
}

void MainWindow::showAndActivate()
{
  const auto wasVisible = isVisible();
#ifdef Q_OS_MACOS
  forceAppActive();
#endif
  showNormal();
  raise();
  activateWindow();
  m_actionRestore->setVisible(false);
  m_actionMinimize->setVisible(true);
  if (!wasVisible)
    restoreWindow();
  refreshWorkspace();
}

void MainWindow::showHostNameEditor()
{
  ui->lineEditName->show();
  ui->lblComputerName->hide();
  ui->btnEditName->hide();
  toggleCanRunCore(false);
  ui->lineEditName->setFocus();
}

void MainWindow::setHostName()
{
  ui->lineEditName->hide();
  ui->lblComputerName->show();
  ui->btnEditName->show();
  toggleCanRunCore(canRunCore());

  QString text = ui->lineEditName->text();
  const auto computerName = Settings::value(Settings::Core::ComputerName).toString();

  if (text == computerName)
    return;

  const bool isServer = ui->rbModeServer->isChecked();
  bool existingComputer = false;
  if (isServer)
    existingComputer = serverConfig().computerExists(text);

  if (!ui->lineEditName->hasAcceptableInput() || text.isEmpty() || existingComputer) {
    blockSignals(true);
    ui->lineEditName->setText(computerName);
    blockSignals(false);

    const auto title = tr("Invalid Computer Name");
    QString body;
    if (existingComputer) {
      body = tr("Computer name already exists");
    } else {
      body =
          tr("The name you have chosen is invalid.\n\n"
             "Valid names:\n"
             "• Use letters and numbers\n"
             "• May also use _ or -\n"
             "• Are between 1 and 255 characters");
    }
    QMessageBox::information(this, title, body);
    return;
  }

  ui->lblComputerName->setText(ui->lineEditName->text());
  Settings::setValue(Settings::Core::ComputerName, ui->lineEditName->text());
  if (isServer)
    serverConfig().updateServerName();
  persistConfiguration();
}

QString MainWindow::trustedFingerprintDatabase() const
{
  const bool isClient = m_coreProcess.mode() == CoreMode::Client;
  return isClient ? Settings::tlsTrustedServersDb() : Settings::tlsTrustedClientsDb();
}

bool MainWindow::generateCertificate()
{
  const auto certificate = Settings::value(Settings::Security::Certificate).toString();
  if (!QFile::exists(certificate) && !TlsUtility::generateCertificate()) {
    return false;
  }

  m_fingerprint = {QCryptographicHash::Sha256, TlsUtility::certFingerprint()};
  updateFingerprintButton();
  return true;
}

void MainWindow::serverClientsChanged(const QStringList &clients)
{
  if (m_coreProcess.mode() != CoreMode::Server || !m_coreProcess.isStarted())
    return;
  for (const auto &name : m_connectedDevices) {
    if (!clients.contains(name)) {
      const auto session = m_applicationState.sessions().value(name);
      m_applicationState.sessionConnected(name, session.attempt, false);
      if (m_setupAssistant)
        m_setupAssistant->observeDisconnected(name);
    }
  }
  m_connectedDevices = clients;
  for (const auto &name : clients) {
    auto session = m_applicationState.sessions().value(name);
    if (session.attempt.isNull() || !session.connected) {
      m_applicationState.beginSession(name);
      session = m_applicationState.sessions().value(name);
    }
    m_applicationState.sessionConnected(name, session.attempt, true);
    if (m_setupAssistant)
      m_setupAssistant->observeConnected(name);
  }
  m_statusBar->setServerClients(clients);
  refreshWorkspace();
}

void MainWindow::daemonIpcClientConnectionFailed()
{
  if (deskflow::gui::messages::showDaemonOffline(this)) {
    m_coreProcess.retryDaemon();
  }
}

void MainWindow::toggleCanRunCore(bool enableButtons)
{
  const bool isStarted = m_coreProcess.isStarted();
  ui->btnToggleCore->setEnabled(enableButtons);
  ui->btnRestartCore->setEnabled(enableButtons && isStarted);
  m_actionStartCore->setEnabled(enableButtons);
  m_actionStopCore->setEnabled(enableButtons && isStarted);
}

void MainWindow::remoteHostChanged(const QString &newRemoteHost)
{
  toggleCanRunCore(!newRemoteHost.isEmpty() && ui->rbModeClient->isChecked());
}

void MainWindow::browseServers()
{
  if (!m_mdnsBrowser) {
    m_mdnsBrowser = new deskflow::mdns::MdnsBrowser(this);
    m_mdnsBrowser->start();
  }

  deskflow::gui::ServerBrowserDialog dialog(m_mdnsBrowser, this);
  if (dialog.exec() == QDialog::Accepted) {
    const QString host = dialog.selectedAddress();
    if (!host.isEmpty()) {
      ui->lineHostname->setText(host);
      Settings::setValue(Settings::Client::AutoConnect, dialog.autoConnectRequested());
      persistConfiguration();
    }
  }
}

void MainWindow::updateIpLabel(const QStringList &addresses)
{
  const auto mode = m_coreProcess.mode();
  if (mode == CoreMode::None) {
    return;
  }

  static const auto colorText = QStringLiteral(R"(<span style="color:%1;">%2</span>)");
  const bool serverStarted = m_coreProcess.isStarted();
  const bool fixedIP =
      !(serverStarted ? m_activeInterface : Settings::value(Settings::Core::Interface).toString()).isEmpty();

  if (!fixedIP && addresses.isEmpty() && !serverStarted || (serverStarted && m_serverStartSuggestedIP.isEmpty())) {
    ui->lblIpAddresses->setText(colorText.arg(palette().linkVisited().color().name(), tr("No IP Detected")));
    ui->lblIpAddresses->setToolTip(tr("Unable to detect an IP address. Check your network connection is active."));
    return;
  }

  QString labelText = fixedIP ? tr("Using IP: ") : tr("Suggested IP: ");
  QString toolTipText = tr("<p>If connecting via the hostname fails, try %1</p>");

  const bool filterIpList = (serverStarted || fixedIP);
  const QRegularExpression ipListFilter(filterIpList ? QStringLiteral("(%1)").arg(m_serverStartIPs.join("|")) : "");
  const QStringList ipList = addresses.filter(ipListFilter);

  bool IPValid = true;
  if (filterIpList && (m_serverStartSuggestedIP != m_currentIpAddress) || !ipList.contains(m_serverStartSuggestedIP)) {
    IPValid = !ipList.isEmpty();
  }

  if (IPValid) {
    m_currentIpAddress = ipList.first();
    labelText.append(m_currentIpAddress);
  } else {
    labelText.append(colorText.arg(palette().linkVisited().color().name(), m_serverStartSuggestedIP));
    toolTipText.append(tr("\nA bound IP is now invalid, you may need to restart the server."));
  }

  if (ipList.count() < 2 || fixedIP) {
    toolTipText = toolTipText.arg(tr("the suggested IP."));
  } else {
    toolTipText = toolTipText.arg(tr("one of the following IPs:<br/>%1").arg(ipList.join("<br/>")));
  }

  ui->lblIpAddresses->setText(labelText);
  ui->lblIpAddresses->setToolTip(mode == CoreMode::Server ? toolTipText : QString());
}

void MainWindow::updateTimeoutDelay(int newDelay)
{
  m_statusBar->setConnectionInterval(newDelay);
}

void MainWindow::setHelpFilePath()
{
  const QString appPath = QCoreApplication::applicationDirPath();
  auto buildPath = QString("%1/../docs/HelpMain.md").arg(appPath);
  auto installPath = QString("%1/../share/doc/%2/HelpMain.md").arg(appPath, kAppId);
  if (deskflow::platform::isMac()) {
    installPath = QString("%1/../Resources/docs/HelpMain.md").arg(appPath);
    buildPath = QString("%1/../../../../docs/HelpMain.md").arg(appPath);
  } else if (deskflow::platform::isWindows()) {
    installPath = QString("%1/docs/HelpMain.md").arg(appPath);
  }

  buildPath = QDir::cleanPath(buildPath);
  installPath = QDir::cleanPath(installPath);

  if (QFile::exists(installPath))
    m_helpPath = QUrl::fromLocalFile(installPath);
  else if (QFile::exists(buildPath))
    m_helpPath = QUrl::fromLocalFile(buildPath);
  else
    m_helpPath = kUrlWiki;
}

void MainWindow::showHelpViewer() const
{
  if (m_helpPath.isLocalFile()) {
    HelpDialog dialogHelp(this->centralWidget(), m_helpPath);
    dialogHelp.exec();
  } else {
    QDesktopServices::openUrl(m_helpPath);
  }
}

bool MainWindow::canRunCore() const
{
  const auto mode = Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>();
  const bool isServer = mode == Settings::CoreMode::Server;
  const bool isClient = mode == Settings::CoreMode::Client;
  return ((isServer || isClient) && (isClient && !ui->lineHostname->text().isEmpty()) || isServer);
}
