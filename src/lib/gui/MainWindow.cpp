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

#include "Diagnostic.h"
#include "StyleUtils.h"

#include "dialogs/AboutDialog.h"
#include "dialogs/ClientConfigDialog.h"
#include "dialogs/FingerprintDialog.h"
#include "dialogs/HelpDialog.h"
#include "widgets/BrandIcons.h"
#include "widgets/DeviceListPage.h"
#include "widgets/ScreenLayoutPage.h"
#include "widgets/SettingsPage.h"
#include <QStackedWidget>

#include "common/PlatformInfo.h"
#include "common/Settings.h"
#include "common/UrlConstants.h"
#include "common/VersionInfo.h"
#include "gui/Messages.h"
#include "gui/TlsUtility.h"
#include "gui/core/BackupConnectionPanel.h"
#include "gui/core/CoreProcess.h"
#include "gui/core/TransportNotifications.h"
#include "gui/widgets/LocalBluetoothInfo.h"
#ifdef Q_OS_MACOS
#include "gui/OSXHelpers.h"
#endif
#include "gui/ipc/DaemonIpcClient.h"
#include "gui/widgets/LogDock.h"
#include "net/FingerprintDatabase.h"
#include "widgets/HomeIcons.h"
#include "widgets/StatusBar.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QCompleter>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFocusEvent>
#include <QGraphicsOpacityEffect>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStringListModel>
#include <QStyle>
#include <QSysInfo>
#include <QWindow>

#include <memory>

#if defined(Q_OS_MACOS)
#include <ApplicationServices/ApplicationServices.h>
#endif

using namespace deskflow::gui;

namespace {
auto deferHomeLayout(QWidget *content, QLayout *layout)
{
  const bool paintEnabled = content->updatesEnabled();
  const bool layoutEnabled = layout->isEnabled();
  content->setUpdatesEnabled(false);
  layout->setEnabled(false);
  return qScopeGuard([content, layout, paintEnabled, layoutEnabled] {
    layout->setEnabled(layoutEnabled);
    if (layoutEnabled) {
      layout->invalidate();
      layout->activate();
    }
    content->setUpdatesEnabled(paintEnabled);
  });
}
} // namespace

MainWindow::MainWindow()
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
  m_coreProcess.setParent(this);
  setWindowFlag(Qt::FramelessWindowHint);
  setAttribute(Qt::WA_TranslucentBackground);
  setStyleSheet(QStringLiteral("QMainWindow#MainWindow { background: transparent; }"));
  ui->setupUi(this);
  setupHome();
  m_transportNotifications = new TransportNotifications(this);
  connect(
      &m_coreProcess, &CoreProcess::transportStateChanged, m_transportNotifications, &TransportNotifications::update
  );
  connect(&m_coreProcess, &CoreProcess::processStateChanged, this, [this](auto state) {
    if (state == deskflow::core::ProcessState::Stopped)
      m_transportNotifications->reset();
  });
  connect(m_transportNotifications, &TransportNotifications::statusChanged, this, [this](const auto &text) {
    m_statusBar->showMessage(text);
    m_trayIcon->setToolTip(text.isEmpty() ? QString::fromLatin1(kAppName) : text);
  });
  connect(
      m_transportNotifications, &TransportNotifications::notification, this,
      [this](const QString &title, const QString &body) {
#ifdef Q_OS_MACOS
        showOSXNotification(title, body);
#else
        m_trayIcon->showMessage(title, body, QSystemTrayIcon::Information, 6000);
#endif
      }
  );

  setWindowIcon(applicationIcon());
  QGuiApplication::setWindowIcon(applicationIcon());

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

  m_guiDupeChecker->setObjectName("guiInstanceServer");

  createMenuBar();
  setupControls();
  updateText();
  connectSlots();
  setupTrayIcon();
  updateComputerName();
  setHelpFilePath();

  qDebug().noquote() << "active settings path:" << Settings::settingsPath();

  // Force generation of a certificate on the host
  if (TlsUtility::isEnabled()) {
    if (Settings::value(Settings::Security::KeySize).toInt() < 2048) {
      QMessageBox::information(
          this, kAppName,
          tr("Your current TLS key is smaller than the minimum allowed size, A new key 2048-bit key will be generated.")
      );
      Settings::setValue(Settings::Security::KeySize, 2048);
    }
    if (!TlsUtility::isCertValid()) {
      generateCertificate();
    } else {
      m_fingerprint = {QCryptographicHash::Sha256, TlsUtility::certFingerprint()};
    }
  }

  applyConfig();
  m_statusBar->setSecurityIcon(TlsUtility::isEnabled());
  restoreWindow();
  refreshPermissionStatus();
}

void MainWindow::listenForOtherInstances()
{
  if (m_guiDupeChecker->isListening())
    return;
  // Only the production entry point, after acquiring its instance lock, owns this
  // process-wide endpoint. Constructing a test/preview window must not unlink it.
  QLocalServer::removeServer(m_guiSocketName);
  if (!m_guiDupeChecker->listen(m_guiSocketName))
    qWarning() << "Unable to listen for duplicate GUI instances:" << m_guiDupeChecker->errorString();
#ifdef Q_OS_MACOS
  // An explicit native Quit must exit. Closing a window can still hide to tray.
  // Install the process-wide hook once, rather than for every test window.
  installQuitHandler([this] { return close(); });
#endif
}
MainWindow::~MainWindow()
{
  if (m_mdnsBrowser) {
    // Finish discovery while MainWindow's slots and device page are still alive.
    m_mdnsBrowser->disconnect(this);
    m_mdnsBrowser->stop();
    m_mdnsBrowser->wait();
  }
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
  if (!windowGeometry.isValid() || windowGeometry.width() < minimumWidth() || windowGeometry.height() < 500) {
    resize(QSize(1060, 790).boundedTo(totalGeometry.size()));
    windowGeometry = geometry();
  } else {
    setGeometry(windowGeometry);
  }

  if (!totalGeometry.contains(windowGeometry)) {
    QRect screenGeometry = QGuiApplication::primaryScreen()->geometry();
    move(screenGeometry.center() - rect().center());
  }

  // Old releases stored the compact toolbar-sized window. Grow it to the new homepage minimum.
  resize(size().expandedTo(minimumSize()));
}

void MainWindow::setupControls()
{
  secureSocket(false);

  ui->btnConfigureServer->setIcon(homeIcon(HomeIcon::Panel, QColor("#267ed9")));
  ui->btnConfigureClient->setIcon(homeIcon(HomeIcon::Settings, QColor("#267ed9")));

  if (Settings::value(Settings::Core::LastVersion).toString() != kVersion) {
    Settings::setValue(Settings::Core::LastVersion, kVersion);
  }

  // Diagnostics are opt-in for each launch, including profiles from the old dock layout.
  m_logDock->hide();

  ui->serverOptions->setVisible(false);

  const auto coreMode = Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>();
  ui->rbModeClient->setChecked(coreMode == Settings::CoreMode::Client);
  ui->rbModeServer->setChecked(coreMode == Settings::CoreMode::Server);

  ui->lineEditName->setValidator(new QRegularExpressionValidator(m_nameRegEx, this));
  ui->lineEditName->setVisible(false);
  ui->lineEditName->installEventFilter(this);

  if (deskflow::platform::isMac()) {
    ui->rbModeServer->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->rbModeClient->setAttribute(Qt::WA_MacShowFocusRect, false);
  } else {
    ui->btnSaveServerConfig->setIconSize(QSize(22, 22));
  }
  setStatusBar(m_statusBar);
  m_statusBar->hide();
}

