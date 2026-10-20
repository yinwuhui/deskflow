// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "MainWindow.h"
#include "common/VersionInfo.h"
#include "dialogs/ClientConfigDialog.h"
#include "dialogs/FingerprintDialog.h"
#include "dialogs/ServerConfigDialog.h"
#include "dialogs/SettingsDialog.h"
#include "gui/TlsUtility.h"
#include "gui/core/ConfigurationBackup.h"
#include "gui/widgets/LogDock.h"
#include "gui/widgets/StatusBar.h"
#include "net/FingerprintDatabase.h"
#include "ui_MainWindow.h"
#include "workspace/DeviceWorkspace.h"
#include "workspace/SettingsWorkspace.h"
#include "workspace/SetupAssistant.h"
#include "workspace/WorkspaceUi.h"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPromise>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyleHints>
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include <QAccessibilityHints>
#endif
#include <QSysInfo>
#include <QTabWidget>
#include <QThreadPool>
#include <QTreeWidget>
#include <memory>
#ifdef Q_OS_MACOS
#include "gui/OSXHelpers.h"
#include <ApplicationServices/ApplicationServices.h>
#endif

using namespace deskflow::gui;

using namespace deskflow::gui::workspace;

void MainWindow::setupWorkspace()
{
  m_paused = Settings::value(Settings::Core::SharingPaused).toBool();
  // Keep the legacy signal adapters alive while the visible hierarchy is rebuilt.
  // They never form part of the new workspace or receive navigation focus.
  auto *legacy = takeCentralWidget();
  legacy->setParent(this);
  legacy->hide();
  auto *root = new QWidget(this);
  root->setObjectName("workspaceRoot");
  auto *outer = new QHBoxLayout(root);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  m_navigation = new Navigation(root);
  m_navigation->setObjectName("workspaceNavigation");
  outer->addWidget(m_navigation);
  auto *body = new QWidget(root);
  auto *vertical = new QVBoxLayout(body);
  vertical->setContentsMargins(26, 24, 26, 20);
  vertical->setSpacing(20);
  auto *session = new QHBoxLayout;
  session->setSpacing(16);
  auto *summary = new QVBoxLayout;
  summary->setSpacing(6);
  m_workspaceStatus = label({}, "subtitle");
  m_workspaceStatus->setObjectName("workspaceStatus");
  m_sessionDetail = label({}, "muted");
  summary->addWidget(m_workspaceStatus);
  summary->addWidget(m_sessionDetail);
  session->addLayout(summary, 1);
  m_addComputer = button(tr("添加电脑"), true);
  m_addComputer->setObjectName("addComputer");
  m_pauseSharing = button(tr("暂停共享"));
  m_pauseSharing->setObjectName("pauseSharing");
  session->addWidget(m_addComputer);
  session->addWidget(m_pauseSharing);
  vertical->addLayout(session);
  auto *divider = new QFrame;
  divider->setProperty("separator", true);
  vertical->addWidget(divider);
  m_pages = new QStackedWidget(body);
  m_pages->setObjectName("workspacePages");
  vertical->addWidget(m_pages, 1);
  m_pendingPanel = new QWidget(body);
  m_pendingPanel->setProperty("warning", true);
  auto *pending = new QHBoxLayout(m_pendingPanel);
  pending->setContentsMargins(14, 12, 14, 12);
  pending->setSpacing(12);
  m_configStatus = label({}, "body");
  pending->addWidget(m_configStatus, 1);
  m_applySaved = button(tr("应用已保存配置"), true);
  m_applySaved->setObjectName("applySavedConfiguration");
  pending->addWidget(m_applySaved);
  m_cancelSourceChange = button(tr("取消切换并恢复旧来源"));
  pending->addWidget(m_cancelSourceChange);
  vertical->addWidget(m_pendingPanel);
  outer->addWidget(body, 1);
  setCentralWidget(root);
  m_statusBar->hide();
  m_deviceModes = new QStackedWidget;
  m_devicesWorkspace = new DeviceWorkspace(m_serverConfig, [this] { return persistConfiguration(); });
  m_deviceModes->addWidget(workspace::scroll(m_devicesWorkspace));
  auto *receiver = page(tr("接收端设备"), tr("本机接收另一台电脑的键盘与鼠标。完整排列由来源端管理。"));
  m_receiverSource = label({}, "subtitle");
  auto *sourcePanel = panel();
  auto *sourceLayout = new QVBoxLayout(sourcePanel);
  sourceLayout->setContentsMargins(18, 18, 18, 18);
  sourceLayout->addWidget(m_receiverSource);
  sourceLayout->addWidget(label(tr("连接与输入验证独立记录，身份信息可在设备信任中核对。"), "muted"));
  contents(receiver)->addWidget(sourcePanel);
  auto *relation = panel();
  auto *relationLayout = new QHBoxLayout(relation);
  relationLayout->setContentsMargins(28, 32, 28, 32);
  relationLayout->addWidget(label(tr("键鼠来源"), "subtitle"), 1, Qt::AlignCenter);
  relationLayout->addWidget(label("→", "title"));
  relationLayout->addWidget(label(tr("这台电脑"), "subtitle"), 1, Qt::AlignCenter);
  contents(receiver)->addWidget(relation);
  contents(receiver)->addWidget(label(tr("尚未获得来源端完整拓扑；这里不推测其他电脑的位置。"), "muted"));
  auto *receiverActions = new QHBoxLayout;
  receiverActions->addStretch();
  auto *receiverSettings = button(tr("接收端输入选项"));
  auto *receiverTrust = button(tr("管理设备信任"));
  auto *verify = button(tr("验证实际输入"));
  receiverActions->addWidget(receiverSettings);
  receiverActions->addWidget(receiverTrust);
  receiverActions->addWidget(verify);
  contents(receiver)->addLayout(receiverActions);
  contents(receiver)->addStretch();
  m_deviceModes->addWidget(workspace::scroll(receiver));
  m_setupAssistant = new SetupAssistant;
  m_deviceModes->addWidget(m_setupAssistant);
  setupAssistantSignals();
  m_pages->addWidget(m_deviceModes);
  m_settingsWorkspace = new SettingsWorkspace;
  m_settingsWorkspace->installPage("rules", m_devicesWorkspace->createRulesPage());
  m_settingsWorkspace->installPage("trust", createTrustPage());
  m_pages->addWidget(m_settingsWorkspace);
  m_diagnosticsPages = new QStackedWidget;
  m_diagnosticsPages->addWidget(workspace::scroll(createDiagnosticsPage()));
  m_diagnosticsPages->addWidget(workspace::scroll(createLogsPage()));
  m_pages->addWidget(m_diagnosticsPages);
  m_pages->addWidget(workspace::scroll(createHelpPage()));
  connect(m_navigation, &Navigation::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
  connect(m_navigation, &Navigation::currentRowChanged, this, [this](int index) {
    if (index == 2)
      m_diagnosticsPages->setCurrentIndex(0);
    refreshWorkspace();
  });
  connect(m_addComputer, &QPushButton::clicked, this, &MainWindow::showSetupAssistant);
  connect(m_applySaved, &QPushButton::clicked, this, &MainWindow::applySavedConfiguration);
  connect(receiverSettings, &QPushButton::clicked, this, [this] { navigateSettings("receiver"); });
  connect(receiverTrust, &QPushButton::clicked, this, &MainWindow::showTrust);
  connect(verify, &QPushButton::clicked, this, &MainWindow::verifySelectedInput);
  connect(m_devicesWorkspace, &DeviceWorkspace::requestConfiguration, this, [this] { navigateSettings("config"); });
  connect(m_devicesWorkspace, &DeviceWorkspace::requestTrust, this, &MainWindow::showTrust);
  connect(m_devicesWorkspace, &DeviceWorkspace::requestVerify, this, [this](const QString &) {
    verifySelectedInput();
  });
  connect(m_settingsWorkspace, &SettingsWorkspace::requestGraphExport, this, &MainWindow::exportGraphicalConfiguration);
  connect(m_settingsWorkspace, &SettingsWorkspace::requestRoutingExport, this, &MainWindow::exportRoutingConfiguration);
  connect(m_settingsWorkspace, &SettingsWorkspace::requestTrust, this, &MainWindow::refreshTrustPage);
  connect(m_settingsWorkspace, &SettingsWorkspace::requestFingerprint, this, &MainWindow::showMyFingerprint);
  connect(m_settingsWorkspace, &SettingsWorkspace::requestClear, this, &MainWindow::clearSettings);
  connect(m_settingsWorkspace, &SettingsWorkspace::requestBackup, this, &MainWindow::exportConfigurationBackup);
  connect(m_settingsWorkspace, &SettingsWorkspace::requestRestore, this, &MainWindow::restoreConfigurationBackup);
  connect(m_settingsWorkspace, &SettingsWorkspace::requestRoleChange, this, [this] { beginSetupAssistant(true); });
  connect(m_settingsWorkspace, &SettingsWorkspace::saved, this, [this](const QString &) {
    ui->lineHostname->setText(Settings::value(Settings::Client::RemoteHost).toString());
    persistConfiguration();
  });
  connect(
      m_settingsWorkspace, &SettingsWorkspace::requestValidateFile, &m_coreProcess, &CoreProcess::validateExternalFile
  );
  connect(
      &m_coreProcess, &CoreProcess::fileValidationFinished, m_settingsWorkspace,
      &SettingsWorkspace::setFileValidationResult
  );
  connect(
      m_settingsWorkspace, &SettingsWorkspace::requestIdentityGeneration, this, &MainWindow::regenerateLocalIdentity
  );
  connect(&m_coreProcess, &CoreProcess::peerIdentityBlocked, this, [this] {
    m_lastOperationError = tr("已阻止被撤销身份重新连接。请在设备信任中主动重新核对。");
    refreshWorkspace();
  });
  connect(&m_applicationState, &ApplicationState::changed, this, &MainWindow::refreshWorkspace);
  auto *tray = m_trayIcon->contextMenu();
  const auto first = tray->actions().first();
  m_traySummary = new QAction(this);
  m_traySummary->setEnabled(false);
  tray->insertAction(first, m_traySummary);
  m_trayConnections = new QAction(this);
  m_trayConnections->setEnabled(false);
  tray->insertAction(first, m_trayConnections);
  m_trayProblem = new QAction(tr("查看需要处理的问题…"), this);
  tray->insertAction(first, m_trayProblem);
  connect(m_trayProblem, &QAction::triggered, this, [this] {
    showAndActivate();
    if (m_setupAssistant->identityPending())
      showSetupAssistant();
    else
      m_navigation->setCurrentRow(2);
  });
  auto *openWorkspace = new QAction(tr("打开 Deskflow"), this);
  tray->insertAction(first, openWorkspace);
  connect(openWorkspace, &QAction::triggered, this, &MainWindow::showAndActivate);
  tray->insertSeparator(first);
  tray->removeAction(m_actionMinimize);
  tray->removeAction(m_actionRestore);
  m_actionPause = new QAction(tr("暂停共享"), this);
  m_trayIcon->contextMenu()->insertAction(m_actionStopCore, m_actionPause);
  connect(m_actionPause, &QAction::triggered, m_pauseSharing, &QPushButton::click);
  connect(m_pauseSharing, &QPushButton::clicked, this, [this] {
    if (!m_pauseRequest.isEmpty() || !m_coreProcess.isStarted())
      return;
    m_pauseRequest = m_coreProcess.control("pause", m_paused ? "0" : "1");
    refreshWorkspace();
  });
  connect(&m_coreProcess, &CoreProcess::controlFinished, this, [this](const QJsonObject &result) {
    const auto command = result.value("command").toString();
    const bool success = result.value("success").toBool();
    if (command == "pause" && result.value("id").toString() == m_pauseRequest) {
      m_pauseRequest.clear();
      if (success) {
        m_paused = result.value("argument").toString() == "1";
        Settings::setValue(Settings::Core::SharingPaused, m_paused);
        Settings::save(false);
      }
    }
    if (command == "ready" && result.value("id").toString() == m_readyRequest && success) {
      m_paused = result.value("paused").toBool();
      m_activeTlsEnabled = result.value("tlsEnabled").toBool();
      m_activePeerAuth = result.value("peerAuthEnabled").toBool();
      m_activeInterface = result.value("interface").toString();
      if (!m_applyingRevision && result.value("settingsFile").toString() != m_confirmedRuntimeFile) {
        m_applicationState.restore(m_applicationState.savedSnapshot(), {}, m_applicationState.savedRevision(), 0);
        m_lastOperationError = tr("已连接运行中的核心，其配置版本尚未确认。请检查后再应用已保存配置。");
      }
      if (m_coreProcess.mode() == CoreMode::Client)
        m_activeSource = result.value("source").toString();
    }
    if (command == "ready" && result.value("id").toString() == m_readyRequest && m_applyingRevision) {
      if (m_sourceChange.active()) {
        if (success)
          m_sourceChange.runtimeReady(m_sourceChange.attempt(), m_activeSource);
        else
          m_sourceChange.failed(
              m_sourceChange.attempt(), m_sourceChange.expectedSource(), tr("核心未确认新会话就绪。")
          );
        refreshWorkspace();
        return;
      }
      if (success) {
        m_activeExternalConfig = m_applyingExternalConfig;
        m_confirmedRuntimeFile = result.value("settingsFile").toString();
      }
      m_applicationState.finishApply(
          m_applyingRevision, success, tr("核心未确认配置已接受。共享可能已经停止，请检查诊断后重试。")
      );
      m_applyingRevision = 0;
      persistWorkspaceState();
    }
    if (!success)
      m_lastOperationError = result.value("reason").toString(tr("核心未完成操作，请查看诊断并重试。"));
    else
      m_lastOperationError.clear();
    if (!success)
      QMessageBox::warning(
          this, tr("操作未确认"), result.value("reason").toString(tr("核心未完成操作，请查看诊断并重试。"))
      );
    refreshWorkspace();
  });
  connect(&m_coreProcess, &CoreProcess::ipcReady, this, [this] { m_readyRequest = m_coreProcess.control("ready"); });
  connect(ui->lineHostname, &QLineEdit::editingFinished, this, &MainWindow::persistConfiguration);
  setupSourceChange();
  restoreWorkspaceState();
  connect(m_trayIcon, &QSystemTrayIcon::messageClicked, this, [this] {
    showAndActivate();
    m_navigation->setCurrentRow(2);
  });
  m_navigation->setCurrentRow(0);
  connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [this] { refreshWorkspaceTheme(); });
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
  connect(qApp->styleHints()->accessibility(), &QAccessibilityHints::contrastPreferenceChanged, this, [this] {
    refreshWorkspaceTheme();
  });
#endif
#ifdef Q_OS_MACOS
  observeOSXAccessibilityChanges(this, [this] { refreshWorkspaceTheme(); });
#endif
  refreshWorkspaceTheme();
  resize(1100, 780);
  setMinimumSize(800, 600);
  refreshWorkspace();
}

