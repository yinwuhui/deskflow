/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "gui/MainWindow.h"
#include "gui/core/BackupConnectionPanel.h"
#include "gui/widgets/LogDock.h"

#include "gui/widgets/HomeDeviceList.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocalServer>
#include <QMenuBar>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTranslator>
#include <QtTest>

static bool inputControlAllowed()
{
#ifdef Q_OS_MACOS
  return hasOSXAccessibilityPermission();
#else
  return true;
#endif
}

class MainWindowHomeTests : public QObject
{
  Q_OBJECT
  static QJsonObject backupSnapshot(int revision, const QString &backup = "ready", const QString &actionId = {})
  {
    return {
        {"version", 2},
        {"revision", revision},
        {"backupPeer", "peer-a"},
        {"peers", QJsonArray{QJsonObject{
                      {"peerId", "peer-a"},
                      {"name", "Test laptop"},
                      {"requested", true},
                      {"effective", true},
                      {"backup", backup},
                      {"state", "network"},
                      {"actionId", actionId},
                      {"action", actionId.isEmpty() ? "" : "confirmPairing"},
                      {"reason", "428196"},
                      {"channels", QJsonArray{
                                       QJsonObject{{"type", "network"}, {"state", "active"}},
                                       QJsonObject{{"type", "bluetooth"}, {"state", "ready"}}
                                   }}
                  }}}
    };
  }

private Q_SLOTS:
  void backupSwitchUpdatesOnlyTheSelectedPeer()
  {
    Settings::setValue(Settings::Security::TlsEnabled, true);
    Settings::setValue(Settings::Security::CheckPeers, true);
    BackupConnectionPanel panel;
    panel.setRunning(true);
    QSignalSpy commands(&panel, &BackupConnectionPanel::command);
    panel.updateSnapshot(backupSnapshot(1));
    QCOMPARE(commands.count(), 0);
    QVERIFY(panel.detailsWidget()->isHidden());
    auto toggle = panel.findChild<QCheckBox *>("checkAutoBackup");
    QVERIFY(toggle->isChecked());
    toggle->click();
    QCOMPARE(commands.count(), 1);
    const auto request = commands.takeFirst().first().toJsonObject();
    QCOMPARE(request.value("kind").toString(), QString("policy"));
    QCOMPARE(request.value("peerId").toString(), QString("peer-a"));
    QCOMPARE(request.value("enabled").toBool(), false);
    QVERIFY(panel.findChild<QLabel *>("lblBackupStatus")->text().contains("Using local network"));
  }

  void backupSnapshotsRejectOldRevisionsAndResetOnStop()
  {
    Settings::setValue(Settings::Security::TlsEnabled, true);
    Settings::setValue(Settings::Security::CheckPeers, true);
    BackupConnectionPanel panel;
    panel.setRunning(true);
    panel.updateSnapshot(backupSnapshot(5));
    panel.updateSnapshot(backupSnapshot(4, "unavailable"));
    QVERIFY(panel.findChild<QLabel *>("lblBackupStatus")->text().contains("Backup ready"));
    QVERIFY(panel.detailsWidget()->isHidden());
    panel.setRunning(false);
    panel.updateSnapshot(backupSnapshot(8));
    QVERIFY(panel.selectedPeer().isEmpty());
    panel.setRunning(true);
    panel.updateSnapshot(backupSnapshot(1, "blockedByPeer"));
    QVERIFY(panel.findChild<QLabel *>("lblBackupStatus")->text().contains("disabled by the other computer"));
  }

  void expiredPairingConfirmationCannotBeSubmitted()
  {
    BackupConnectionPanel panel;
    panel.setRunning(true);
    panel.updateSnapshot(backupSnapshot(1, "needsAction", "old-action"));
    panel.findChild<QPushButton *>("btnBackupDetails")->click();
    QSignalSpy commands(&panel, &BackupConnectionPanel::command);
    panel.updateSnapshot(backupSnapshot(2, "needsAction", "new-action"));
    panel.findChild<QPushButton *>("btnConfirmBackupAction")->click();
    QCOMPARE(commands.count(), 0);
    panel.findChild<QPushButton *>("btnBackupDetails")->click();
    panel.findChild<QPushButton *>("btnConfirmBackupAction")->click();
    QCOMPARE(commands.count(), 1);
    const auto request = commands.takeFirst().first().toJsonObject();
    QCOMPARE(request.value("actionId").toString(), QString("new-action"));
    QCOMPARE(request.value("peerId").toString(), QString("peer-a"));
    QVERIFY(request.value("accepted").toBool());
  }