void MainWindow::setupHome()
{
  auto *backupPanel = new BackupConnectionPanel(ui->homeActionCard);
  m_backupPanel = backupPanel;
  ui->modeLinksLayout->insertWidget(0, backupPanel);
  ui->modeLinksLayout->removeItem(ui->modeLinksStretch);
  ui->modeLinksLayout->insertSpacerItem(ui->modeLinksLayout->indexOf(ui->btnBrowseServers), ui->modeLinksStretch);
  ui->optionsLayout->insertWidget(0, backupPanel->sharingSelector());
  ui->optionsLayout->insertWidget(1, backupPanel->transportSelector());
  ui->homeFooterLayout->insertWidget(0, backupPanel->statusWidget(), 1);
  connect(backupPanel, &BackupConnectionPanel::statusVisibilityChanged, this, [this](bool visible) {
    ui->lblHomeStatus->setVisible(!visible);
  });
  ui->lblConnectionKind->hide();
  ui->contentLayout->insertWidget(ui->contentLayout->indexOf(ui->homeActionCard) + 1, backupPanel->detailsWidget());
  connect(backupPanel, &BackupConnectionPanel::command, &m_coreProcess, &CoreProcess::transportCommand);
  connect(&m_coreProcess, &CoreProcess::transportSnapshot, backupPanel, &BackupConnectionPanel::updateSnapshot);
  connect(
      &m_coreProcess, &CoreProcess::transportOperationFinished, backupPanel, &BackupConnectionPanel::operationFinished
  );
  connect(&m_coreProcess, &CoreProcess::processStateChanged, this, [this, backupPanel](ProcessState state) {
    const bool running = state != ProcessState::Stopped;
    backupPanel->setRunning(running);
    // A connecting/running client may switch roles; only an in-flight transition locks the selector.
    const bool transitioning = state == ProcessState::Starting || state == ProcessState::Stopping;
    ui->rbModeServer->setEnabled(!transitioning);
    ui->rbModeClient->setEnabled(!transitioning);
    ui->lineHostname->setReadOnly(running);
    ui->btnBrowseServers->setEnabled(!running);
    if (!running || state == ProcessState::Starting) {
      if (auto *bluetooth = findChild<LocalBluetoothInfo *>())
        bluetooth->setSharingState(running && m_coreProcess.mode() == CoreMode::Server ? tr("Starting…") : QString());
      if (auto *network = findChild<QLabel *>("lblLocalNetworkState"))
        network->setText(running && m_coreProcess.mode() == CoreMode::Server ? tr("Starting…") : tr("Available"));
    }
    if (!running)
      updateNetworkInfo();
  });
  connect(backupPanel, &BackupConnectionPanel::transportChanged, this, &MainWindow::updateModeControls);
  connect(backupPanel, &BackupConnectionPanel::snapshotChanged, this, [this](const QJsonObject &snapshot) {
    if (m_coreProcess.mode() != CoreMode::Server)
      return;
    if (auto *state = findChild<QLabel *>("lblLocalNetworkState"))
      state->setText(snapshot.value("networkListening").toBool() ? tr("Waiting for connections") : tr("Not shared"));
    if (auto *bluetooth = findChild<LocalBluetoothInfo *>())
      bluetooth->setSharingState(
          snapshot.value("bluetoothListening").toBool()                    ? tr("Discoverable")
          : !snapshot.value("bluetoothReason").toString().isEmpty()        ? tr("Unavailable")
          : Settings::value(Settings::Connection::ShareBluetooth).toBool() ? tr("Starting…")
                                                                           : tr("Not shared")
      );
  });
  while (auto *item = ui->modeFieldsLayout->takeAt(0)) {
    if (item->widget())
      item->widget()->hide();
    delete item;
  }
  auto *inputStack = new QStackedWidget(ui->modeFields);
  inputStack->setObjectName("connectionInputStack");
  inputStack->setFixedHeight(44);
  inputStack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  inputStack->setStyleSheet("QStackedWidget#connectionInputStack { border: none; background: transparent; }");
  inputStack->addWidget(ui->clientOptions);
  inputStack->addWidget(backupPanel->bluetoothSelector());
  ui->clientOptions->setFixedHeight(44);
  ui->lineHostname->setFixedHeight(44);
  ui->lineHostname->setAttribute(Qt::WA_MacShowFocusRect, false);
  ui->modeFieldsLayout->setSpacing(0);
  ui->modeFieldsLayout->addWidget(inputStack);
  ui->modeFields->setFixedHeight(44);
  // Keep the homepage light like the approved prototype, independently of native dialog appearance.
  ui->topLevelWidget->setPalette(lightControlPalette());
  ui->topLevelWidget->setStyleSheet(ui->topLevelWidget->styleSheet() + lightScrollBarStyle());
  ui->lblVersion->setText(QStringLiteral("v%1").arg(QString::fromLatin1(kVersion).section('.', 0, 2)));
  ui->lblVersion->setToolTip(kDisplayVersion);
#ifdef Q_OS_MACOS
  ui->lblLocalPlatform->setText(tr("macOS · This computer"));
#else
  ui->lblLocalPlatform->setText(QSysInfo::prettyProductName());
#endif
  ui->lblComputerName->setTextInteractionFlags(Qt::TextSelectableByMouse);
  ui->lblIpAddresses->setTextInteractionFlags(Qt::TextSelectableByMouse);
  ui->nameColumn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  ui->addressColumn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  ui->localCard->setFixedHeight(112);
  ui->localHeader->setFixedHeight(20);
  ui->widgetModeSelection->setFixedHeight(50);
  // Both cards share the same vertical centreline, independent of label length.
  ui->localCardLayout->setStretch(0, 1);
  ui->localCardLayout->setStretch(2, 1);
  ui->modeLayout->setStretch(0, 1);
  ui->modeLayout->setStretch(1, 1);
  // Keep IP and Bluetooth on two aligned rows inside the original 112px card.
  while (auto *item = ui->addressLayout->takeAt(0)) {
    if (item->widget())
      item->widget()->hide();
    delete item;
  }
  ui->addressLayout->setContentsMargins(0, 0, 0, 0);
  ui->addressLayout->setSpacing(2);
  ui->addressValueRow->setFixedHeight(28);
  ui->addressValueLayout->setSpacing(6);
  ui->addressValueRow->show();
  auto *wifi = new QLabel(ui->addressValueRow);
  wifi->setPixmap(homeIcon(HomeIcon::Wifi, QColor("#087fff")).pixmap(16, 16));
  wifi->setFixedWidth(18);
  ui->addressValueLayout->insertWidget(0, wifi);
  auto *networkState = new QLabel(tr("Available"), ui->addressValueRow);
  networkState->setObjectName("lblLocalNetworkState");
  networkState->setProperty("role", "muted");
  networkState->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  ui->addressValueLayout->insertWidget(ui->addressValueLayout->indexOf(ui->btnCopyAddress), networkState);
  ui->lblIpAddresses->setStyleSheet("font-size: 13px;");
  ui->lblIpAddresses->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  ui->addressLayout->addWidget(ui->addressValueRow);
  ui->addressLayout->addWidget(new LocalBluetoothInfo(ui->addressColumn));
  ui->optionsLayout->setStretch(ui->optionsLayout->indexOf(backupPanel->sharingSelector()), 1);
  ui->optionsLayout->setStretch(ui->optionsLayout->indexOf(ui->modeFields), 1);
  ui->btnToggleCore->setFixedWidth(124);
  ui->homeActionCard->setFixedHeight(148);
  // Hidden mode controls must not impose their cached minimum width while switching.
  ui->homeActionCard->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  ui->homeActionLayout->setContentsMargins(14, 10, 14, 10);
  ui->homeActionLayout->setSpacing(6);
  ui->lblShareHint->hide();
  ui->m_pLabelServerName->hide();
  ui->lblActionHint->hide();
  ui->localCardSpace->changeSize(0, 16, QSizePolicy::Minimum, QSizePolicy::Fixed);
  ui->widgetModeSelectionSpace->changeSize(0, 10, QSizePolicy::Minimum, QSizePolicy::Fixed);
  ui->homeFooterLayout->setContentsMargins(0, 10, 0, 0);
  // Keep the footer at the bottom without adding a second device summary.
  ui->contentLayout->removeItem(ui->contentStretch);
  ui->contentLayout->insertSpacerItem(ui->contentLayout->indexOf(ui->homeFooter), ui->contentStretch);
  ui->lblShareTitle->setFixedHeight(24);
  ui->lblShareHint->setFixedHeight(36);
  ui->m_pLabelServerName->setFixedHeight(18);
  ui->widgetModeOptions->setFixedHeight(44);
  ui->modeLinks->setFixedHeight(28);
  ui->lblActionHint->setFixedHeight(18);
  ui->homeActionCard->setStyleSheet(
      QStringLiteral("QWidget#homeActionCard { background: #f7f8fa; border: 1px solid #e6e9ee; border-radius: 10px; }")
  );
  ui->lineShareAddress->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  ui->lineHostname->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  ui->lineHostname->setPlaceholderText(tr("Choose a computer or enter an IP"));
  auto *completion = new QCompleter(new QStringListModel(this), this);
  completion->setObjectName("homeComputerCompletion");
  completion->setCaseSensitivity(Qt::CaseInsensitive);
  completion->setFilterMode(Qt::MatchContains);
  ui->lineHostname->setCompleter(completion);
  auto *recentComputers = ui->lineHostname->addAction(homeIcon(HomeIcon::ChevronDown), QLineEdit::TrailingPosition);
  recentComputers->setText(tr("Recent computers"));
  connect(recentComputers, &QAction::triggered, this, [this, completion] {
    if (ui->lineHostname->isReadOnly())
      return;
    browseServers();
  });
  connect(completion, qOverload<const QString &>(&QCompleter::activated), this, [this] {
    m_homeDiscoveryRequested = false;
  });
  connect(ui->lineHostname, &QLineEdit::textEdited, this, [completion] {
    completion->setCompletionMode(QCompleter::PopupCompletion);
  });
  m_copyShareAddress = ui->lineShareAddress->addAction(homeIcon(HomeIcon::Copy), QLineEdit::TrailingPosition);
  m_copyShareAddress->setText(tr("Copy"));
  m_copyShareAddress->setToolTip(tr("Copy"));
  connect(m_copyShareAddress, &QAction::triggered, ui->btnCopyAddress, &QPushButton::click);
  m_actionTransitionOverlay = new QLabel(ui->homeContent);
  m_actionTransitionOverlay->setObjectName("homeActionTransitionOverlay");
  m_actionTransitionOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
  m_actionTransitionOverlay->hide();
  auto transitionOpacity = new QGraphicsOpacityEffect(m_actionTransitionOverlay);
  m_actionTransitionOverlay->setGraphicsEffect(transitionOpacity);
  m_actionTransition = new QPropertyAnimation(transitionOpacity, "opacity", this);
  m_actionTransition->setObjectName("homeActionTransition");
  m_actionTransition->setDuration(150);
  connect(m_actionTransition, &QPropertyAnimation::finished, m_actionTransitionOverlay, &QWidget::hide);
  ui->btnRestartCore->setFixedWidth(38);
  ui->btnRestartCore->setIcon(homeIcon(HomeIcon::Refresh));
  ui->btnRestartCore->hide();

  ui->lblAvatar->setPixmap(homeIcon(HomeIcon::Monitor, QColor("#087fff")).pixmap(20, 20));
  ui->lblBrandIcon->setFixedSize(24, 28);
  ui->lblBrandIcon->setAlignment(Qt::AlignCenter);
  ui->lblBrandIcon->setPixmap(brandIcon().pixmap(28, 28));
  ui->modeFieldsLayout->setAlignment(Qt::AlignVCenter);
  ui->optionsLayout->setAlignment(ui->btnToggleCore, Qt::AlignVCenter);
  ui->permissionLayout->setStretch(0, 1);
  ui->btnPermissionSettings->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  ui->btnPermissionRecheck->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  ui->computerValueLayout->setStretch(0, 1);
  ui->addressValueLayout->setStretch(ui->addressValueLayout->indexOf(ui->lblIpAddresses), 1);
  ui->btnEditName->setFixedSize(26, 28);
  ui->btnCopyAddress->setFixedSize(42, 26);
  ui->lblShareTitle->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  ui->lblShareHint->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  ui->btnHome->setIcon(homeIcon(HomeIcon::Monitor, QColor("#087fff")));
  ui->btnDevices->setIcon(homeIcon(HomeIcon::Panel));
  ui->btnLayout->setIcon(homeIcon(HomeIcon::Layout));
  ui->btnSettings->setIcon(homeIcon(HomeIcon::Settings));
  ui->btnCopyAddress->setIcon(homeIcon(HomeIcon::Copy, QColor("#267ed9")));
  ui->btnEditName->setIcon(homeIcon(HomeIcon::Edit, QColor("#267ed9")));
  ui->btnHelp->setIcon(homeIcon(HomeIcon::Help));
  ui->btnLogs->setIcon(homeIcon(HomeIcon::Logs));
  ui->btnSecurity->setIcon(homeIcon(HomeIcon::Shield, QColor("#438b7c")));
  ui->btnBrowseServers->setIcon(homeIcon(HomeIcon::Search, QColor("#267ed9")));
  ui->btnMore->setIcon(homeIcon(HomeIcon::Menu));
  ui->rbModeServer->setIcon(homeIcon(HomeIcon::Keyboard, QColor("#087fff")));
  ui->rbModeClient->setIcon(homeIcon(HomeIcon::MonitorDown));
  ui->widgetModeSelection->setButtons(ui->rbModeServer, ui->rbModeClient);
  ui->homeScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  auto homeScrollBar = ui->homeScroll->verticalScrollBar();
  connect(homeScrollBar, &QScrollBar::rangeChanged, this, [homeScrollBar](int minimum, int maximum) {
    homeScrollBar->setEnabled(maximum > minimum);
  });
  homeScrollBar->setEnabled(homeScrollBar->maximum() > homeScrollBar->minimum());
  for (auto button : {ui->btnHome, ui->btnDevices, ui->btnLayout, ui->btnSettings})
    button->setIconSize(QSize(22, 22));
  for (auto button : {ui->btnHelp, ui->btnLogs, ui->btnMore})
    button->setIconSize(QSize(21, 21));
  ui->btnEditName->setIconSize(QSize(16, 16));
  ui->btnCopyAddress->setIconSize(QSize(16, 16));
  connect(ui->btnWindowClose, &QPushButton::clicked, this, [this] {
    if (!maybeHideToTray())
      close();
  });
  connect(ui->btnWindowMinimize, &QPushButton::clicked, this, &MainWindow::showMinimized);
  connect(ui->btnWindowZoom, &QPushButton::clicked, this, [this] { isMaximized() ? showNormal() : showMaximized(); });
  qApp->installEventFilter(this);
  m_homeReady = true;
  m_pageStack = new QStackedWidget(ui->rightPanel);
  m_pageStack->setObjectName("mainPages");
  ui->rightPanelLayout->removeWidget(ui->homeScroll);
  m_pageStack->addWidget(ui->homeScroll);
  ui->rightPanelLayout->addWidget(m_pageStack, 1);
  ui->rightPanelLayout->addWidget(m_logDock);
  for (auto button : {ui->btnHome, ui->btnDevices, ui->btnLayout, ui->btnSettings}) {
    button->setCheckable(true);
    button->setAutoExclusive(true);
  }
  updateHomeMetrics();
  connect(m_statusBar, &StatusBar::statusTextChanged, ui->lblHomeStatus, &QLabel::setText);
  connect(m_statusBar, &QStatusBar::messageChanged, this, [this](const QString &text) {
    ui->lblHomeStatus->setText(text.isEmpty() ? m_statusBar->statusText() : text);
    if (m_devicesPage)
      m_devicesPage->message(text.isEmpty() ? m_statusBar->statusText() : text);
  });
  ui->lblHomeStatus->setText(m_statusBar->statusText());
  connect(m_pageStack, &QStackedWidget::currentChanged, this, &MainWindow::updatePageTitle);
  const auto showHome = [this] {
    m_pageStack->setCurrentWidget(ui->homeScroll);
    ui->btnHome->setChecked(true);
    ui->btnSettings->setChecked(false);
    ui->homeScroll->verticalScrollBar()->setValue(0);
  };
  connect(ui->btnHome, &QPushButton::clicked, this, showHome);
  connect(ui->btnHome, &QPushButton::toggled, this, [showHome](bool checked) {
    if (checked)
      showHome();
  });
  connect(ui->btnDevices, &QPushButton::toggled, this, [this](bool checked) {
    if (checked && (!m_devicesPage || m_pageStack->currentWidget() != m_devicesPage))
      openDevices();
  });
  const auto openLayout = [this] { showConfigureServer(""); };
  connect(ui->btnLayout, &QPushButton::toggled, this, [this, openLayout](bool checked) {
    if (checked && (!m_layoutPage || m_pageStack->currentWidget() != m_layoutPage))
      openLayout();
  });
  connect(ui->btnSettings, &QPushButton::toggled, this, [this](bool checked) {
    if (checked && (!m_settingsPage || m_pageStack->currentWidget() != m_settingsPage))
      openSettings();
  });
  connect(ui->btnPermissionRecheck, &QPushButton::clicked, this, &MainWindow::refreshPermissionStatus);
  connect(ui->btnPermissionSettings, &QPushButton::clicked, this, [] {
    QDesktopServices::openUrl(QUrl("x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"));
  });
#ifdef Q_OS_MACOS
  auto permissionTimer = new QTimer(this);
  permissionTimer->setInterval(2000);
  connect(permissionTimer, &QTimer::timeout, this, [this] {
    if (isVisible() && !ui->permissionBanner->isHidden())
      refreshPermissionStatus();
  });
  permissionTimer->start();
#endif
  connect(ui->btnHelp, &QPushButton::clicked, this, &MainWindow::showHelpViewer);
  connect(ui->btnLogs, &QPushButton::clicked, this, [this] { m_logDock->setVisible(!m_logDock->isVisible()); });
  auto more = new QMenu(ui->btnMore);
  more->setPalette(lightControlPalette());
  more->addAction(m_actionAbout);
  more->addAction(m_actionStartCore);
  more->addAction(m_actionRestartCore);
  more->addAction(m_actionStopCore);
  more->addAction(tr("Export configuration"), this, &MainWindow::saveServerConfig);
  more->addAction(tr("Client settings"), this, &MainWindow::showConfigureClient);
  more->addSeparator();
  more->addAction(m_actionSettings);
  more->addAction(m_logDock->toggleViewAction());
  more->addAction(m_actionShowHelp);
  more->addSeparator();
  more->addAction(m_actionQuit);
  ui->btnMore->setMenu(more);
  connect(ui->btnSecurity, &QPushButton::clicked, this, [this] {
    if (TlsUtility::isEnabled())
      showMyFingerprint();
    else
      openSettings();
  });
  connect(ui->btnCopyAddress, &QPushButton::clicked, this, [this] {
    if (!m_currentIpAddress.isEmpty()) {
      QGuiApplication::clipboard()->setText(m_currentIpAddress);
      m_statusBar->showMessage(tr("Address copied"), 2500);
    }
  });
}