QByteArray MainWindow::configurationSnapshot() const
{
  QMap<QString, QVariant> values;
  auto &settings = Settings::proxy().get();
  for (const auto &key : settings.allKeys()) {
    if (!key.startsWith("gui/") && key != Settings::Core::LastVersion && key != Settings::Core::SharingPaused)
      values.insert(key, settings.value(key));
  }
  const auto certificate = Settings::value(Settings::Security::Certificate).toString();
  values.insert("$localIdentityFingerprint", TlsUtility::certFingerprint(certificate));
  if (Settings::value(Settings::Server::ExternalConfig).toBool()) {
    QFile file(Settings::value(Settings::Server::ExternalConfigFile).toString());
    values.insert(
        "$externalContentDigest", file.open(QIODevice::ReadOnly)
                                      ? QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256)
                                      : QByteArray("unreadable")
    );
  }
  QByteArray data;
  QDataStream stream(&data, QIODevice::WriteOnly);
  stream << values;
  return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

bool MainWindow::persistConfiguration()
{
  if (!m_serverConfig.loadError().isEmpty()) {
    QMessageBox::warning(this, tr("配置需要恢复"), m_serverConfig.loadError());
    return false;
  }
  if (!Settings::isWritable()) {
    QMessageBox::warning(this, tr("配置只读"), tr("当前配置不可写，草稿保持保留。"));
    return false;
  }
  Settings::setValue(Settings::Client::RemoteHost, ui->lineHostname->text().trimmed());
  Settings::save();
  Settings::proxy().sync();
  if (Settings::proxy().get().status() != QSettings::NoError) {
    QMessageBox::warning(this, tr("保存失败"), tr("无法写入当前配置。输入已保留，请检查路径和权限。"));
    return false;
  }
  m_applicationState.persisted(configurationSnapshot());
  persistWorkspaceState();
  refreshWorkspace();
  return true;
}