  void bothTransportsAndRolesKeepActionAnchors()
  {
    MainWindow window;
    window.resize(740, 600);
    window.show();
    QCoreApplication::processEvents();
    auto action = window.findChild<QPushButton *>("btnToggleCore");
    auto content = window.findChild<QWidget *>("homeContent");
    auto local = window.findChild<QWidget *>("localCard");
    const QRect anchor(action->mapTo(content, QPoint()), action->size());
    const auto localRect = local->geometry();
    for (auto role : {"rbModeClient", "rbModeServer"}) {
      window.findChild<QRadioButton *>(role)->click();
      for (auto transport : {"btnInitialBluetooth", "btnInitialNetwork"}) {
        window.findChild<QPushButton *>(transport)->click();
        QCoreApplication::processEvents();
        QCOMPARE(QRect(action->mapTo(content, QPoint()), action->size()), anchor);
        QCOMPARE(local->geometry(), localRect);
        QCOMPARE(local->height(), 112);
        QVERIFY(window.findChild<QWidget *>("backupConnectionDetails")->isHidden());
      }
    }
  }

  void pagesPreserveRoundedWindowCorner()
  {
    MainWindow window;
    window.show();
    for (const auto size : {QSize(740, 600), QSize(1060, 790)}) {
      window.resize(size);
      for (const auto name : {"btnHome", "rbModeClient", "rbModeServer", "btnDevices", "btnLayout", "btnSettings"}) {
        auto button = window.findChild<QAbstractButton *>(name);
        QVERIFY(button);
        button->click();
        QCoreApplication::processEvents();
        const auto image = window.grab().toImage();
        QVERIFY2(image.pixelColor(image.width() - 1, image.height() - 1).alpha() < 32, name);
        const auto inset = qRound(20 * image.devicePixelRatio());
        QVERIFY(image.pixelColor(image.width() - inset, image.height() - inset).alpha() > 240);
      }
    }
  }

  void modeIndicatorSlidesAndReverses()
  {
    MainWindow window;
    window.resize(886, 600);
    window.show();
    QCoreApplication::processEvents();
    auto server = window.findChild<QRadioButton *>("rbModeServer");
    auto client = window.findChild<QRadioButton *>("rbModeClient");
    auto indicator = window.findChild<QWidget *>("modeSelectionIndicator");
    auto animation = window.findChild<QPropertyAnimation *>("modeSelectionAnimation");
    QVERIFY(indicator && animation);
    QCOMPARE(indicator->geometry(), server->geometry());
    client->click();
#ifdef Q_OS_MACOS
    if (prefersOSXReducedMotion()) {
      QCOMPARE(indicator->geometry(), client->geometry());
      return;
    }
#endif
    QCOMPARE(animation->state(), QAbstractAnimation::Running);
    if (animation->state() == QAbstractAnimation::Running) {
      animation->setCurrentTime(animation->duration() / 2);
      QVERIFY(indicator->x() > server->x());
      QVERIFY(indicator->x() < client->x());
      const auto midway = indicator->geometry();
      server->click();
      QCOMPARE(animation->startValue().toRect(), midway);
      animation->setCurrentTime(animation->duration());
    }
    QCOMPARE(indicator->geometry(), server->geometry());
    client->click();
    if (animation->state() == QAbstractAnimation::Running)
      animation->setCurrentTime(animation->duration());
    QCOMPARE(indicator->geometry(), client->geometry());
    window.resize(1060, 790);
    QCoreApplication::processEvents();
    QCOMPARE(indicator->geometry(), client->geometry());
  }

  void modeChangesKeepUpperPageAnchorsStable_data()
  {
    QTest::addColumn<QSize>("size");
    QTest::newRow("narrow") << QSize(740, 600);
    QTest::newRow("laptop") << QSize(886, 600);
    QTest::newRow("desktop") << QSize(1060, 790);
  }