void MainWindow::updateHomeDevices()
{
  if (auto *completion = findChild<QCompleter *>("homeComputerCompletion")) {
    QStringList addresses = Settings::value(Settings::Gui::RecentServers).toStringList();
    const auto known = Settings::value(Settings::Gui::KnownServers).toMap();
    addresses.append(known.keys());
    if (m_mdnsBrowser) {
      const auto ownAddresses = NetworkMonitor::validAddresses();
      for (const auto &server : m_mdnsBrowser->servers()) {
        if (server.ipAddress.isEmpty() || server.port < 1 || server.port > 65535 ||
            ownAddresses.contains(server.ipAddress))
          continue;
        const auto endpoint =
            DeviceListPage::normalizedAddress(QStringLiteral("%1:%2").arg(server.ipAddress).arg(server.port));
        if (!endpoint.isEmpty())
          addresses.append(endpoint);
      }
    }
    addresses.removeDuplicates();
    addresses.removeAll(QString());
    qobject_cast<QStringListModel *>(completion->model())->setStringList(addresses);
  }
  updateDevicesPage();
}

void MainWindow::ensureDeviceDiscovery()
{
  if (m_mdnsBrowser && m_mdnsBrowser->isFinished()) {
    delete m_mdnsBrowser;
    m_mdnsBrowser = nullptr;
  }
  if (!m_mdnsBrowser) {
    m_mdnsBrowser = new deskflow::mdns::MdnsBrowser(this);
    connect(m_mdnsBrowser, &deskflow::mdns::MdnsBrowser::serversChanged, this, [this] {
      updateHomeDevices();
      if (m_homeDiscoveryRequested && ui->lineHostname->hasFocus() && m_pageStack->currentWidget() == ui->homeScroll) {
        auto *completion = ui->lineHostname->completer();
        if (completion->model()->rowCount())
          completion->complete();
      }
    });
    connect(m_mdnsBrowser, &QThread::finished, this, [this] {
      if (m_devicesPage)
        m_devicesPage->setDiscoveryAvailable(false);
    });
    m_mdnsBrowser->start();
  }
}