void MainWindow::applySavedConfiguration()
{
  ++m_trustGeneration;
  m_pendingIdentities.clear();
  m_pendingFingerprints.clear();
  if (m_identityRegenerating || !persistConfiguration() || m_applicationState.applying())
    return;
  const auto candidate = Settings::value(Settings::Client::RemoteHost).toString();
  const bool changingSource =
      m_coreProcess.isStarted() && m_coreProcess.mode() == CoreMode::Client &&
      Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>() == CoreMode::Client &&
      candidate != m_activeSource;
  if (changingSource) {
    m_sourceBeforeChange = m_coreProcess.runtimeConfiguration();
    if (m_sourceBeforeChange.settingsFile.isEmpty() || m_sourceBeforeChange.source.isEmpty() ||
        !QFileInfo(m_sourceBeforeChange.settingsFile).isReadable()) {
      QMessageBox::warning(
          this, tr("无法安全更换来源"), tr("缺少旧来源的运行快照，无法在失败时恢复。当前连接保持不变。")
      );
      return;
    }
  }
  if (m_coreProcess.isStarted() &&
      QMessageBox::question(
          this, tr("应用配置"),
          changingSource ? tr("更换来源会短暂中断共享。新来源连接失败或取消时，将尝试恢复旧来源并显示结果。是否继续？")
                         : tr("应用可能中断现有连接。失败时将保留已保存配置，但共享可能停止。是否继续？"),
          QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel
      ) != QMessageBox::Yes)
    return;
  if (!m_coreProcess.prepareConfiguration()) {
    QMessageBox::warning(
        this, tr("配置准备失败"), tr("无法准备运行配置；现有共享未被停止。\n%1").arg(m_coreProcess.configurationError())
    );
    return;
  }
  m_applicationState.persisted(configurationSnapshot());
  m_fingerprint = {QCryptographicHash::Sha256, TlsUtility::certFingerprint()};
  updateFingerprintButton();
  m_applyingExternalConfig = Settings::value(Settings::Server::ExternalConfig).toBool();
  m_activeSource = Settings::value(Settings::Client::RemoteHost).toString();
  m_applyingRevision = m_applicationState.beginApply();
  m_trustResumeAllowed = true;
  m_checkedServers.clear();
  m_checkedClients.clear();
  if (changingSource) {
    m_sourcePhaseStarted = false;
    m_sourceChange.begin(m_sourceBeforeChange.source, candidate);
    m_sourceChangeTimer.start(45000);
  }
  const auto applyingRevision = m_applyingRevision;
  QTimer::singleShot(15000, this, [this, applyingRevision] {
    if (!applyingRevision || m_applyingRevision != applyingRevision || m_sourceChange.active())
      return;
    m_applicationState.finishApply(applyingRevision, false, tr("核心未确认应用结果；请检查诊断。"));
    m_applyingRevision = 0;
  });
  m_coreProcess.setMode(Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>());
  m_coreProcess.setAddress(Settings::value(Settings::Client::RemoteHost).toString());
  if (m_coreProcess.isStarted())
    m_coreProcess.restart();
  else
    startCore();
}

