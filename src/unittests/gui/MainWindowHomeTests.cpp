/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "gui/MainWindow.h"
#include "gui/core/BackupConnectionPanel.h"
#include "gui/core/NearbyBluetoothModel.h"
#include "gui/widgets/DiscoveryComboBox.h"
#include "gui/widgets/LogDock.h"
#include "gui/widgets/HomeCheckBox.h"

#include <QAction>
#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocalServer>
#include <QMenuBar>
#include <QMenu>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopeGuard>
#include <QStringListModel>
#include <QTemporaryDir>
#include <QTranslator>
#include <QVBoxLayout>
#include <QtTest>
#include <algorithm>

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
  void checkboxesRemainLegibleAndInteractiveWithADarkPalette()
  {
    HomeCheckBox check("Connection method");
    QPalette dark;
    dark.setColor(QPalette::Window, Qt::black);
    dark.setColor(QPalette::Base, Qt::black);
    dark.setColor(QPalette::Button, Qt::black);
    dark.setColor(QPalette::Text, Qt::white);
    dark.setColor(QPalette::ButtonText, Qt::white);
    check.setPalette(dark);
    check.resize(check.sizeHint());
    check.show();
    check.setFocus();
    QCoreApplication::processEvents();
    QTest::keyClick(&check, Qt::Key_Space);
    QVERIFY(check.isChecked());
    const auto selected = check.grab().toImage();
    int bluePixels = 0;
    int whitePixels = 0;
    const auto scale = selected.devicePixelRatio();
    for (int y = int((check.height() / 2 - 6) * scale); y < int((check.height() / 2 + 6) * scale); ++y) {
      for (int x = int(5 * scale); x < int(17 * scale); ++x) {
        const auto color = selected.pixelColor(x, y);
        bluePixels += color.blue() > 180 && color.red() < 80;
        whitePixels += color.red() > 230 && color.green() > 230 && color.blue() > 230;
      }
    }
    QVERIFY2(bluePixels > 25, "Selected checkbox must have a blue indicator even under a dark desktop palette");
    QVERIFY2(whitePixels > 3, "Selected checkbox must have a visible white check mark");
    auto accessible = QAccessible::queryAccessibleInterface(&check);
    QVERIFY(accessible);
    QCOMPARE(accessible->role(), QAccessible::CheckBox);
    QVERIFY(accessible->state().checked);
    check.setEnabled(false);
    QTest::keyClick(&check, Qt::Key_Space);
    QVERIFY(check.isChecked());
    QVERIFY(check.grab().toImage() != selected);
    check.setEnabled(true);
    QTest::mouseClick(&check, Qt::LeftButton, Qt::NoModifier, QPoint(check.width() - 3, check.height() / 2));
    QVERIFY(!check.isChecked());
    const auto unchecked = check.grab().toImage();
    QVERIFY(unchecked.pixelColor(int(9 * scale), int(check.height() / 2 * scale)).lightness() > 230);
  }

  void lightPagesDoNotInheritDarkNativeControlColors()
  {
    const auto original = qApp->palette();
    const auto restore = qScopeGuard([original] { qApp->setPalette(original); });
    auto dark = original;
    dark.setColor(QPalette::Window, QColor("#202020"));
    dark.setColor(QPalette::Base, QColor("#202020"));
    dark.setColor(QPalette::Button, QColor("#303030"));
    dark.setColor(QPalette::Text, Qt::white);
    dark.setColor(QPalette::ButtonText, Qt::white);
    qApp->setPalette(dark);
    MainWindow window;
    window.show();
    window.findChild<QPushButton *>("btnSettings")->click();
    QCoreApplication::processEvents();
    for (auto combo : window.findChildren<QComboBox *>()) {
      QVERIFY(combo->palette().color(QPalette::Active, QPalette::Button).lightness() > 200);
      QVERIFY(combo->palette().color(QPalette::Active, QPalette::ButtonText).lightness() < 150);
      const auto background = combo->view()->palette().color(QPalette::Active, QPalette::Base);
      QVERIFY2(background.lightness() > 200, qPrintable(combo->objectName()));
      QVERIFY2(
          combo->view()->window()->palette().color(QPalette::Active, QPalette::Window).lightness() > 200,
          qPrintable(combo->objectName())
      );
    }
    const auto more = window.findChild<QPushButton *>("btnMore")->menu();
    QVERIFY(more->palette().color(QPalette::Window).lightness() > 200);
    const auto evidence = qEnvironmentVariable("DESKFLOW_CONTROLS_EVIDENCE");
    if (!evidence.isEmpty()) {
      QVERIFY(window.grab().save(evidence + "/settings-dark-desktop.png"));
      window.findChild<QPushButton *>("btnHome")->click();
      QCoreApplication::processEvents();
      QVERIFY(window.grab().save(evidence + "/sharing-checkboxes.png"));
      window.findChild<QRadioButton *>("rbModeClient")->click();
      for (const auto name : {"modeSelectionAnimation", "homeActionTransition"}) {
        if (auto animation = window.findChild<QPropertyAnimation *>(name))
          animation->setCurrentTime(animation->duration());
      }
      QCoreApplication::processEvents();
      QVERIFY(window.grab().save(evidence + "/backup-checkbox.png"));
    }
  }

  void bluetoothPopupAcceptsLiveDiscoveryResults()
  {
    NearbyBluetoothModel devices;
    DiscoveryComboBox picker;
    picker.setModel(&devices);
    picker.resize(360, 44);
    picker.show();
    picker.showPopup();
    QCoreApplication::processEvents();
    auto *motion = picker.findChild<QParallelAnimationGroup *>("bluetoothPopupAnimation");
    QVERIFY(motion);
    if (motion->state() == QAbstractAnimation::Running)
      motion->setCurrentTime(motion->duration());
    const auto initialHeight = picker.view()->height();
    devices.addDevice("a", "AA:00:00:00:00:01", "Computer", true);
    devices.addDevice("b", "AA:00:00:00:00:02", "Phone", true);
    devices.addDevice("c", "AA:00:00:00:00:03", "Headphones", true);
    devices.sort(0);
    QCoreApplication::processEvents();
    QVERIFY(picker.view()->isVisible());
    QVERIFY(picker.view()->height() > initialHeight);
    QCOMPARE(picker.view()->model()->rowCount(), 4);
    auto *accessible = QAccessible::queryAccessibleInterface(picker.view());
    QVERIFY(accessible && !accessible->tableInterface());
    auto *rowButton = qobject_cast<QPushButton *>(picker.view()->indexWidget(devices.index(1, 0)));
    QVERIFY(rowButton);
    auto *rowAccessible = QAccessible::queryAccessibleInterface(rowButton);
    QVERIFY(rowAccessible && rowAccessible->actionInterface());
    QCOMPARE(rowButton->accessibleName(), devices.index(1, 0).data().toString());
    rowButton->click();
    QCOMPARE(picker.currentIndex(), 1);
    QTRY_VERIFY(!picker.view()->isVisible());
    picker.hidePopup();
  }

  void localInformationAndModeSwitchShareCentreline()
  {
    MainWindow window;
    window.show();
    for (const auto size : {QSize(740, 600), QSize(886, 600), QSize(1060, 790)}) {
      window.resize(size);
      QCoreApplication::processEvents();
      auto *divider = window.findChild<QWidget *>("cardDivider");
      auto *left = window.findChild<QRadioButton *>("rbModeServer");
      auto *right = window.findChild<QRadioButton *>("rbModeClient");
      const auto divideX = divider->mapTo(&window, divider->rect().center()).x();
      const auto leftEnd = left->mapTo(&window, QPoint(left->width(), 0)).x();
      const auto rightStart = right->mapTo(&window, QPoint()).x();
      QVERIFY2(qAbs(divideX - (leftEnd + rightStart) / 2) <= 2, "Top divider must align with mode split");
      QVERIFY(qAbs(left->width() - right->width()) <= 1);
    }
  }

  void bluetoothListIncludesDevicesWithoutBMouseAndDeduplicates()
  {
    NearbyBluetoothModel devices;
    devices.beginScan();
    devices.addDevice("aa", "AA:00:00:00:00:01", "Work computer", true);
    devices.addDevice("bb", "AA:00:00:00:00:02", "Headphones", true);
    devices.addDevice("ble-uuid", {}, "Low energy sensor", false);
    devices.addDevice("bb", "AA:00:00:00:00:02", "Renamed headphones", true);
    QCOMPARE(devices.rowCount(), 4);
    QCOMPARE(devices.foundCount(), 3);
    devices.finishScan();
    devices.sort(0);
    QCOMPARE(devices.connectableAddress(devices.rowForIdentity("aa")), QString("AA:00:00:00:00:01"));
    QCOMPARE(devices.connectableAddress(devices.rowForIdentity("bb")), QString("AA:00:00:00:00:02"));
    QVERIFY(devices.connectableAddress(devices.rowForIdentity("ble-uuid")).isEmpty());
    QVERIFY(devices.item(devices.rowForIdentity("bb"))->text().contains("Renamed headphones"));
    QCOMPARE(devices.foundCount(), 3);
    QCOMPARE(devices.item(devices.rowForIdentity("aa"))->text(), QString("Work computer"));
  }
  void bluetoothRescanInvalidatesStaleAddressesWithoutCheckingServices()
  {
    NearbyBluetoothModel devices;
    devices.resetDevices("AA:00:00:00:00:01");
    QVERIFY(devices.connectableAddress(1).isEmpty());
    devices.beginScan();
    devices.addDevice("aa", "AA:00:00:00:00:02", "BMouse in the name is not proof", true);
    QCOMPARE(devices.connectableAddress(devices.rowForIdentity("aa")), QString("AA:00:00:00:00:02"));
    devices.finishScan();
    QCOMPARE(devices.rowCount(), 2); // Old saved device was not found in this scan.
    QVERIFY(!devices.connectableAddress(1).isEmpty());
    devices.beginScan();
    QVERIFY(devices.connectableAddress(1).isEmpty());
  }
  void choosingBluetoothDevicesOnlyChangesTheTarget()
  {
    MainWindow window;
    window.show();
    window.findChild<QRadioButton *>("rbModeClient")->click();
    window.findChild<QPushButton *>("btnInitialBluetooth")->click();
    auto panel = window.findChild<BackupConnectionPanel *>();
    auto picker = window.findChild<QComboBox *>("bluetoothTarget");
    auto devices = static_cast<NearbyBluetoothModel *>(picker->model());
    devices->addDevice("aa", "AA:00:00:00:00:01", "Computer", true);
    QSignalSpy commands(panel, &BackupConnectionPanel::command);
    picker->setCurrentIndex(devices->rowForIdentity("aa"));
    QVERIFY(panel->bluetoothTargetReady());
    QCOMPARE(Settings::value(Settings::Connection::BluetoothPeer).toString(), QString("AA:00:00:00:00:01"));
    devices->addDevice("bb", "AA:00:00:00:00:02", "Headphones", true);
    picker->setCurrentIndex(devices->rowForIdentity("bb"));
    QCOMPARE(Settings::value(Settings::Connection::BluetoothPeer).toString(), QString("AA:00:00:00:00:02"));
    QVERIFY(panel->bluetoothTargetReady());
    QCOMPARE(commands.count(), 0); // Listing and selection must not start any connection or pairing.
    devices->addDevice("ble", {}, "Low energy device", false);
    picker->setCurrentIndex(devices->rowForIdentity("ble"));
    QVERIFY(!panel->bluetoothTargetReady());
    QVERIFY(Settings::value(Settings::Connection::BluetoothPeer).toString().isEmpty());
    QVERIFY(!window.findChild<QPushButton *>("btnToggleCore")->isEnabled());
  }
  void transportInputsHaveIdenticalVisibleBounds_data()
  {
    QTest::addColumn<QSize>("size");
    QTest::newRow("minimum") << QSize(740, 600);
    QTest::newRow("laptop") << QSize(886, 600);
    QTest::newRow("desktop") << QSize(1060, 790);
  }
  void transportInputsHaveIdenticalVisibleBounds()
  {
    QFETCH(QSize, size);
    MainWindow window;
    window.resize(size);
    window.show();
    window.findChild<QRadioButton *>("rbModeClient")->click();
    auto transition = window.findChild<QPropertyAnimation *>("homeActionTransition");
    if (transition->state() == QAbstractAnimation::Running)
      transition->setCurrentTime(transition->duration());
    QCoreApplication::processEvents();
    auto input = window.findChild<QLineEdit *>("lineHostname");
    const QRect anchor(input->mapTo(&window, QPoint()), input->size());
    for (int repeat = 0; repeat < 4; ++repeat) {
      for (auto name : {"btnInitialBluetooth", "btnInitialNetwork"}) {
        auto *transport = window.findChild<QPushButton *>(name);
        if (repeat % 2)
          transport->setChecked(true); // Native accessibility actions change the checked state.
        else
          transport->click();
        QCoreApplication::processEvents();
        QWidget *field = QString(name) == "btnInitialBluetooth"
                             ? static_cast<QWidget *>(window.findChild<QComboBox *>("bluetoothTarget"))
                             : input;
        QCOMPARE(QRect(field->mapTo(&window, QPoint()), field->size()), anchor);
        QCOMPARE(field->height(), 44);
        auto image = window.grab().toImage();
        auto fieldImage = field->grab().toImage();
        const auto point = QPoint(field->width() / 2, field->height() - 1);
        const auto mapped = field->mapTo(&window, point);
        const auto dpr = image.devicePixelRatio();
        const auto actual = image.pixelColor(qRound(mapped.x() * dpr), qRound(mapped.y() * dpr));
        const auto expected = fieldImage.pixelColor(qRound(point.x() * dpr), qRound(point.y() * dpr));
        QCOMPARE(actual, expected); // Bottom border must be visible in its parent, not clipped.
      }
    }
  }

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
    auto footer = window.findChild<QWidget *>("homeFooter");
    auto server = window.findChild<QRadioButton *>("rbModeServer");
    auto client = window.findChild<QRadioButton *>("rbModeClient");
    client->click();
    QCoreApplication::processEvents();
    const auto viewportWidth = scroll->viewport()->width();
    const auto localRect = local->geometry();
    const auto selectorRect = selector->geometry();
    const QRect actionRect(action->mapTo(scroll->widget(), QPoint()), action->size());
    const auto cardRect = card->geometry();
    const auto footerRect = footer->geometry();
    for (int i = 0; i < 12; ++i) {
      (i % 2 ? client : server)->click();
      QCoreApplication::processEvents();
      QCOMPARE(scroll->viewport()->width(), viewportWidth);
      if (local->geometry() != localRect)
        for (auto *child : window.findChildren<QWidget *>())
          if (child->isVisible() && child->minimumSizeHint().width() > 350)
            qInfo() << "CHANGED" << child->objectName() << child->minimumSizeHint() << child->minimumSize()
                    << child->size();
      if (local->geometry() != localRect) {
        auto layout = window.findChild<QVBoxLayout *>("contentLayout");
        for (int item = 0; item < layout->count(); ++item)
          qInfo() << "ITEM"
                  << (layout->itemAt(item)->widget() ? layout->itemAt(item)->widget()->objectName() : "spacer")
                  << layout->itemAt(item)->minimumSize() << layout->itemAt(item)->sizeHint();
      }
      QCOMPARE(local->geometry(), localRect);
      QCOMPARE(selector->geometry(), selectorRect);
      QCOMPARE(QRect(action->mapTo(scroll->widget(), QPoint()), action->size()), actionRect);
      QCOMPARE(card->geometry(), cardRect);
      QCOMPARE(footer->geometry(), footerRect);
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

  void sharingTransportsAreIndependentOfReceiverPolicy()
  {
    MainWindow window;
    window.show();
    auto network = window.findChild<QCheckBox *>("checkShareNetwork");
    auto bluetooth = window.findChild<QCheckBox *>("checkShareBluetooth");
    QVERIFY(network->isChecked() && bluetooth->isChecked());
    network->click();
    QVERIFY(!Settings::value(Settings::Connection::ShareNetwork).toBool());
    QVERIFY(Settings::value(Settings::Connection::ShareBluetooth).toBool());
    bluetooth->click();
    QVERIFY(bluetooth->isChecked()); // At least one transport must remain selected.
    QVERIFY(Settings::value(Settings::Connection::AutoBackup).toBool());
    window.findChild<QRadioButton *>("rbModeClient")->click();
    QVERIFY(window.findChild<QWidget *>("sharingTransportSelector")->isHidden());
    QVERIFY(window.findChild<QWidget *>("initialTransportSelector")->isVisible());
    auto automatic = window.findChild<QCheckBox *>("checkAutoBackup");
    QVERIFY(automatic->isVisible());
    automatic->click();
    QVERIFY(!Settings::value(Settings::Connection::AutoBackup).toBool());
    QVERIFY(Settings::value(Settings::Connection::ShareBluetooth).toBool());
    window.findChild<QRadioButton *>("rbModeServer")->click();
    QVERIFY(window.findChild<QWidget *>("sharingTransportSelector")->isVisible());
    QVERIFY(!automatic->isVisible());
    QVERIFY(!network->isChecked() && bluetooth->isChecked());
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
    Settings::setValue(Settings::Connection::ShareNetwork, true);
    Settings::setValue(Settings::Connection::ShareBluetooth, true);
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
    QVERIFY(!window.findChild<QWidget *>("sharingTransportSelector")->isHidden());
    client->click();
    QCOMPARE(window.coreMode(), Settings::CoreMode::Client);
    QVERIFY(window.findChild<QWidget *>("serverOptions")->isHidden());
    QVERIFY(!window.findChild<QWidget *>("clientOptions")->isHidden());
    QVERIFY(!action->isEnabled());
    host->setText("192.0.2.42");
    QCOMPARE(action->isEnabled(), inputControlAllowed());
    QCOMPARE(Settings::value(Settings::Client::RemoteHost).toString(), QString("192.0.2.42"));
    host->clear();
    QVERIFY(!action->isEnabled());
    server->click();
    QCOMPARE(action->isEnabled(), inputControlAllowed());
    QCOMPARE(window.coreMode(), Settings::CoreMode::Server);
  }

  void runningConnectionDoesNotLockTheModeSelector_data()
  {
    QTest::addColumn<deskflow::core::ProcessState>("reportedState");
    QTest::newRow("running") << deskflow::core::ProcessState::Started;
    QTest::newRow("retrying") << deskflow::core::ProcessState::RetryPending;
  }

  void runningConnectionDoesNotLockTheModeSelector()
  {
    QFETCH(deskflow::core::ProcessState, reportedState);
    Settings::setValue(Settings::Gui::ShownServerFirstStartMessage, true);
    MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    auto core = window.findChild<deskflow::gui::CoreProcess *>();
    auto server = window.findChild<QRadioButton *>("rbModeServer");
    auto client = window.findChild<QRadioButton *>("rbModeClient");
    QVERIFY(core && server && client);
    QTest::mouseClick(client, Qt::LeftButton);
    // Exercise the UI response to the same notifications as a live/retrying core,
    // without starting input capture or connecting to another computer in a unit test.
    core->processStateChanged(reportedState);
    QVERIFY(server->isEnabled());
    QVERIFY(client->isEnabled());
    QTest::mouseClick(server, Qt::LeftButton);
    QCOMPARE(window.coreMode(), Settings::CoreMode::Server);
    QCOMPARE(Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>(), Settings::CoreMode::Server);
    QVERIFY(window.findChild<QWidget *>("sharingTransportSelector")->isVisible());
    QVERIFY(window.findChild<QWidget *>("initialTransportSelector")->isHidden());
    QCOMPARE(window.findChild<QPushButton *>("btnToggleCore")->text(), QString("Start sharing"));
    core->processStateChanged(reportedState);
    QTest::mouseClick(client, Qt::LeftButton);
    QCOMPARE(window.coreMode(), Settings::CoreMode::Client);
    core->processStateChanged(deskflow::core::ProcessState::Stopped);
  }

  void restoresAResizableHomepageAndLogs()
  {
    // An old profile must not restore the legacy expanded diagnostics layout.
    Settings::setValue(Settings::Gui::LogExpanded, true);
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
    QVERIFY(dock->isHidden());
    const auto initialSize = window.size();
    QTest::mouseClick(logs, Qt::LeftButton);
    QCoreApplication::processEvents();
    QVERIFY(dock->isVisible());
    QCOMPARE(window.size(), initialSize);
    QVERIFY(window.centralWidget()->isAncestorOf(dock));
    const QRect logRect(dock->mapTo(window.centralWidget(), QPoint()), dock->size());
    QVERIFY(window.centralWidget()->rect().contains(logRect));
    QVERIFY(dock->toggleViewAction()->isChecked());
    const auto evidence = qEnvironmentVariable("DESKFLOW_LOG_EVIDENCE");
    if (!evidence.isEmpty()) {
      dock->appendLine("INFO: Diagnostic log stays inside the application window.");
      QVERIFY(window.grab().save(evidence));
    }
    window.hide();
    QCoreApplication::processEvents();
    QVERIFY(dock->toggleViewAction()->isChecked());
    window.show();
    QCoreApplication::processEvents();
    QVERIFY(dock->isVisible());
    logs->click();
    QVERIFY(!dock->isVisible());
    QVERIFY(!dock->toggleViewAction()->isChecked());
    dock->toggleViewAction()->trigger();
    QVERIFY(dock->isVisible());
    auto close = dock->findChild<QPushButton *>("btnCloseLog");
    QVERIFY(close);
    close->click();
    QVERIFY(dock->isHidden());
    QVERIFY(!dock->toggleViewAction()->isChecked());
    QVERIFY(window.maximumWidth() > window.minimumWidth());
    window.resize(880, 650);
    QCoreApplication::processEvents();
    QCOMPARE(window.width(), 880);
  }

  void menusRespectThePlatformAndShortcutsRemainAvailable()
  {
    MainWindow window;
#ifdef Q_OS_MACOS
    QVERIFY(window.findChild<QMenuBar *>());
#else
    QVERIFY(!window.findChild<QMenuBar *>());
#endif
    const auto actions = window.actions();
    QVERIFY(std::any_of(actions.begin(), actions.end(), [](QAction *action) {
      return action->shortcut() == QKeySequence(QKeySequence::HelpContents);
    }));
    auto more = window.findChild<QPushButton *>("btnMore");
    auto log = window.findChild<LogDock *>();
    QVERIFY(more && more->menu() && log);
    QVERIFY(more->menu()->actions().contains(log->toggleViewAction()));
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

  void recentServersAreAvailableWithoutADeviceSection()
  {
    Settings::setValue(Settings::Gui::KnownServers, QVariantMap{{"192.0.2.42", "Work computer"}});
    Settings::setValue(Settings::Gui::RecentServers, QStringList{"192.0.2.43"});
    MainWindow window;
    window.show();
    window.findChild<QRadioButton *>("rbModeClient")->click();
    auto host = window.findChild<QLineEdit *>("lineHostname");
    auto completion = host->completer();
    QVERIFY(completion);
    auto addresses = qobject_cast<QStringListModel *>(completion->model())->stringList();
    QVERIFY(addresses.contains("192.0.2.42"));
    QVERIFY(addresses.contains("192.0.2.43"));
    QVERIFY(!window.findChild<QWidget *>("deviceList"));
    QVERIFY(!window.findChild<QWidget *>("homeEmptyState"));
    window.findChild<QPushButton *>("btnDevices")->click();
    QVERIFY(window.findChild<QPushButton *>("btnDevices")->isChecked());
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

  void normalHomepageFitsOneScreen_data()
  {
    QTest::addColumn<QSize>("size");
    QTest::newRow("minimum") << QSize(740, 600);
    QTest::newRow("laptop") << QSize(886, 600);
    QTest::newRow("desktop") << QSize(1060, 790);
  }
  void normalHomepageFitsOneScreen()
  {
    QFETCH(QSize, size);
    MainWindow window;
    window.resize(size);
    window.show();
    auto scroll = window.findChild<QScrollArea *>("homeScroll");
    for (auto role : {"rbModeServer", "rbModeClient"}) {
      window.findChild<QRadioButton *>(role)->click();
      window.findChild<QWidget *>("permissionBanner")->hide();
      QCoreApplication::processEvents();
      QCOMPARE(scroll->verticalScrollBar()->maximum(), 0);
      auto footer = window.findChild<QWidget *>("homeFooter");
      QVERIFY(scroll->viewport()->rect().contains(QRect(footer->mapTo(scroll->viewport(), QPoint()), footer->size())));
      QVERIFY(window.findChild<QLabel *>("lblShareHint")->isHidden());
      QVERIFY(window.findChild<QLabel *>("lblActionHint")->isHidden());
    }
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
      window.resize(1060, 790);
      QCoreApplication::processEvents();
      // Visual fixture for the normal, permission-complete layout. Input services remain stopped.
      window.findChild<QWidget *>("permissionBanner")->hide();
      window.findChild<QPushButton *>("btnToggleCore")->setEnabled(true);
      QCoreApplication::processEvents();
      QVERIFY(window.grab().save(output + "/home-sharing.png"));
      window.resize(747, 600);
      QCoreApplication::processEvents();
      window.findChild<QWidget *>("permissionBanner")->hide();
      window.findChild<QPushButton *>("btnToggleCore")->setEnabled(true);
      QCoreApplication::processEvents();
      QVERIFY(window.grab().save(output + "/home-prototype-width.png"));
      window.resize(1060, 650);
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
      window.findChild<QPushButton *>("btnInitialBluetooth")->click();
      QCoreApplication::processEvents();
      QVERIFY(window.grab().save(output + "/home-receiving-bluetooth.png"));
      auto *picker = static_cast<DiscoveryComboBox *>(window.findChild<QComboBox *>("bluetoothTarget"));
      picker->refreshRequested = {}; // Visual fixture must not scan or request OS permissions.
      auto *devices = static_cast<NearbyBluetoothModel *>(picker->model());
      devices->addDevice("work", "AA:00:00:00:00:01", QString::fromUtf8("工作电脑"), true);
      devices->addDevice("mac", "AA:00:00:00:00:02", "MacBook Pro", true);
      devices->addDevice("speaker", "AA:00:00:00:00:03", QString::fromUtf8("蓝牙音箱"), true);
      picker->setCurrentIndex(devices->rowForIdentity("mac"));
      picker->showPopup();
      QCoreApplication::processEvents();
      auto *popupMotion = picker->findChild<QParallelAnimationGroup *>("bluetoothPopupAnimation");
      popupMotion->setCurrentTime(popupMotion->duration());
      QVERIFY(picker->view()->window()->grab().save(output + "/bluetooth-popup.png"));
      picker->hidePopup();
      popupMotion->setCurrentTime(popupMotion->duration());
      window.findChild<QPushButton *>("btnInitialNetwork")->click();
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