void MainWindow::openDevices()
{
  if (!m_devicesPage) {
    m_devicesPage = new DeviceListPage(m_pageStack);
    m_pageStack->addWidget(m_devicesPage);
    connect(m_devicesPage, &DeviceListPage::layoutRequested, this, [this](const QString &name) {
      showConfigureServer("");
      if (!name.isEmpty())
        m_layoutPage->focusComputer(name);
    });
    connect(m_devicesPage, &DeviceListPage::settingsRequested, this, [this](const QString &name) {
      openSettings();
      if (!name.isEmpty())
        m_settingsPage->activateComputer(name);
    });
    connect(m_devicesPage, &DeviceListPage::diagnosisRequested, this, [this] {
      openSettings();
      m_settingsPage->activate(SettingsPage::Logs);
      m_logDock->show();
    });
    connect(m_devicesPage, &DeviceListPage::inputSettingsRequested, this, &MainWindow::showConfigureClient);
    connect(m_devicesPage, &DeviceListPage::refreshRequested, this, [this] {
      ensureDeviceDiscovery();
      m_mdnsBrowser->requestRefresh();
      updateDevicesPage();
      m_devicesPage->message(tr("Local discovery is continuous; newly found computers appear automatically."));
    });
    connect(m_devicesPage, &DeviceListPage::connectRequested, this, &MainWindow::connectDeviceServer);
    connect(m_devicesPage, &DeviceListPage::disconnectRequested, this, [this] {
      const auto selected = m_devicesPage->selectedDevice();
      m_coreProcess.transportCommand({{"kind", "disconnectName"}, {"name", selected.section(':', 1)}});
    });
    connect(
        m_devicesPage, &DeviceListPage::addServerRequested, this,
        [this](const QString &name, const QString &address) {
          if (!Settings::isWritable()) {
            m_devicesPage->message(tr("Settings are read-only."), true);
            return;
          }
          auto known = Settings::value(Settings::Gui::KnownServers).toMap();
          known.insert(address, name);
          Settings::setValue(Settings::Gui::KnownServers, known);
          Settings::save();
          updateDevicesPage();
          m_devicesPage->selectDevice("server:" + address);
          m_devicesPage->message(tr("Device saved. Select Connect when ready."));
        }
    );
  }
  ensureDeviceDiscovery();
  updateDevicesPage();
  m_pageStack->setCurrentWidget(m_devicesPage);
  ui->btnDevices->setChecked(true);
}