void MainWindow::refreshWorkspace()
{
  if (!m_pages)
    return;
  m_settingsWorkspace->setManagedExternally(m_activeExternalConfig, m_coreProcess.mode() == CoreMode::Client);
  const auto state = m_coreProcess.processState();
  const bool running = m_coreProcess.isStarted();
  const auto role = m_coreProcess.mode() == CoreMode::None     ? tr("尚未选择使用方式")
                    : m_coreProcess.mode() == CoreMode::Server ? tr("提供键鼠")
                                                               : tr("接收输入");
  const bool connected = m_coreProcess.connectionState() == ConnectionState::Connected;
  const QString phase = state == ProcessState::Stopped        ? tr("已停止")
                        : state == ProcessState::Stopping     ? tr("正在确认停止")
                        : state == ProcessState::Starting     ? tr("正在启动")
                        : state == ProcessState::RetryPending ? tr("等待重试")
                        : m_paused                            ? tr("已暂停共享")
                        : connected                           ? tr("已连接")
                                                              : tr("等待连接");
  setStatusText(m_workspaceStatus, tr("%1 · %2").arg(role, phase));
  QString config = m_applicationState.applying() ? tr("正在应用配置…")
                   : m_applicationState.pending()
                       ? (state == ProcessState::Stopped ? tr("已保存，尚未应用。当前共享已停止。")
                          : !m_applicationState.error().isEmpty() || !m_applicationState.activeRevision()
                              ? tr("已保存，运行配置尚未确认。请查看诊断。")
                          : running ? tr("已保存，尚未应用。当前共享使用原配置。")
                                    : tr("已保存，待应用。当前共享正在恢复。"))
                       : tr("保存版本与已接受版本一致；连接和输入需分别验证。");
  if (!m_lastOperationError.isEmpty())
    config += "\n" + m_lastOperationError;
  if (!m_applicationState.error().isEmpty())
    config += "\n" + m_applicationState.error();
  if (!Settings::value(Settings::Security::TlsEnabled).toBool())
    config += "\n" + tr("警告：已保存的配置关闭了 TLS 加密。");
  setStatusText(m_configStatus, config);
  m_applySaved->setEnabled(!m_identityRegenerating && !m_applicationState.applying() && Settings::isWritable());
  m_addComputer->setEnabled(!m_identityRegenerating);
  const bool canPause = running && m_pauseRequest.isEmpty() && !m_applicationState.applying();
  m_pauseSharing->setEnabled(canPause);
  m_actionPause->setEnabled(canPause);
  m_pauseSharing->setText(!m_pauseRequest.isEmpty() ? tr("正在确认…") : m_paused ? tr("恢复共享") : tr("暂停共享"));
  m_actionPause->setText(m_pauseSharing->text());
  const bool source =
      (state == ProcessState::Stopped ? Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>()
                                      : m_coreProcess.mode()) == CoreMode::Server;
  m_deviceModes->setCurrentIndex(m_setupAssistant->active() && !m_setupArranging ? 2 : (source ? 0 : 1));
  m_devicesWorkspace->setSessionState(
      m_connectedDevices, m_applicationState.sessions(), m_activeExternalConfig, !source, m_activeSource
  );
  m_receiverSource->setText(
      tr("键鼠来源：%1 · %2")
          .arg(m_activeSource.isEmpty() ? tr("尚未设置") : m_activeSource, connected ? tr("已连接") : tr("等待连接"))
  );
  m_addComputer->setText(
      m_setupAssistant->active() ? tr("继续本次设置") : (source ? tr("添加电脑") : tr("更换键鼠来源"))
  );
  const auto selectedRole = state == ProcessState::Stopped ? Settings::value(Settings::Core::CoreMode).value<CoreMode>()
                                                           : m_coreProcess.mode();
  m_sessionDetail->setText(
      selectedRole == CoreMode::None
          ? tr("先选择使用方式，再连接电脑。")
          : tr("%1 · %2台电脑已连接")
                .arg(source ? tr("这台电脑提供键盘与鼠标") : tr("这台电脑接收另一台电脑的输入"))
                .arg(source ? m_connectedDevices.size() : (connected ? 1 : 0))
  );
  if (selectedRole == CoreMode::None && !m_setupAssistant->active())
    m_addComputer->setText(tr("开始设置"));
  m_pendingPanel->setVisible(
      m_applicationState.pending() || m_applicationState.applying() || !m_lastOperationError.isEmpty() ||
      !m_applicationState.error().isEmpty()
  );
  bool permitted = true;
#ifdef Q_OS_MACOS
  permitted = AXIsProcessTrusted();
#endif
  const auto security = !running                        ? tr("共享尚未运行")
                        : m_applicationState.applying() ? tr("运行策略正在确认")
                        : !m_activeTlsEnabled           ? tr("未加密兼容模式")
                        : m_activePeerAuth              ? tr("TLS与对端证书校验已启用")
                                                        : tr("TLS已启用，接收端证书校验关闭");
  if (auto *status = m_diagnosticLabels.value("permission"))
    status->setText(
#ifdef Q_OS_MACOS
        permitted ? tr("辅助功能权限已允许；仍需验证实际输入") : tr("需要在系统设置中允许辅助功能访问")
#else
        tr("输入权限取决于当前后端；尚未完成实际输入验证")
#endif
    );
  if (auto *status = m_diagnosticLabels.value("network"))
    status->setText(tr("%1个可用本机地址；不代表对端可达").arg(NetworkMonitor::validAddresses().size()));
  if (auto *status = m_diagnosticLabels.value("identity"))
    status->setText(security);
  if (auto *status = m_diagnosticLabels.value("session"))
    status->setText(m_workspaceStatus->text());
  if (auto *status = m_diagnosticLabels.value("input")) {
    int count = 0;
    for (const auto &session : m_applicationState.sessions())
      if (session.connected && session.verificationAttempt == session.attempt && session.manuallyVerifiedAt.isValid())
        ++count;
    status->setText(tr("%1台电脑具有本次会话的人工验证记录").arg(count));
  }
  if (m_diagnosticStatus)
    setStatusText(m_diagnosticStatus, m_lastOperationError);
  if (!m_trustRefreshScheduled) {
    m_trustRefreshScheduled = true;
    QTimer::singleShot(0, this, [this] {
      m_trustRefreshScheduled = false;
      if (m_trustContent && m_trustContent->isVisible())
        refreshTrustPage();
    });
  }
  m_trayIcon->setToolTip(m_workspaceStatus->text());
  m_traySummary->setText(m_workspaceStatus->text());
  m_trayConnections->setText(
      tr("当前已连接 %1 台电脑")
          .arg(m_coreProcess.mode() == CoreMode::Server ? m_connectedDevices.size() : (connected ? 1 : 0))
  );
  m_trayProblem->setVisible(
      m_setupAssistant->identityPending() || !m_lastOperationError.isEmpty() || !m_applicationState.error().isEmpty()
  );
  m_trayProblem->setText(m_setupAssistant->identityPending() ? tr("有设备等待身份核对…") : tr("查看需要处理的问题…"));
}