  void modeChangesKeepUpperPageAnchorsStable()
  {
    QFETCH(QSize, size);
    MainWindow window;
    for (int i = 0; i < 4; ++i)
      window.serverConfig().addClient(QString("layout-peer-%1").arg(i));
    window.resize(size);
    window.show();
    auto scroll = window.findChild<QScrollArea *>("homeScroll");
    auto local = window.findChild<QWidget *>("localCard");
    auto selector = window.findChild<QWidget *>("widgetModeSelection");
    auto action = window.findChild<QPushButton *>("btnToggleCore");
    auto card = window.findChild<QWidget *>("homeActionCard");
    auto devicesHeader = window.findChild<QWidget *>("devicesHeader");
    auto server = window.findChild<QRadioButton *>("rbModeServer");
    auto client = window.findChild<QRadioButton *>("rbModeClient");
    client->click();
    QCoreApplication::processEvents();
    const auto viewportWidth = scroll->viewport()->width();
    const auto localRect = local->geometry();
    const auto selectorRect = selector->geometry();
    const QRect actionRect(action->mapTo(scroll->widget(), QPoint()), action->size());
    const auto cardRect = card->geometry();
    const auto devicesRect = devicesHeader->geometry();
    for (int i = 0; i < 12; ++i) {
      (i % 2 ? client : server)->click();
      QCoreApplication::processEvents();
      QCOMPARE(scroll->viewport()->width(), viewportWidth);
      QCOMPARE(local->geometry(), localRect);
      QCOMPARE(selector->geometry(), selectorRect);
      QCOMPARE(QRect(action->mapTo(scroll->widget(), QPoint()), action->size()), actionRect);
      QCOMPARE(card->geometry(), cardRect);
      QCOMPARE(devicesHeader->geometry(), devicesRect);
    }
  }

  void actionCardCrossfadesWithoutBlockingInput()
  {
    MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    auto animation = window.findChild<QPropertyAnimation *>("homeActionTransition");
    auto overlay = window.findChild<QLabel *>("homeActionTransitionOverlay");
    auto client = window.findChild<QRadioButton *>("rbModeClient");
    auto server = window.findChild<QRadioButton *>("rbModeServer");
    auto host = window.findChild<QLineEdit *>("lineHostname");
    QVERIFY(animation && overlay);
    QVERIFY(overlay->testAttribute(Qt::WA_TransparentForMouseEvents));
    client->click();
#ifdef Q_OS_MACOS
    if (prefersOSXReducedMotion()) {
      QVERIFY(overlay->isHidden());
      return;
    }
#endif
    QCOMPARE(animation->state(), QAbstractAnimation::Running);
    animation->setCurrentTime(75);
    QVERIFY(overlay->isVisible());
    host->setText("192.0.2.42");
    QCOMPARE(Settings::value(Settings::Client::RemoteHost).toString(), QString("192.0.2.42"));
    server->click();
    QCOMPARE(animation->state(), QAbstractAnimation::Running);
    animation->setCurrentTime(animation->duration());
    QVERIFY(overlay->isHidden());
    auto shareAddress = window.findChild<QLineEdit *>("lineShareAddress");
    QVERIFY(shareAddress->isReadOnly());
    QCOMPARE(shareAddress->text(), window.findChild<QLabel *>("lblIpAddresses")->text());
  }

  void modeSegmentsAcceptClicksAcrossTheirPaintedArea_data()
  {
    QTest::addColumn<QSize>("size");
    QTest::addColumn<bool>("chinese");
    QTest::newRow("english-laptop") << QSize(886, 600) << false;
    QTest::newRow("chinese-laptop") << QSize(886, 600) << true;
    QTest::newRow("english-wide") << QSize(1060, 790) << false;
    QTest::newRow("chinese-wide") << QSize(1060, 790) << true;
  }