void MainWindow::updateDevicesPage()
{
  if (!m_devicesPage)
    return;
  using Kind = DeviceListPage::Kind;
  using State = DeviceListPage::State;
  using Device = DeviceListPage::Device;
  const auto local = Settings::value(Settings::Core::ComputerName).toString();
  const bool sharing = m_coreProcess.mode() == CoreMode::Server;
  const bool started = m_coreProcess.isStarted();
  const auto process = m_coreProcess.processState();
  const bool busy = process == ProcessState::Starting || process == ProcessState::Stopping;
  const auto recent = Settings::value(Settings::Gui::RecentComputers).toStringList();
  const auto recentServers = Settings::value(Settings::Gui::RecentServers).toStringList();
  const auto localAddress =
      m_currentIpAddress.isEmpty()
          ? QString()
          : QStringLiteral("%1:%2").arg(m_currentIpAddress).arg(Settings::value(Settings::Core::Port).toInt());
  QList<Device> devices{{"local", local, localAddress, tr("This computer"), Kind::Local, State::Local}};
  QMap<QString, Device> receivers, servers;
  if (!m_serverConfig.useExternalConfig()) {
    for (const auto &c : std::as_const(m_serverConfig).computers()) {
      if (c.isNull() || c.name() == local)
        continue;
      receivers.insert(
          c.name(), {"computer:" + c.name(),
                     c.name(),
                     {},
                     tr("Configured computer"),
                     Kind::Receiver,
                     State::Offline,
                     recent.contains(c.name()),
                     true}
      );
    }
  }
  const auto known = Settings::value(Settings::Gui::KnownServers).toMap();
  auto addServer = [&](const QString &address, const QString &name) {
    const auto key = DeviceListPage::normalizedAddress(address, Settings::value(Settings::Core::Port).toInt());
    if (key.isEmpty())
      return;
    if (!servers.contains(key))
      servers.insert(
          key, {"server:" + key, name.isEmpty() ? address : name, key, tr("Saved server"), Kind::Server, State::Offline,
                recentServers.contains(key), true}
      );
  };
  for (auto it = known.cbegin(); it != known.cend(); ++it)
    addServer(it.key(), it.value().toString());
  for (const auto &endpoint : recentServers)
    addServer(endpoint, known.value(endpoint).toString());
  const auto targets = ui->lineHostname->text().split(',', Qt::SkipEmptyParts);
  for (const auto &target : targets)
    addServer(
        target,
        known.value(DeviceListPage::normalizedAddress(target, Settings::value(Settings::Core::Port).toInt())).toString()
    );
  if (m_mdnsBrowser) {
    const auto ownAddresses = NetworkMonitor::validAddresses();
    for (const auto &s : m_mdnsBrowser->servers()) {
      if (s.ipAddress.isEmpty() || s.port < 1 || s.port > 65535 ||
          (s.name == local && ownAddresses.contains(s.ipAddress)))
        continue;
      const auto key = DeviceListPage::normalizedAddress(QStringLiteral("%1:%2").arg(s.ipAddress).arg(s.port));
      if (key.isEmpty())
        continue;
      if (!servers.contains(key))
        servers.insert(
            key, {"server:" + key, s.name.isEmpty() ? s.hostName : s.name, key, tr("Local discovery"), Kind::Server,
                  State::Discovered, recentServers.contains(key), false, true}
        );
      else {
        auto &d = servers[key];
        d.discovered = true;
        d.state = State::Discovered;
        if (d.name == d.address && !s.name.isEmpty())
          d.name = s.name;
      }
    }
  }
  for (const auto &name : recent) {
    if (name == local || receivers.contains(name))
      continue;
    // Legacy records have no role or endpoint metadata. Never offer them as a connectable server.
    if (servers.contains(DeviceListPage::normalizedAddress(name, Settings::value(Settings::Core::Port).toInt())))
      continue;
    receivers.insert(
        name, {"computer:" + name, name, {}, tr("Connection history"), Kind::History, State::Offline, true}
    );
  }
  if (sharing && started) {
    for (const auto &name : m_connectedClients) {
      if (!receivers.contains(name))
        receivers.insert(name, {"computer:" + name, name, {}, tr("Current connection"), Kind::Receiver});
      receivers[name].state = State::Connected;
    }
  } else if (!sharing && started && m_coreProcess.connectionState() == ConnectionState::Connected &&
             targets.size() == 1) {
    const auto key = DeviceListPage::normalizedAddress(targets.first(), Settings::value(Settings::Core::Port).toInt());
    if (servers.contains(key))
      servers[key].state = State::Connected;
  }
  for (const auto &d : receivers)
    devices.append(d);
  for (const auto &d : servers)
    devices.append(d);
  m_devicesPage->setDevices(devices, sharing, m_statusBar->statusText(), busy);
  m_devicesPage->setDiscoveryAvailable(m_mdnsBrowser && m_mdnsBrowser->isRunning());
}

void MainWindow::connectDeviceServer(const QString &address, const QString &name, bool autoConnect)
{
  const auto endpoint = DeviceListPage::normalizedAddress(address);
  if (endpoint.isEmpty() || !Settings::isWritable()) {
    m_devicesPage->message(tr("The server address is invalid or settings are read-only."), true);
    return;
  }
  if (m_coreProcess.processState() == ProcessState::Starting || m_coreProcess.processState() == ProcessState::Stopping)
    return;
  auto known = Settings::value(Settings::Gui::KnownServers).toMap();
  known.insert(endpoint, name);
  Settings::setValue(Settings::Gui::KnownServers, known);
  Settings::setValue(Settings::Client::AutoConnect, autoConnect);
  ui->lineHostname->setText(endpoint);
  // Radio toggling uses the existing mode transition and stops sharing before switching to receiving.
  ui->rbModeClient->setChecked(true);
  Settings::save();
  if (m_coreProcess.processState() == ProcessState::Stopped)
    startCore();
  else
    resetCore();
  updateDevicesPage();
}

void MainWindow::rememberHomeDevice(const QString &name)
{
  if (name.isEmpty())
    return;
  auto recent = Settings::value(Settings::Gui::RecentComputers).toStringList();
  recent.removeAll(name);
  recent.prepend(name);
  while (recent.size() > 8)
    recent.removeLast();
  Settings::setValue(Settings::Gui::RecentComputers, recent);
}

void MainWindow::updatePageTitle()
{
  if (!m_pageStack)
    return;
  const auto page = m_pageStack->currentWidget();
  ui->lblWorkspaceTitle->setText(
      page == m_settingsPage  ? tr("Advanced settings")
      : page == m_layoutPage  ? tr("Screen layout")
      : page == m_devicesPage ? tr("Devices")
                              : tr("Home")
  );
}

void MainWindow::updateHomeMetrics()
{
  if (m_actionTransition && m_actionTransition->state() == QAbstractAnimation::Running) {
    m_actionTransition->stop();
    m_actionTransitionOverlay->hide();
  }
  const bool compact = width() <= 800;
  ui->sidebar->setFixedWidth(compact ? 170 : 182);
  const int margin = compact ? 20 : 28;
  ui->contentLayout->setContentsMargins(margin, 18, margin, 16);
  ui->toolbarLayout->setContentsMargins(margin, 18, margin, 18);
  ui->localCardLayout->setContentsMargins(0, 0, 0, 0);
  ui->localCardLayout->setSpacing(compact ? 14 : 24);
  const auto name = Settings::value(Settings::Core::ComputerName).toString();
  ui->lblSidebarName->setMaximumWidth(compact ? 142 : 154);
  ui->lblSidebarName->setText(ui->lblSidebarName->fontMetrics().elidedText(name, Qt::ElideRight, compact ? 142 : 154));
  if (ui->topLevelWidget->property("compact").toBool() != compact) {
    ui->topLevelWidget->setProperty("compact", compact);
    ui->topLevelWidget->setStyleSheet(ui->topLevelWidget->styleSheet());
  }
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
  QMainWindow::resizeEvent(event);
  if (m_homeReady)
    updateHomeMetrics();
}

void MainWindow::updateHomeStatus()
{
  QString text;
  switch (m_coreProcess.processState()) {
    using enum ProcessState;
  case Stopped:
    text = tr("Not running");
    break;
  case Starting:
    text = tr("Starting…");
    break;
  case Stopping:
    text = tr("Stopping…");
    break;
  case RetryPending:
    text = tr("Waiting to retry…");
    break;
  case Started:
    if (m_coreProcess.connectionState() == ConnectionState::Connected)
      text = tr("Connected");
    else if (m_coreProcess.mode() == CoreMode::Server)
      text = tr("Waiting for computers");
    else if (m_coreProcess.connectionState() == ConnectionState::Connecting)
      text = tr("Connecting…");
    else
      text = tr("Disconnected");
    break;
  }
  ui->lblSidebarStatus->setText(text);
  ui->lblStateDot->setProperty("ready", m_coreProcess.connectionState() == ConnectionState::Connected);
  ui->lblStateDot->style()->unpolish(ui->lblStateDot);
  ui->lblStateDot->style()->polish(ui->lblStateDot);
  ui->btnSecurity->setText(TlsUtility::isEnabled() ? tr("Transport encryption enabled") : tr("Encryption disabled"));
  ui->btnSecurity->setIcon(homeIcon(HomeIcon::Shield, TlsUtility::isEnabled() ? QColor("#438b7c") : QColor("#bf8a38")));
  ui->btnSecurity->setToolTip(
      tr("Transport encryption is configured. An established connection is required to confirm its security. View this "
         "computer’s certificate fingerprint.")
  );
}

void MainWindow::refreshPermissionStatus()
{
#ifdef Q_OS_MACOS
  ui->permissionBanner->setVisible(!hasOSXAccessibilityPermission());
#else
  ui->permissionBanner->hide();
#endif
  ui->permissionSpace->changeSize(
      0, ui->permissionBanner->isHidden() ? 0 : 16, QSizePolicy::Minimum, QSizePolicy::Fixed
  );
  ui->contentLayout->invalidate();
  toggleCanRunCore(canRunCore());
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

  connect(m_actionQuit, &QAction::triggered, this, &MainWindow::close);
  connect(m_actionTrayQuit, &QAction::triggered, this, &MainWindow::close);
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
  connect(&m_coreProcess, &CoreProcess::missingKeyboardLayouts, this, &MainWindow::handleMissingKeyboardLayouts);

  if (Settings::value(Settings::Gui::AutoStartCore).toBool()) {
    connect(ui->btnToggleCore, &QPushButton::clicked, m_actionStopCore, &QAction::trigger, Qt::UniqueConnection);
  } else {
    connect(ui->btnToggleCore, &QPushButton::clicked, m_actionStartCore, &QAction::trigger, Qt::UniqueConnection);
  }

  connect(ui->btnRestartCore, &QPushButton::clicked, this, &MainWindow::resetCore);

  connect(ui->lineHostname, &QLineEdit::returnPressed, this, [this] {
    if (!canRunCore())
      return;
    if (m_coreProcess.isStarted())
      resetCore();
    else
      startCore();
  });
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
  // Diagnostics remain inside the resizable homepage shell.
  Settings::setValue(Settings::Gui::WindowGeometry, geometry());
}