void MainWindow::verifySelectedInput()
{
  if (m_paused || m_applicationState.applying()) {
    QMessageBox::information(this, tr("暂不能验证输入"), tr("请先恢复共享并等待应用完成，再验证实际输入。"));
    return;
  }
  const auto name =
      m_coreProcess.mode() == CoreMode::Client ? m_activeSource : m_devicesWorkspace->selectedIdentifier();
  if (name.isEmpty() || !m_applicationState.sessions().value(name).connected) {
    QMessageBox::information(this, tr("输入验证"), tr("请先选择一台已经连接的远端电脑。"));
    return;
  }
  if (QMessageBox::question(
          this, tr("人工验证输入"),
          tr("请将鼠标移至目标电脑，并实际输入测试文字。\n确认 %1 的鼠标和键盘均正常？\n此结果将标记为人工验证。")
              .arg(name),
          QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel
      ) == QMessageBox::Yes)
    m_applicationState.verifyManually(name);
}

void MainWindow::showTrust()
{
  navigateSettings("trust");
}

void MainWindow::exportDiagnosticReport()
{
  // Whitelist only: no names, addresses, paths, command bodies or raw logs.
  QJsonObject report{
      {"version", kVersion},
      {"platform", QSysInfo::prettyProductName()},
      {"architecture", QSysInfo::currentCpuArchitecture()},
      {"processState", static_cast<int>(m_coreProcess.processState())},
      {"connectionState", static_cast<int>(m_coreProcess.connectionState())},
      {"paused", m_paused},
      {"savedRevision", static_cast<qint64>(m_applicationState.savedRevision())},
      {"activeRevision", static_cast<qint64>(m_applicationState.activeRevision())},
      {"generatedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}
  };
  const auto data = QJsonDocument(report).toJson(QJsonDocument::Indented);
  QDialog preview(this);
  preview.setWindowTitle(tr("脱敏报告预览"));
  preview.resize(650, 450);
  auto *layout = new QVBoxLayout(&preview);
  auto *text = new QPlainTextEdit(QString::fromUtf8(data));
  text->setReadOnly(true);
  layout->addWidget(text);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &preview, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &preview, &QDialog::reject);
  if (preview.exec() != QDialog::Accepted)
    return;
  const auto path = QFileDialog::getSaveFileName(this, tr("导出报告"), {}, "JSON (*.json)");
  if (path.isEmpty())
    return;
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
    QMessageBox::warning(this, tr("导出失败"), tr("报告未能完整保存，请检查路径与权限。"));
}