  void modeSegmentsAcceptClicksAcrossTheirPaintedArea()
  {
    QFETCH(QSize, size);
    QFETCH(bool, chinese);
    MainWindow window;
    window.resize(size);
    window.show();
    auto server = window.findChild<QRadioButton *>("rbModeServer");
    auto client = window.findChild<QRadioButton *>("rbModeClient");
    if (chinese) {
      server->setText(QString::fromUtf8("共享本机键鼠"));
      client->setText(QString::fromUtf8("使用其他电脑的键鼠"));
    }
    QCoreApplication::processEvents();
    for (auto target : {server, client}) {
      for (const auto fraction : {0.05, 0.5, 0.8, 0.95}) {
        (target == server ? client : server)->click();
        QTest::mouseClick(
            target, Qt::LeftButton, Qt::NoModifier, QPoint(int(target->width() * fraction), target->height() / 2)
        );
        QVERIFY2(
            target->isChecked(),
            qPrintable(QString("Ignored click at %1 of %2").arg(fraction).arg(target->objectName()))
        );
      }
      for (const int y : {2, target->height() - 3}) {
        (target == server ? client : server)->click();
        QTest::mouseClick(target, Qt::LeftButton, Qt::NoModifier, QPoint(target->width() / 2, y));
        QVERIFY(target->isChecked());
      }
    }
  }

  void homeCardsRetainAccessibleActionsAcrossModeChanges()
  {
    Settings::setValue(Settings::Gui::RecentComputers, QStringList{"mode-test-peer"});
    MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    auto list = window.findChild<QListWidget *>("deviceList");
    auto accessible = QAccessible::queryAccessibleInterface(list);
    QVERIFY(accessible);
    QCOMPARE(accessible->role(), QAccessible::List);
    QVERIFY(!accessible->tableInterface());
    for (int i = 0; i < 20; ++i) {
      window.findChild<QRadioButton *>(i % 2 ? "rbModeServer" : "rbModeClient")->click();
      QCoreApplication::processEvents();
      QCOMPARE(accessible, QAccessible::queryAccessibleInterface(list));
      QVERIFY(list->count() >= 1);
      auto card = qobject_cast<QPushButton *>(list->itemWidget(list->item(0)));
      QVERIFY(card);
      QCOMPARE(card->geometry(), list->visualItemRect(list->item(0)));
      auto cardAccessible = QAccessible::queryAccessibleInterface(card);
      QVERIFY(cardAccessible && cardAccessible->actionInterface());
      QCOMPARE(card->accessibleName(), list->item(0)->text());
    }
    auto card = qobject_cast<QPushButton *>(list->itemWidget(list->item(0)));
    QSignalSpy clicks(list, &QListWidget::itemClicked);
    card->click();
    QCOMPARE(clicks.count(), 1);
    QVERIFY(window.findChild<QPushButton *>("btnDevices")->isChecked());
  }

  void constructingAWindowDoesNotOwnTheProductionInstanceEndpoint()
  {
    MainWindow window;
    auto listener = window.findChild<QLocalServer *>("guiInstanceServer");
    QVERIFY(listener);
    QVERIFY(!listener->isListening());
  }