void MainWindow::settingsChanged(const QString &key)
{
  if (key == Settings::Connection::InitialTransport || key == Settings::Connection::BluetoothPeer) {
    updateModeControls();
    return;
  }
  if (key == Settings::Log::Level) {
    m_coreProcess.applyLogLevel();
    return;
  }

  if (key == Settings::Core::ComputerName)
    updateComputerName();

  if ((key == Settings::Security::Certificate) || (key == Settings::Security::KeySize) ||
      (key == Settings::Security::TlsEnabled) || (key == Settings::Security::CheckPeers)) {
    if (TlsUtility::isEnabled()) {
      if (!TlsUtility::isCertValid()) {
        qWarning() << tr("invalid certificate, generating a new one");
        TlsUtility::generateCertificate();
      }
      m_fingerprint = {QCryptographicHash::Sha256, TlsUtility::certFingerprint()};
      updateFingerprintButton();
    }
    updateSecurityIcon(m_statusBar->securityIconVisible());
    updateHomeStatus();
    return;
  }
}

void MainWindow::serverConfigSaving()
{
  m_serverConfig.commit();
  updateHomeDevices();
}

void MainWindow::trayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
  if (reason != QSystemTrayIcon::Trigger)
    return;
  isVisible() ? hide() : showAndActivate();
}