void MainWindow::requestQuit()
{
  m_explicitQuit = true;
  close();
}

void MainWindow::exportConfigurationBackup()
{
  if (!persistConfiguration())
    return;
  if (QMessageBox::question(
          this, tr("完整配置备份"),
          tr("备份包含设备、别名、映射、规则、共享选项、路径及自动化命令正文；不包含证书私钥。请将文件保存在可信位置。"
          ),
          QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel
      ) != QMessageBox::Save)
    return;
  ConfigurationBackup backup;
  auto &settings = Settings::proxy().get();
  for (const auto &key : settings.allKeys())
    backup.settings.insert(key, settings.value(key));
  if (Settings::value(Settings::Server::ExternalConfig).toBool()) {
    QFile source(Settings::value(Settings::Server::ExternalConfigFile).toString());
    if (!source.open(QIODevice::ReadOnly)) {
      QMessageBox::warning(this, tr("备份失败"), tr("外部配置不可读，未生成不完整备份。"));
      return;
    }
    backup.serverConfig = source.readAll();
  } else {
    QString text;
    QTextStream stream(&text);
    stream << m_serverConfig;
    stream.flush();
    backup.serverConfig = text.toUtf8();
  }
  QStringList unreadable;
  auto read = [&unreadable](const QString &path) {
    QFile file(path);
    if (!file.exists())
      return QByteArray();
    if (!file.open(QIODevice::ReadOnly)) {
      unreadable.append(path);
      return QByteArray();
    }
    return file.readAll();
  };
  backup.trustedServers = read(Settings::tlsTrustedServersDb());
  backup.trustedClients = read(Settings::tlsTrustedClientsDb());
  backup.revokedFingerprints = read(Settings::tlsRevokedDb());
  backup.peerIdentities = read(Settings::peerIdentityDb());
  if (backup.peerIdentities.isEmpty())
    backup.peerIdentities = R"({"version":1,"peers":[]})";
  if (!unreadable.isEmpty()) {
    QMessageBox::warning(
        this, tr("备份未完成"), tr("以下身份或信任文件不可读，未生成不完整备份：\n%1").arg(unreadable.join("\n"))
    );
    return;
  }
  const auto data = backup.encode();
  ConfigurationBackup check;
  QString error;
  if (!ConfigurationBackup::decode(data, check, error) || check.settings != backup.settings ||
      check.serverConfig != backup.serverConfig) {
    QMessageBox::warning(this, tr("备份失败"), tr("备份读回校验失败，未写入文件。"));
    return;
  }
  const auto path =
      QFileDialog::getSaveFileName(this, tr("导出配置备份"), "Deskflow.backup.json", "Deskflow backup (*.json)");
  if (path.isEmpty())
    return;
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    QMessageBox::warning(this, tr("导出失败"), file.errorString());
    return;
  }
  file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
  if (file.write(data) != data.size() || !file.commit())
    QMessageBox::warning(this, tr("导出失败"), tr("备份未能完整写入。"));
}

void MainWindow::restoreWorkspaceState()
{
  QFile file(QDir(Settings::settingsPath()).filePath(".workspace-state.json"));
  QJsonObject state;
  if (file.open(QIODevice::ReadOnly))
    state = QJsonDocument::fromJson(file.readAll()).object();
  const auto saved = configurationSnapshot();
  const auto previousSaved = QByteArray::fromHex(state.value("savedDigest").toString().toLatin1());
  auto revision = static_cast<quint64>(std::max(qint64(1), state.value("savedRevision").toInteger(1)));
  if (!previousSaved.isEmpty() && saved != previousSaved)
    ++revision;
  m_confirmedRuntimeFile = state.value("confirmedRuntimeFile").toString();
  m_activeInterface = state.value("activeInterface").toString();
  m_activeExternalConfig = state.value("activeExternalConfig").toBool(false);
  m_activeTlsEnabled = state.value("activeTlsEnabled").toBool(false);
  m_activePeerAuth = state.value("activePeerAuth").toBool(false);
  m_applicationState.restore(
      saved, QByteArray::fromHex(state.value("activeDigest").toString().toLatin1()), revision,
      static_cast<quint64>(std::max(qint64(0), state.value("activeRevision").toInteger(0)))
  );
}

