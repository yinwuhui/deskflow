/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "gui/MainWindow.h"
#include "gui/widgets/LogDock.h"

#include "gui/widgets/HomeDeviceList.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocalServer>
#include <QMenuBar>
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

private Q_SLOTS:
  void constructingAWindowDoesNotOwnTheProductionInstanceEndpoint()
  {
    MainWindow window;
    auto listener = window.findChild<QLocalServer *>("guiInstanceServer");
    QVERIFY(listener);
    QVERIFY(!listener->isListening());
  }

  void init()
  {
    Settings::setValue(Settings::Gui::AutoStartCore, false);
    Settings::setValue(Settings::Gui::LogExpanded, false);
    Settings::setValue(Settings::Gui::WindowGeometry, QRect(0, 0, 971, 197));
    Settings::setValue(Settings::Security::TlsEnabled, false);
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Server);
    Settings::setValue(Settings::Core::ComputerName, "home-test-computer");
    Settings::setValue(Settings::Client::RemoteHost, "");
    Settings::setValue(Settings::Client::AutoConnect, false);
    Settings::setValue(Settings::Core::Interface, "");
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
    QCOMPARE(window.findChild<QLabel *>("lblComputerName")->text(), QString("home-test-computer"));
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
    QCOMPARE(window.findChild<QLabel *>("lblComputerName")->text(), QString("home-test-computer"));
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
      QVERIFY(window.grab().save(output + "/home-sharing.png"));
      window.resize(747, 840);
      QCoreApplication::processEvents();
      window.findChild<QWidget *>("permissionBanner")->hide();
      window.findChild<QPushButton *>("btnToggleCore")->setEnabled(true);
      auto cards = window.findChild<QListWidget *>("deviceList");
      QCOMPARE(cards->count(), 2);
      QVERIFY(cards->visualItemRect(cards->item(1)).right() <= cards->viewport()->rect().right());
      QVERIFY(window.grab().save(output + "/home-prototype-width.png"));
      window.findChild<QRadioButton *>("rbModeClient")->click();
      window.findChild<QLineEdit *>("lineHostname")->setText("192.0.2.42");
      QCoreApplication::processEvents();
      window.findChild<QWidget *>("permissionBanner")->hide();
      QVERIFY(window.grab().save(output + "/home-receiving.png"));
      window.resize(820, 600);
      QCoreApplication::processEvents();
      window.findChild<QWidget *>("permissionBanner")->hide();
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