  void init()
  {
    Settings::setValue(Settings::Connection::InitialTransport, "network");
    Settings::setValue(Settings::Connection::BluetoothPeer, "");
    Settings::setValue(Settings::Connection::AutoBackup, true);
    Settings::setValue(Settings::Gui::AutoStartCore, false);
    Settings::setValue(Settings::Gui::LogExpanded, false);
    Settings::setValue(Settings::Gui::WindowGeometry, QRect(0, 0, 971, 197));
    Settings::setValue(Settings::Security::TlsEnabled, false);
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Server);
    Settings::setValue(Settings::Core::ComputerName, "home-test-computer");
    Settings::setValue(Settings::Client::RemoteHost, "");
    Settings::setValue(Settings::Client::AutoConnect, false);
    Settings::setValue(Settings::Core::Interface, "");
    Settings::setValue(Settings::Gui::RecentComputers, QStringList{});
    Settings::setValue("internalConfig/screens/size", 0);
  }

  void switchesModesAndRequiresAnAddress()
  {
    MainWindow window;
    auto server = window.findChild<QRadioButton *>("rbModeServer");
    auto client = window.findChild<QRadioButton *>("rbModeClient");
    auto action = window.findChild<QPushButton *>("btnToggleCore");
    auto host = window.findChild<QLineEdit *>("lineHostname");
    QVERIFY(server && client && action && host);
    QCOMPARE(action->isEnabled(), inputControlAllowed());
    QVERIFY(!window.findChild<QWidget *>("serverOptions")->isHidden());
    client->click();
    QCOMPARE(window.coreMode(), Settings::CoreMode::Client);
    QVERIFY(window.findChild<QWidget *>("serverOptions")->isHidden());
    QVERIFY(!window.findChild<QWidget *>("clientOptions")->isHidden());
    QVERIFY(!action->isEnabled());
    host->setText("192.0.2.42");
    QCOMPARE(action->isEnabled(), inputControlAllowed());
    QCOMPARE(Settings::value(Settings::Client::RemoteHost).toString(), QString("192.0.2.42"));
    auto devices = window.findChild<QListWidget *>("deviceList");
    QCOMPARE(devices->count(), 1);
    QVERIFY(devices->item(0)->text().contains("192.0.2.42"));
    QVERIFY(devices->item(0)->text().contains("Not connected"));
    host->clear();
    QVERIFY(!action->isEnabled());
    server->click();
    QCOMPARE(action->isEnabled(), inputControlAllowed());
    QCOMPARE(window.coreMode(), Settings::CoreMode::Server);
  }

  void restoresAResizableHomepageAndLogs()
  {
    MainWindow window;
    QVERIFY(window.height() >= 600);
    QVERIFY(window.width() >= 740);
    QVERIFY(window.maximumWidth() > window.minimumWidth());
    QVERIFY(window.maximumHeight() > window.minimumHeight());
    window.show();
    QCoreApplication::processEvents();
    auto logs = window.findChild<QPushButton *>("btnLogs");
    auto dock = window.findChild<LogDock *>();
    QVERIFY(logs && dock);
    logs->click();
    QVERIFY(dock->isVisible());
    logs->click();
    QVERIFY(!dock->isVisible());
    QVERIFY(window.maximumWidth() > window.minimumWidth());
    window.resize(880, 650);
    QCoreApplication::processEvents();
    QCOMPARE(window.width(), 880);
  }

  void showsIdentityWithoutInventingAConnection()
  {
    MainWindow window;
    QCOMPARE(window.findChild<QLabel *>("lblComputerName")->toolTip(), QString("home-test-computer"));
    QCOMPARE(window.findChild<QLabel *>("lblComputerName")->accessibleName(), QString("home-test-computer"));
    QCOMPARE(window.findChild<QLabel *>("lblSidebarName")->text(), QString("home-test-computer"));
    QCOMPARE(window.findChild<QLabel *>("lblSidebarStatus")->text(), QString("Not running"));
    QCOMPARE(window.findChild<QPushButton *>("btnSecurity")->text(), QString("Encryption disabled"));
    auto address = window.findChild<QLabel *>("lblIpAddresses");
    QVERIFY(!address->isHidden());
    QVERIFY(!address->text().isEmpty());
  }

  void missingPermissionAllowsHomeButBlocksInputService()
  {
#ifdef Q_OS_MACOS
    if (inputControlAllowed())
      QSKIP("This test process already has input-control permission.");
    MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    QVERIFY(window.isVisible());
    QVERIFY(!window.findChild<QWidget *>("permissionBanner")->isHidden());
    QVERIFY(!window.findChild<QPushButton *>("btnToggleCore")->isEnabled());
    window.findChild<QPushButton *>("btnPermissionRecheck")->click();
    QVERIFY(window.isVisible());

    deskflow::gui::CoreProcess core(window.serverConfig());
    core.setMode(Settings::CoreMode::Server);
    bool refused = false;
    connect(&core, &deskflow::gui::CoreProcess::error, this, [&refused](auto error) {
      refused = error == deskflow::gui::CoreProcess::Error::AccessibilityRequired;
    });
    QSignalSpy states(&core, &deskflow::gui::CoreProcess::processStateChanged);
    core.start();
    QVERIFY(refused);
    QCOMPARE(core.processState(), deskflow::core::ProcessState::Stopped);
    QCOMPARE(states.count(), 0);
#else
    QSKIP("macOS permission check");
#endif
  }

  void configuredDevicesAreNotReportedAsConnected()
  {
    MainWindow window;
    window.serverConfig().addClient("home-test-peer");
    Settings::instance()->serverSettingsChanged();
    auto devices = window.findChild<QListWidget *>("deviceList");
    bool found = false;
    for (int i = 0; i < devices->count(); ++i) {
      if (devices->item(i)->text().contains("home-test-peer")) {
        found = true;
        QVERIFY(devices->item(i)->text().contains("Not connected"));
      }
    }
    QVERIFY(found);
  }

  void retranslationPreservesIdentityAndIcons()
  {
    MainWindow window;
    auto avatar = window.findChild<QLabel *>("lblAvatar");
    QVERIFY(!avatar->pixmap().isNull());
    const auto version = window.findChild<QLabel *>("lblVersion")->text();
    QEvent event(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&window, &event);
    QCOMPARE(window.findChild<QLabel *>("lblComputerName")->toolTip(), QString("home-test-computer"));
    QCOMPARE(window.findChild<QLabel *>("lblVersion")->text(), version);
    QVERIFY(!avatar->pixmap().isNull());
    QVERIFY(!window.findChild<QLabel *>("lblLocalPlatform")->text().isEmpty());
  }

  void layoutsFitBothModes_data()
  {
    QTest::addColumn<QSize>("size");
    QTest::newRow("desktop") << QSize(1060, 790);
    QTest::newRow("minimum") << QSize(820, 600);
    QTest::newRow("prototype-width") << QSize(747, 840);
  }

  void layoutsFitBothModes()
  {
    QFETCH(QSize, size);
    MainWindow window;
    window.resize(size);
    window.show();
    QCoreApplication::processEvents();
    auto scroll = window.findChild<QScrollArea *>("homeScroll");
    QVERIFY(scroll);
    QCOMPARE(scroll->widget()->width(), scroll->viewport()->width());
    window.findChild<QRadioButton *>("rbModeClient")->click();
    QCoreApplication::processEvents();
    QCOMPARE(scroll->widget()->width(), scroll->viewport()->width());
    QVERIFY(window.findChild<QLineEdit *>("lineHostname")->width() >= 120);
  }

  void devicesUseTwoCardsAtPrototypeWidth()
  {
    Settings::setValue(Settings::Gui::RecentComputers, QStringList{"desktop-peer", "laptop-peer"});
    MainWindow window;
    window.resize(747, 840);
    window.show();
    QCoreApplication::processEvents();
    auto list = window.findChild<QListWidget *>("deviceList");
    QVERIFY(list->count() >= 2);
    const auto first = list->visualItemRect(list->item(0));
    const auto second = list->visualItemRect(list->item(1));
    QCOMPARE(first.top(), second.top());
    QVERIFY(second.left() >= first.right());
    QVERIFY(second.right() <= list->viewport()->rect().right());
    QVERIFY(first.height() >= 70);
    QVERIFY(list->item(0)->data(HomeDeviceList::Name).isValid());
    QVERIFY(!list->item(0)->data(HomeDeviceList::Connected).toBool());
  }

  void primaryActionFitsFirstScreenWithLongIdentity_data()
  {
    QTest::addColumn<QSize>("size");
    QTest::newRow("narrow") << QSize(740, 600);
    QTest::newRow("laptop") << QSize(886, 600);
    QTest::newRow("desktop") << QSize(1060, 790);
  }

  void primaryActionFitsFirstScreenWithLongIdentity()
  {
    QFETCH(QSize, size);
    const QString name = "very-long-computer-name-for-testing-the-homepage-layout.local";
    Settings::setValue(Settings::Core::ComputerName, name);
    MainWindow window;
    window.resize(size);
    window.show();
    auto scroll = window.findChild<QScrollArea *>("homeScroll");
    auto action = window.findChild<QPushButton *>("btnToggleCore");
    auto input = window.findChild<QLineEdit *>("lineHostname");
    auto identity = window.findChild<QLabel *>("lblComputerName");
    auto localCard = window.findChild<QWidget *>("localCard");
    for (const auto mode : {"rbModeServer", "rbModeClient"}) {
      window.findChild<QRadioButton *>(mode)->click();
      window.findChild<QWidget *>("permissionBanner")->show();
      QCoreApplication::processEvents();
      const QRect buttonRect(action->mapTo(scroll->viewport(), QPoint()), action->size());
      QVERIFY2(scroll->viewport()->rect().contains(buttonRect), "Primary action must fit without scrolling");
      QVERIFY(localCard->height() <= 160);
      QCOMPARE(identity->toolTip(), name);
      QVERIFY(identity->text().endsWith(QChar(0x2026)));
      QVERIFY(identity->fontMetrics().horizontalAdvance(identity->text()) <= identity->width());
      if (input->isVisible()) {
        const QRect inputRect(input->mapTo(scroll->viewport(), QPoint()), input->size());
        QCOMPARE(buttonRect.top(), inputRect.top());
        QCOMPARE(buttonRect.bottom(), inputRect.bottom());
      }
    }
    QCOMPARE(window.findChild<QLabel *>("lblBrand")->text(), QString("BMouse"));
    auto empty = window.findChild<QWidget *>("homeEmptyState");
    QVERIFY(empty->isVisible());
    QCOMPARE(empty->width(), localCard->width());
    auto emptyAction = window.findChild<QPushButton *>("btnEmptyAction");
    emptyAction->click();
    QVERIFY(window.findChild<QPushButton *>("btnDevices")->isChecked());
  }

  void renderEvidence()
  {
    const auto output = qEnvironmentVariable("DESKFLOW_HOME_EVIDENCE");
    if (output.isEmpty())
      QSKIP("Set DESKFLOW_HOME_EVIDENCE to capture rendered homepage previews.");
    QTranslator chinese;
    QVERIFY(chinese.load(qEnvironmentVariable("DESKFLOW_HOME_TRANSLATION")));
    qApp->installTranslator(&chinese);
    QTemporaryDir fixture;
    Settings::setSettingsFile(fixture.path() + "/Deskflow.conf");
    init();
    Settings::setValue(Settings::Core::ComputerName, QString::fromUtf8("我的 Mac mini"));
    Settings::setValue(Settings::Gui::RecentComputers, QStringList{QString::fromUtf8("工作电脑"), "MacBook Pro"});
    {
      MainWindow window;
      window.show();
      window.menuBar()->hide();
      window.resize(1060, 790);
      QCoreApplication::processEvents();
      // Visual fixture for the normal, permission-complete layout. Input services remain stopped.
      window.findChild<QWidget *>("permissionBanner")->hide();
      window.findChild<QPushButton *>("btnToggleCore")->setEnabled(true);
      QCoreApplication::processEvents();
      QVERIFY(window.grab().save(output + "/home-sharing.png"));
      window.resize(747, 840);
      QCoreApplication::processEvents();
      window.findChild<QWidget *>("permissionBanner")->hide();
      window.findChild<QPushButton *>("btnToggleCore")->setEnabled(true);
      auto cards = window.findChild<QListWidget *>("deviceList");
      QCOMPARE(cards->count(), 2);
      QVERIFY(cards->visualItemRect(cards->item(1)).right() <= cards->viewport()->rect().right());
      QCoreApplication::processEvents();
      QVERIFY(window.grab().save(output + "/home-prototype-width.png"));
      window.findChild<QRadioButton *>("rbModeClient")->click();
      window.findChild<QLineEdit *>("lineHostname")->setText("192.0.2.42");
      QCoreApplication::processEvents();
      window.findChild<QWidget *>("permissionBanner")->hide();
      QCoreApplication::processEvents();
      auto slide = window.findChild<QPropertyAnimation *>("modeSelectionAnimation");
      if (slide->state() == QAbstractAnimation::Running) {
        slide->setCurrentTime(45);
        QVERIFY(window.grab().save(output + "/home-mode-slide-midpoint.png"));
        slide->setCurrentTime(slide->duration());
      }
      auto crossfade = window.findChild<QPropertyAnimation *>("homeActionTransition");
      if (crossfade->state() == QAbstractAnimation::Running)
        crossfade->setCurrentTime(crossfade->duration());
      QVERIFY(window.grab().save(output + "/home-receiving.png"));
      window.resize(820, 600);
      QCoreApplication::processEvents();
      window.findChild<QWidget *>("permissionBanner")->hide();
      QCoreApplication::processEvents();
      QVERIFY(window.grab().save(output + "/home-minimum.png"));
    }
    qApp->removeTranslator(&chinese);
  }
};

int main(int argc, char **argv)
{
  // Use disposable settings and state before the Settings singleton is first accessed.
  QTemporaryDir profile;
  qputenv("XDG_CONFIG_HOME", profile.path().toUtf8());
  qputenv("XDG_STATE_HOME", profile.path().toUtf8());
  QApplication app(argc, argv);
  MainWindowHomeTests tests;
  return QTest::qExec(&tests, argc, argv);
}

#include "MainWindowHomeTests.moc"