bool MainWindow::persistWorkspaceState()
{
  QJsonObject state{
      {"schema", 1},
      {"savedDigest", QString::fromLatin1(m_applicationState.savedSnapshot().toHex())},
      {"activeDigest", QString::fromLatin1(m_applicationState.activeSnapshot().toHex())},
      {"savedRevision", static_cast<qint64>(m_applicationState.savedRevision())},
      {"activeRevision", static_cast<qint64>(m_applicationState.activeRevision())},
      {"activeExternalConfig", m_activeExternalConfig},
      {"activeTlsEnabled", m_activeTlsEnabled},
      {"activePeerAuth", m_activePeerAuth},
      {"activeInterface", m_activeInterface},
      {"confirmedRuntimeFile", m_confirmedRuntimeFile}
  };
  const auto data = QJsonDocument(state).toJson(QJsonDocument::Compact);
  QSaveFile file(QDir(Settings::settingsPath()).filePath(".workspace-state.json"));
  if (!file.open(QIODevice::WriteOnly)) {
    m_lastOperationError = tr("无法保存配置版本记录；请检查配置目录权限。");
    refreshWorkspace();
    return false;
  }
  file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
  if (file.write(data) != data.size() || !file.commit()) {
    m_lastOperationError = tr("配置版本记录写入失败。请在下次启动后重新确认应用状态。");
    refreshWorkspace();
    return false;
  }
  return true;
}

void MainWindow::restoreConfigurationBackup()
{
  if (m_identityRegenerating) {
    QMessageBox::information(this, tr("身份更新中"), tr("请等待本机身份生成结束，再恢复备份。"));
    return;
  }
  if (m_coreProcess.processState() != ProcessState::Stopped) {
    QMessageBox::information(
        this, tr("请先停止共享"), tr("恢复包含信任记录。请先明确停止共享，再恢复备份；恢复后仍需另行应用与连接。")
    );
    return;
  }
  if (!Settings::isWritable()) {
    QMessageBox::warning(this, tr("配置只读"), tr("当前配置不可写。"));
    return;
  }
  const auto path = QFileDialog::getOpenFileName(this, tr("选择配置备份"), {}, "Deskflow backup (*.json)");
  if (path.isEmpty())
    return;
  QFile source(path);
  ConfigurationBackup backup;
  QString error;
  if (!source.open(QIODevice::ReadOnly) || source.size() > 16 * 1024 * 1024 ||
      !ConfigurationBackup::decode(source.readAll(), backup, error)) {
    QMessageBox::warning(this, tr("备份无效"), error.isEmpty() ? tr("文件不可读或超过大小限制。") : error);
    return;
  }
  if (QMessageBox::question(
          this, tr("恢复配置备份"),
          tr("恢复将替换当前已保存配置、信任记录及页面草稿。备份可能包含本机自动化命令，请在应用前检查维护页。不会导入"
             "证书私钥，也不会自动连接。是否继续？"),
          QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel
      ) != QMessageBox::Yes)
    return;
  const auto backupPath =
      Settings::settingsFile() + ".before-restore-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (QFile::exists(Settings::settingsFile()) && !QFile::copy(Settings::settingsFile(), backupPath)) {
    QMessageBox::warning(this, tr("恢复未执行"), tr("无法备份当前配置，原配置保持保留。"));
    return;
  }
  const auto externalPath =
      QDir(Settings::settingsPath())
          .filePath("restored-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".server.conf");
  auto write = [](const QString &path, const QByteArray &data) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
      return false;
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    return file.write(data) == data.size() && file.commit();
  };
  if (backup.settings.value(Settings::Server::ExternalConfig).toBool() && !write(externalPath, backup.serverConfig)) {
    QMessageBox::warning(this, tr("恢复失败"), tr("无法写入恢复的服务器配置，原配置保持保留。"));
    return;
  }
  auto &settings = Settings::proxy().get();
  QMap<QString, QVariant> original;
  for (const auto &key : settings.allKeys())
    original.insert(key, settings.value(key));
  auto replace = [&settings](const QMap<QString, QVariant> &values) {
    settings.clear();
    for (auto entry = values.cbegin(); entry != values.cend(); ++entry)
      settings.setValue(entry.key(), entry.value());
    settings.sync();
    return settings.status() == QSettings::NoError;
  };
  if (backup.settings.value(Settings::Server::ExternalConfig).toBool())
    backup.settings.insert(Settings::Server::ExternalConfigFile, externalPath);
  if (!replace(backup.settings)) {
    replace(original);
    QFile::remove(externalPath);
    QMessageBox::warning(this, tr("恢复失败"), tr("配置写入失败；已尝试恢复原配置。备份位于 %1").arg(backupPath));
    return;
  }
  QDir().mkpath(Settings::tlsDir());
  QStringList failed, written;
  QMap<QString, QByteArray> previousTrust;
  QSet<QString> existed;
  for (const auto &path :
       {Settings::tlsTrustedServersDb(), Settings::tlsTrustedClientsDb(), Settings::tlsRevokedDb(),
        Settings::peerIdentityDb()}) {
    QFile file(path);
    if (file.exists())
      existed.insert(path);
    if (file.open(QIODevice::ReadOnly))
      previousTrust.insert(path, file.readAll());
  }
  for (const auto &entry : QList<QPair<QString, QByteArray>>{
           {Settings::tlsTrustedServersDb(), backup.trustedServers},
           {Settings::tlsTrustedClientsDb(), backup.trustedClients},
           {Settings::tlsRevokedDb(), backup.revokedFingerprints},
           {Settings::peerIdentityDb(),
            backup.peerIdentities.isEmpty() ? QByteArray(R"({"version":1,"peers":[]})") : backup.peerIdentities}
       }) {
    if (!write(entry.first, entry.second))
      failed.append(entry.first);
    else
      written.append(entry.first);
  }
  if (!failed.isEmpty()) {
    bool rolledBack = replace(original);
    for (const auto &path : written)
      rolledBack =
          (existed.contains(path) ? write(path, previousTrust.value(path)) : QFile::remove(path)) && rolledBack;
    QFile::remove(externalPath);
    QMessageBox::warning(
        this, tr("恢复失败"),
        rolledBack ? tr("信任文件未能写入，已恢复原配置与原信任。")
                   : tr("恢复和回退未能全部完成。请停止共享并从 %1 恢复配置，检查信任文件权限。").arg(backupPath)
    );
    return;
  }
  m_serverConfig.reloadSaved();
  m_devicesWorkspace->reloadSaved();
  m_settingsWorkspace->reloadSaved();
  {
    const QSignalBlocker sourceBlock(ui->rbModeServer), receiverBlock(ui->rbModeClient);
    ui->rbModeServer->setChecked(
        Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>() == CoreMode::Server
    );
    ui->rbModeClient->setChecked(
        Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>() == CoreMode::Client
    );
  }
  ui->lineHostname->setText(Settings::value(Settings::Client::RemoteHost).toString());
  applyConfig();
  m_applicationState.persisted(configurationSnapshot());
  persistWorkspaceState();
  refreshWorkspace();
  if (!failed.isEmpty())
    QMessageBox::warning(
        this, tr("恢复部分完成"), tr("配置已恢复，但以下信任文件未能写入。请修复后再应用：\n%1").arg(failed.join("\n"))
    );
  else
    QMessageBox::information(this, tr("备份已恢复"), tr("已恢复到保存状态。请检查设置后明确应用；尚未连接。"));
}