void MainWindow::coreProcessError(CoreProcess::Error error)
{
  if (error == CoreProcess::Error::AccessibilityRequired) {
    refreshPermissionStatus();
    m_actionStartCore->setVisible(true);
    m_actionRestartCore->setVisible(false);
    updateModeControlLabels();
    showAndActivate();
    ui->homeScroll->ensureWidgetVisible(ui->permissionBanner);
  } else if (error == CoreProcess::Error::AddressMissing) {
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
  // The child process reads these values from disk, including a just-selected transport.
  if (m_coreProcess.mode() == CoreMode::Server && Settings::value(Settings::Connection::ShareBluetooth).toBool() &&
      (!Settings::value(Settings::Security::TlsEnabled).toBool() ||
       !Settings::value(Settings::Security::CheckPeers).toBool())) {
    m_statusBar->showMessage(tr("Enable encryption and certificate verification to share over Bluetooth."));
    return;
  }
  Settings::save(false);
  // An explicit new connection may ask again after a rejected trust prompt.
  // Background retries retain prompt suppression within CoreProcess.
  m_checkedServers.clear();
  m_checkedClients.clear();

  // Save current IP state when server starts
  if (m_coreProcess.mode() == CoreMode::Server && Settings::value(Settings::Core::Interface).toString().isEmpty()) {
    m_serverStartIPs = NetworkMonitor::validAddresses();
    m_serverStartSuggestedIP = m_serverStartIPs.isEmpty() ? "" : m_serverStartIPs.first();
  }

  m_actionStartCore->setVisible(false);
  m_actionRestartCore->setVisible(true);
  m_coreProcess.start();
}

void MainWindow::stopCore()
{
  qDebug() << "stopping core process";
  m_coreProcess.stop();
  m_actionStartCore->setVisible(true);
  m_actionRestartCore->setVisible(false);
}

void MainWindow::clearSettings()
{
  qDebug() << "clearing settings";

  m_networkMonitor->stopMonitoring();

  disconnect(&m_coreProcess, nullptr, this, nullptr);
  disconnect(&m_versionChecker, nullptr, this, nullptr);
  disconnect(m_guiDupeChecker, nullptr, this, nullptr);
  disconnect(m_trayIcon, nullptr, this, nullptr);
  disconnect(m_logDock->toggleViewAction(), nullptr, this, nullptr);

  m_coreProcess.stop();
  m_coreProcess.clearSettings();

  m_saveOnExit = false;
  diagnostic::clearSettings(true);
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
  if (!m_settingsPage) {
    m_settingsPage = new SettingsPage(m_serverConfig, m_pageStack);
    m_pageStack->addWidget(m_settingsPage);
    connect(m_settingsPage, &SettingsPage::clearSettingsRequested, this, &MainWindow::clearSettings);
    connect(m_settingsPage, &SettingsPage::showLogsRequested, m_logDock, &QWidget::show);
    connect(m_settingsPage, &SettingsPage::saved, this, [this](bool restart) {
      applyConfig();
      if (restart && m_coreProcess.isStarted())
        m_coreProcess.restart();
    });
  }
  m_settingsPage->activate();
  m_pageStack->setCurrentWidget(m_settingsPage);
  ui->btnHome->setChecked(false);
  ui->btnSettings->setChecked(true);
}

void MainWindow::resetCore()
{
  m_coreProcess.restart();
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

  bool animate = m_actionTransition && ui->homeActionCard->isVisible();
#ifdef Q_OS_MACOS
  animate = animate && !prefersOSXReducedMotion();
#endif
  if (animate) {
    // Capture the displayed composite, including an interrupted crossfade.
    const auto frame = ui->homeContent->grab(ui->homeActionCard->geometry());
    m_actionTransition->stop();
    m_actionTransitionOverlay->setPixmap(frame);
    m_actionTransitionOverlay->setGeometry(ui->homeActionCard->geometry());
    m_actionTransitionOverlay->show();
    m_actionTransitionOverlay->raise();
    m_actionTransition->setStartValue(1.0);
    m_actionTransition->setEndValue(0.0);
    m_actionTransition->start();
  }

  // Settings::save() also refreshes device cards. Include that refresh in the
  // same transaction as changing the fields, rather than painting a mixed mode.
  const auto layoutTransaction = deferHomeLayout(ui->homeContent, ui->contentLayout);
  Settings::CoreMode mode = Settings::CoreMode::None;

  if (ui->rbModeServer->isChecked())
    mode = Settings::CoreMode::Server;
  if (ui->rbModeClient->isChecked())
    mode = Settings::CoreMode::Client;

  qDebug() << QStringLiteral("change mode to: %1").arg(QVariant::fromValue(mode).toString());

  if (m_coreProcess.processState() != ProcessState::Stopped && m_coreProcess.mode() != mode)
    m_coreProcess.stop();
  m_coreProcess.setMode(mode);

  Settings::setValue(Settings::Core::CoreMode, mode);
  Settings::save();

  updateModeControls();
}

void MainWindow::updateModeControls()
{
  // Show/hide all mode-specific fields as a single layout transaction. Showing the
  // sharing panel before hiding the receiver panel otherwise briefly displays both,
  // which can make the scroll area resize and repaint the whole page twice.
  const auto layoutTransaction = deferHomeLayout(ui->homeContent, ui->contentLayout);
  const auto mode = m_coreProcess.mode();
  const bool isServer = mode == Settings::CoreMode::Server;
  const bool isClient = mode == Settings::CoreMode::Client;
  const bool bluetooth = m_backupPanel && m_backupPanel->bluetoothSelected();

  ui->lineHostname->setPlaceholderText(tr("Choose a computer or enter an IP"));
  ui->serverOptions->hide();
  auto *inputStack = findChild<QStackedWidget *>("connectionInputStack");
  inputStack->setCurrentIndex(bluetooth ? 1 : 0);
  inputStack->currentWidget()->show();
  ui->modeFields->setVisible(isClient);
  if (m_backupPanel) {
    m_backupPanel->setServerMode(isServer);
  }
  ui->m_pLabelServerName->hide();
  ui->lblShareTitle->setText(
      isServer ? tr("Share this computer’s keyboard and mouse") : tr("Use another computer’s input")
  );
  ui->lblNoMode->setVisible(!isServer && !isClient);
  ui->btnConfigureServer->setVisible(isServer);
  ui->btnConfigureServer->setText(tr("Screen layout") + QStringLiteral("    ›"));
  ui->btnSaveServerConfig->hide();
  ui->btnConfigureClient->hide();
  ui->btnBrowseServers->setVisible(isClient);
  ui->btnBrowseServers->setText(bluetooth ? tr("Refresh devices") : tr("Find computers nearby"));
  ui->lblShareHint->setText(
      bluetooth  ? tr("Choose a nearby BMouse computer. Its identity is verified when connecting.")
      : isServer ? tr("Use this computer’s keyboard and mouse to control your other computers across screen edges.")
      : isClient ? tr("Connect to the computer whose keyboard and mouse you want to use on this computer.")
                 : tr("Choose how this computer will work.")
  );
  toggleCanRunCore(canRunCore());
  updateNetworkInfo();
  m_networkMonitor->startMonitoring();
  updateHomeDevices();
  updateHomeStatus();

  if (isServer || isClient)
    updateModeControlLabels();
}

void MainWindow::updateModeControlLabels()
{
  const bool isServer = m_coreProcess.mode() == CoreMode::Server;
  const auto process = m_coreProcess.processState();
  const bool isStarted =
      process == ProcessState::Started || process == ProcessState::Starting || process == ProcessState::RetryPending;
  ui->btnRestartCore->hide();

  QString startText;
  QString stopText;
  QIcon startIcon;
  QIcon stopIcon;

  if (isServer) {
    startText = tr("Start sharing");
    stopText = tr("Stop sharing");
    startIcon = homeIcon(HomeIcon::Play, Qt::white);
    stopIcon = homeIcon(HomeIcon::Stop, Qt::white);
  } else {
    startText = tr("Connect");
    stopText = tr("Disconnect");
    startIcon = homeIcon(HomeIcon::Monitor, Qt::white);
    stopIcon = homeIcon(HomeIcon::Stop, Qt::white);
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
  int actionWidth = 124;
  for (const auto &text : {tr("Start sharing"), tr("Stop sharing"), tr("Connect"), tr("Disconnect")})
    actionWidth = qMax(actionWidth, ui->btnToggleCore->fontMetrics().horizontalAdvance(text) + 42);
  ui->btnToggleCore->setFixedWidth(actionWidth);
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
  updateIpLabel(NetworkMonitor::validAddresses());
}

void MainWindow::serverConnectionConfigureClient(const QString &clientName)
{
  showConfigureServer("");
  m_layoutPage->suggestComputer(clientName);
}

//////////////////////////////////////////////////////////////////////////////
// End slots
//////////////////////////////////////////////////////////////////////////////

void MainWindow::open()
{
  if (!Settings::value(Settings::Gui::Autohide).toBool())
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
    Settings::setValue(Settings::Gui::AutoUpdateCheck, messages::showUpdateCheckOption(this));
  }

  if (Settings::value(Settings::Gui::AutoUpdateCheck).toBool()) {
    m_versionChecker.checkLatest();
  } else {
    qDebug() << "skipping check for new version, disabled";
  }

  const bool autoConnect = ui->rbModeClient->isChecked() && Settings::value(Settings::Client::AutoConnect).toBool();
  if (Settings::value(Settings::Gui::AutoStartCore).toBool() || autoConnect) {
    if (ui->rbModeClient->isChecked() && ui->lineHostname->text().isEmpty())
      return;
    startCore();
  }
}

void MainWindow::createMenuBar()
{
  // Keep shortcuts active without an in-window menu on Linux and Windows.
  addActions({m_actionQuit, m_actionSettings, m_actionShowHelp});
#ifdef Q_OS_MACOS
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
#endif
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

  coreModeToggled(true);
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
  if (!Settings::value(Settings::Gui::SymbolicTrayIcon).toBool()) {
    m_trayIcon->setIcon(brandIcon());
    return;
  }
  if (deskflow::platform::isWindows()) {
    QSettings settings(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat
    );
    const QString theme = settings.value(QStringLiteral("SystemUsesLightTheme"), 1).toBool() ? QStringLiteral("light")
                                                                                             : QStringLiteral("dark");
    m_trayIcon->setIcon(brandTrayIcon(theme == QStringLiteral("dark")));
    return;
  }
  m_trayIcon->setIcon(brandTrayIcon(iconMode() == QStringLiteral("dark"), deskflow::platform::isMac()));
}

void MainWindow::handleLogLine(const QString &line)
{
  m_logDock->appendLine(line);
}

void MainWindow::handleUnrecognisedClient(const QString &clientName)
{
  if (m_ignoredClients.contains(clientName)) {
    qDebug("ignoring %s:", qPrintable(clientName));
    return;
  }

  if (m_newClientPromptShowing || m_serverConfigDialogVisible)
    return;

  if (Settings::value(Settings::Server::ExternalConfig).toBool())
    return;

  if (m_serverConfig.isFull() || m_serverConfig.computerExists(clientName))
    return;

  m_newClientPromptShowing = true;

  showAndActivate();

  if (deskflow::gui::messages::showNewClientPrompt(this, clientName)) {
    serverConnectionConfigureClient(clientName);
  } else {
    m_ignoredClients.insert(clientName);
  }

  m_newClientPromptShowing = false;
}

void MainWindow::handleConnectionRefused(deskflow::core::ConnectionRefusal reason)
{
  if (reason != deskflow::core::ConnectionRefusal::AlreadyConnected)
    return;

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

void MainWindow::handlePeerFingerprint(const QString &fingerprint)
{
  const auto sha256Text = QString(fingerprint).remove(':');
  const Fingerprint sha256 = {QCryptographicHash::Sha256, QByteArray::fromHex(sha256Text.toLatin1())};

  const bool isClient = m_coreProcess.mode() == CoreMode::Client;
  if ((isClient && m_checkedServers.contains(sha256Text)) || (!isClient && m_checkedClients.contains(sha256Text))) {
    qDebug("ignoring fingerprint, already handled");
    return;
  }

  FingerprintDatabase db;
  db.read(trustedFingerprintDatabase());

  if (db.isTrusted(sha256)) {
    qDebug("fingerprint is trusted");
    return;
  }

  if (isClient) {
    m_checkedServers.append(sha256Text);
    m_coreProcess.stop();
  } else {
    m_checkedClients.append(sha256Text);
  }

  auto mode = isClient ? FingerprintDialogMode::Client : FingerprintDialogMode::Server;
  FingerprintDialog fingerprintDialog(this, m_fingerprint, mode, sha256);

  if (fingerprintDialog.exec() == QDialog::Accepted) {
    db.addTrusted(sha256);
    if (!db.write(trustedFingerprintDatabase())) {
      qCritical().noquote() << "unable to write fingerprint to db:" << trustedFingerprintDatabase();
      m_coreProcess.stop();
      return;
    }
    if (isClient) {
      m_checkedServers.removeAll(sha256Text);
      m_coreProcess.start();
    } else {
      m_checkedClients.removeAll(sha256Text);
    }
  }
}

bool MainWindow::maybeHideToTray()
{
  if (!Settings::value(Settings::Gui::CloseToTray).toBool()) {
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
  if (event->spontaneous() && maybeHideToTray()) {
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
  if (process == Stopped || !isServer)
    m_connectedClients.clear();
  updateHomeStatus();
  updateHomeDevices();
}

void MainWindow::coreProcessStateChanged(ProcessState state)
{
  using enum ProcessState;
  updateStatus();
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
  toggleCanRunCore(canRunCore());
  updateModeControlLabels();
}

void MainWindow::coreConnectionStateChanged(ConnectionState state)
{
  qDebug() << "core connection state changed:" << static_cast<int>(state);

  updateStatus();

  // always assume connection is not secure when connection changes
  // to anything except connected. the only way the padlock shows is
  // when the correct TLS version string is detected.
  if (state != ConnectionState::Connected) {
    secureSocket(false);
  } else {
    if (m_coreProcess.mode() == CoreMode::Client && !ui->lineHostname->text().contains(',')) {
      rememberHomeDevice(ui->lineHostname->text().trimmed());
      const auto endpoint =
          DeviceListPage::normalizedAddress(ui->lineHostname->text(), Settings::value(Settings::Core::Port).toInt());
      if (!endpoint.isEmpty()) {
        auto recent = Settings::value(Settings::Gui::RecentServers).toStringList();
        recent.removeAll(endpoint);
        recent.prepend(endpoint);
        while (recent.size() > 8)
          recent.removeLast();
        Settings::setValue(Settings::Gui::RecentServers, recent);
      }
    }
    updateDevicesPage();
    if (isVisible())
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
  if (e->type() == QEvent::ActivationChange && isActiveWindow()) {
    refreshPermissionStatus();
  } else if (e->type() == QEvent::PaletteChange) {
    updateIconTheme();
    setWindowIcon(applicationIcon());
    setTrayIcon();
  } else if (e->type() == QEvent::LanguageChange) {
    ui->retranslateUi(this);
    updateModeControls();
    updateStatus();
    serverClientsChanged(m_connectedClients);
    updateText();
    refreshPermissionStatus();
  }
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
  if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
    auto button = qobject_cast<QPushButton *>(obj);
    if (button && button->window() == this) {
      const auto reason = static_cast<QFocusEvent *>(event)->reason();
      button->setProperty(
          "keyboardFocus",
          event->type() == QEvent::FocusIn && (reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason)
      );
      button->style()->unpolish(button);
      button->style()->polish(button);
    }
  }
  if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
    auto widget = qobject_cast<QWidget *>(obj);
    auto mouse = static_cast<QMouseEvent *>(event);
    if (widget && widget->window() == this && mouse->button() == Qt::LeftButton) {
      const auto point = mapFromGlobal(mouse->globalPosition().toPoint());
      Qt::Edges edges;
      if (point.x() < 6)
        edges |= Qt::LeftEdge;
      if (point.x() > width() - 6)
        edges |= Qt::RightEdge;
      if (point.y() < 6)
        edges |= Qt::TopEdge;
      if (point.y() > height() - 6)
        edges |= Qt::BottomEdge;
      if (edges && windowHandle() && !isMaximized())
        return windowHandle()->startSystemResize(edges);
      const bool title =
          obj == ui->toolbar || obj == ui->lblWorkspaceTitle || obj == ui->sideTitleBar || obj == ui->lblVersion;
      if (title && windowHandle()) {
        if (event->type() == QEvent::MouseButtonDblClick)
          isMaximized() ? showNormal() : showMaximized();
        else
          windowHandle()->startSystemMove();
        return true;
      }
    }
  }
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
  updatePageTitle();
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
  Q_UNUSED(message);
  if (!m_layoutPage) {
    m_layoutPage = new ScreenLayoutPage(m_serverConfig, m_pageStack);
    m_pageStack->addWidget(m_layoutPage);
    connect(m_layoutPage, &ScreenLayoutPage::saved, this, [this] {
      updateHomeDevices();
      if (m_coreProcess.isStarted())
        m_coreProcess.restart();
    });
    connect(m_pageStack, &QStackedWidget::currentChanged, this, [this] {
      m_serverConfigDialogVisible = m_pageStack->currentWidget() == m_layoutPage;
    });
  }
  m_layoutPage->activate();
  m_pageStack->setCurrentWidget(m_layoutPage);
  ui->btnLayout->setChecked(true);
}

void MainWindow::showConfigureClient()
{
  openSettings();
  m_settingsPage->activate(SettingsPage::Input);
}

void MainWindow::secureSocket(bool secureSocket)
{
  m_secureSocket = secureSocket;
  updateSecurityIcon(m_statusBar->securityIconVisible());
}

void MainWindow::updateComputerName()
{
  const auto computerName = Settings::value(Settings::Core::ComputerName).toString();
  ui->lblComputerName->setIdentity(computerName);
  ui->lineEditName->setText(computerName);
  ui->lblSidebarName->setText(
      ui->lblSidebarName->fontMetrics().elidedText(computerName, Qt::ElideRight, width() <= 800 ? 142 : 154)
  );
  ui->lblSidebarName->setToolTip(computerName);
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

  ui->lblComputerName->setIdentity(ui->lineEditName->text());
  Settings::setValue(Settings::Core::ComputerName, ui->lineEditName->text());
  if (isServer)
    serverConfig().updateServerName();
  applyConfig();
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
  m_connectedClients = clients;
  for (const auto &client : clients)
    rememberHomeDevice(client);
  m_statusBar->setServerClients(clients);
  updateHomeDevices();
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
  bool permission = true;
#ifdef Q_OS_MACOS
  permission = hasOSXAccessibilityPermission();
#endif
  // Stopping an existing session must remain possible even if permission is revoked.
  const auto process = m_coreProcess.processState();
  enableButtons = enableButtons && permission && process != ProcessState::Stopping;
  const bool canStop = isStarted || process == ProcessState::Starting || process == ProcessState::RetryPending;
  ui->btnToggleCore->setEnabled(enableButtons || canStop);
  ui->btnRestartCore->setEnabled(enableButtons && isStarted);
  m_actionStartCore->setEnabled(enableButtons);
  m_actionStopCore->setEnabled(canStop);
  ui->lblActionHint->setText(!permission && !canStop ? tr("Authorize keyboard and mouse control first") : QString());
  ui->btnToggleCore->setToolTip(
      !permission && !canStop ? tr("Authorize keyboard and mouse control first")
      : !enableButtons && !canStop
          ? (m_backupPanel && m_backupPanel->bluetoothSelected() ? tr("Select the other computer’s Bluetooth device")
                                                                 : tr("Enter the other computer’s address to connect"))
          : QString()
  );
}

void MainWindow::remoteHostChanged(const QString &newRemoteHost)
{
  m_coreProcess.setAddress(newRemoteHost);
  if (newRemoteHost.isEmpty()) {
    Settings::setValue(Settings::Client::RemoteHost);
  } else {
    Settings::setValue(Settings::Client::RemoteHost, newRemoteHost);
  }
  toggleCanRunCore(canRunCore());
  updateHomeDevices();
}

void MainWindow::browseServers()
{
  if (m_coreProcess.processState() != ProcessState::Stopped)
    return;
  if (m_backupPanel && m_backupPanel->bluetoothSelected()) {
    m_backupPanel->discover();
    return;
  }
  ensureDeviceDiscovery();
  updateHomeDevices();
  m_homeDiscoveryRequested = true;
  ui->lineHostname->setFocus();
  auto *completion = ui->lineHostname->completer();
  completion->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
  completion->setCompletionPrefix({});
  if (completion->model()->rowCount())
    completion->complete();
  else
    m_statusBar->showMessage(tr("Looking for computers on the local network. You can also enter an IP."));
}

void MainWindow::updateIpLabel(const QStringList &addresses)
{
  const auto fixedAddress = Settings::value(Settings::Core::Interface).toString();
  const bool boundServer = m_coreProcess.mode() == CoreMode::Server && m_coreProcess.isStarted();
  const auto preferred = fixedAddress.isEmpty() && boundServer ? m_serverStartSuggestedIP : fixedAddress;
  m_currentIpAddress = preferred.isEmpty() ? addresses.value(0) : preferred;
  const bool available = !m_currentIpAddress.isEmpty() && addresses.contains(m_currentIpAddress);
  ui->lblIpAddresses->setText(m_currentIpAddress.isEmpty() ? tr("No network address") : m_currentIpAddress);
  ui->lblIpAddresses->setToolTip(addresses.join('\n'));
  ui->lblAddressHint->setText(
      !available ? tr("Check your network or bound address before connecting.")
                 : tr("Connect from another computer on your network.")
  );
  ui->lblAddressHint->hide();
  ui->localCard->setToolTip(!available ? ui->lblAddressHint->text() : QString());
  ui->localCard->setFixedHeight(112);
  ui->btnCopyAddress->setEnabled(!m_currentIpAddress.isEmpty());
  if (auto *state = findChild<QLabel *>("lblLocalNetworkState");
      state && (m_coreProcess.processState() == ProcessState::Stopped || m_coreProcess.mode() != CoreMode::Server))
    state->setText(available ? tr("Available") : tr("Unavailable"));
  ui->lineShareAddress->setText(ui->lblIpAddresses->text());
  m_copyShareAddress->setEnabled(ui->btnCopyAddress->isEnabled());
  updateDevicesPage();
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
  const auto mode = m_coreProcess.mode();
  const bool isServer = mode == Settings::CoreMode::Server;
  const bool isClient = mode == Settings::CoreMode::Client;
  if (isClient && Settings::value(Settings::Connection::InitialTransport) == "bluetooth")
    return m_backupPanel && m_backupPanel->bluetoothTargetReady();
  return ((isServer || isClient) && (isClient && !ui->lineHostname->text().isEmpty()) || isServer);
}