void MainWindow::exportGraphicalConfiguration()
{
  exportConfigurationBackup();
}

void MainWindow::exportRoutingConfiguration()
{
  const auto path =
      QFileDialog::getSaveFileName(this, tr("导出已保存的核心路由文本"), {}, tr("配置文件 (*.conf);;所有文件 (*)"));
  if (path.isEmpty())
    return;
  if (!m_serverConfig.save(path))
    QMessageBox::warning(this, tr("导出失败"), tr("配置未能完整写入，请检查位置与权限。"));
}

void MainWindow::regenerateLocalIdentity(int keyLength)
{
  if (m_identityRegenerating || !Settings::isWritable() || (keyLength != 2048 && keyLength != 4096))
    return;
  const auto path = Settings::value(Settings::Security::Certificate).toString();
  m_identityRegenerating = true;
  m_settingsWorkspace->setIdentityGenerationState(true);
  m_lastOperationError = tr("正在停止共享并生成本机身份，请稍候…");
  auto started = std::make_shared<bool>(false);
  auto stopped = std::make_shared<QMetaObject::Connection>();
  auto generate = [this, path, keyLength, started, stopped] {
    if (*started || !m_identityRegenerating)
      return;
    *started = true;
    disconnect(*stopped);
    auto promise = std::make_shared<QPromise<bool>>();
    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher] {
      const bool success = watcher->result();
      watcher->deleteLater();
      m_identityRegenerating = false;
      m_settingsWorkspace->setIdentityGenerationState(
          false, success ? QString() : tr("生成失败，旧身份保持保留。请检查目录与权限。")
      );
      if (success) {
        m_fingerprint = {QCryptographicHash::Sha256, TlsUtility::certFingerprint()};
        updateFingerprintButton();
        m_checkedClients.clear();
        m_checkedServers.clear();
        m_applicationState.persisted(configurationSnapshot());
        persistWorkspaceState();
      }
      m_lastOperationError = success ? tr("本机身份已更新，共享保持停止。请在对端重新核对完整指纹，再明确连接。")
                                     : tr("本机身份生成失败，旧证书保持保留，共享已停止。");
      refreshWorkspace();
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, path, keyLength] {
      promise->start();
      promise->addResult(TlsUtility::generateCertificateAtPath(path, keyLength));
      promise->finish();
    });
  };
  if (m_coreProcess.processState() != ProcessState::Stopped)
    *stopped = connect(&m_coreProcess, &CoreProcess::processStateChanged, this, [generate](ProcessState state) {
      if (state == ProcessState::Stopped)
        generate();
    });
  stopCore();
  if (m_coreProcess.processState() == ProcessState::Stopped)
    generate();
  QTimer::singleShot(22000, this, [this, started, stopped] {
    if (!m_identityRegenerating || *started)
      return;
    disconnect(*stopped);
    m_identityRegenerating = false;
    m_settingsWorkspace->setIdentityGenerationState(false, tr("无法确认共享已经停止，未更换本机身份。"));
    m_lastOperationError = tr("停止结果未确认，本机证书保持保留。");
    refreshWorkspace();
  });
  refreshWorkspace();
}
