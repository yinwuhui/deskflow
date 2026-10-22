/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "common/Settings.h"
#include "gui/MainWindow.h"
#include "gui/widgets/DeviceListPage.h"
#include "gui/widgets/ScreenLayoutPage.h"
#include "gui/widgets/SettingsPage.h"
#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTranslator>
#include <QtTest>

using Device = DeviceListPage::Device;
using Kind = DeviceListPage::Kind;
using State = DeviceListPage::State;

class DeviceListPageTests : public QObject
{
  Q_OBJECT
  const QList<Device> fixture{
      {"local", "Mac-mini", "192.0.2.12", "This computer", Kind::Local, State::Local},
      {"computer:work", "work", {}, "Configured computer", Kind::Receiver, State::Connected, false, true},
      {"computer:book", "book", {}, "Configured computer", Kind::Receiver, State::Offline, false, true},
      {"server:192.0.2.30:24800", "studio", "192.0.2.30:24800", "Local discovery", Kind::Server, State::Discovered,
       false, false, true}
  };
  int deviceRows(DeviceListPage &page)
  {
    int count = 0;
    const auto list = page.findChild<QListWidget *>("devicesList");
    for (int i = 0; i < list->count(); ++i)
      if (!list->item(i)->data(Qt::UserRole).toString().isEmpty())
        ++count;
    return count;
  }
private Q_SLOTS:
  void init()
  {
    Settings::restoreDefaultSettings();
    Settings::setValue(Settings::Core::ComputerName, "Mac-mini");
    Settings::setValue(Settings::Core::CoreMode, Settings::Server);
    Settings::setValue(Settings::Security::TlsEnabled, false);
    Settings::setValue(Settings::Gui::AutoStartCore, false);
    Settings::setValue(Settings::Gui::LogExpanded, false);
    Settings::setValue(Settings::Gui::AutoUpdateCheck, false);
    Settings::setValue(Settings::Gui::RecentComputers, QStringList());
    Settings::setValue(Settings::Gui::KnownServers, QVariantMap());
    Settings::setValue(Settings::Gui::RecentServers, QStringList());
    Settings::setValue(Settings::Client::AutoConnect, false);
    Settings::setValue(Settings::Client::RemoteHost, "");
  }
  void validatesAndNormalizesEndpoints()
  {
    QCOMPARE(DeviceListPage::normalizedAddress(" HOST.local "), QString("host.local:24800"));
    QCOMPARE(DeviceListPage::normalizedAddress("host.local", 24801), QString("host.local:24801"));
    QCOMPARE(DeviceListPage::normalizedAddress("192.0.2.30:24801"), QString("192.0.2.30:24801"));
    QCOMPARE(DeviceListPage::normalizedAddress("[2001:db8::1]:24800"), QString("[2001:db8::1]:24800"));
    for (const auto &bad :
         {"", "a,b", "host:0", "host:65536", "host:abc", "user@host", "host/path", "host?q=x", "bad host"})
      QVERIFY2(DeviceListPage::normalizedAddress(bad).isEmpty(), bad);
  }
  void filtersSearchAndRestoresTheListAfterNoResults()
  {
    DeviceListPage page;
    page.setDevices(fixture, true, "Ready");
    QCOMPARE(deviceRows(page), 4);
    auto search = page.findChild<QLineEdit *>("deviceSearch");
    search->setText("STUDIO");
    QCOMPARE(deviceRows(page), 1);
    search->setText("192.0.2.30");
    QCOMPARE(deviceRows(page), 1);
    search->setText("missing");
    QCOMPARE(deviceRows(page), 0);
    search->clear();
    QCOMPARE(deviceRows(page), 4);
    auto filter = page.findChild<QComboBox *>("deviceFilter");
    filter->setCurrentIndex(1);
    QCOMPARE(deviceRows(page), 1);
    filter->setCurrentIndex(2);
    QCOMPARE(deviceRows(page), 1);
    filter->setCurrentIndex(3);
    QCOMPARE(deviceRows(page), 1);
  }
  void rowsRemainAccessibleWithoutSyntheticNativeTableCells()
  {
    DeviceListPage page;
    page.setDevices(fixture, true, "Ready");
    auto list = page.findChild<QListWidget *>("devicesList");
    const auto accessible = QAccessible::queryAccessibleInterface(list);
    QVERIFY(accessible);
    QCOMPARE(accessible->role(), QAccessible::List);
    QVERIFY(!accessible->tableInterface());
    auto row = qobject_cast<QPushButton *>(list->itemWidget(list->item(1)));
    QVERIFY(row);
    const auto rowAccessible = QAccessible::queryAccessibleInterface(row);
    QVERIFY(rowAccessible && rowAccessible->actionInterface());
    QVERIFY(rowAccessible->actionInterface()->actionNames().contains(QAccessibleActionInterface::pressAction()));
    QVERIFY(!rowAccessible->text(QAccessible::Name).isEmpty());
  }
  void typingKeepsSearchFocusWhileRowsChange()
  {
    DeviceListPage page;
    page.resize(800, 700);
    page.show();
    page.setDevices(fixture, true, "Ready");
    auto search = page.findChild<QLineEdit *>("deviceSearch");
    search->setFocus();
    QCoreApplication::processEvents();
    QTest::keyClicks(search, "studio");
    QCOMPARE(search->text(), QString("studio"));
    QCOMPARE(QApplication::focusWidget(), search);
    QCOMPARE(deviceRows(page), 1);
  }
  void connectionRequestsDoNotInventConnectedState()
  {
    DeviceListPage page;
    page.setDevices(fixture, true, "Ready");
    page.selectDevice("server:192.0.2.30:24800");
    QSignalSpy connectSpy(&page, &DeviceListPage::connectRequested);
    page.findChild<QCheckBox *>("deviceAutoConnect")->setChecked(true);
    page.findChild<QPushButton *>("devicePrimaryAction")->click();
    QCOMPARE(connectSpy.count(), 1);
    QCOMPARE(connectSpy.first().at(0).toString(), QString("192.0.2.30:24800"));
    QVERIFY(connectSpy.first().at(2).toBool());
    QCOMPARE(page.findChild<QLabel *>("deviceDetailState")->text(), QString("Discoverable"));
    QVERIFY(!page.findChild<QPushButton *>("deviceLayoutAction")->isEnabled());
    auto live = fixture;
    live.last().state = State::Connected;
    page.setDevices(live, false, "Connected");
    QCOMPARE(page.selectedDevice(), QString("server:192.0.2.30:24800"));
    QCOMPARE(page.findChild<QLabel *>("deviceDetailState")->text(), QString("Connected"));
    QSignalSpy disconnectSpy(&page, &DeviceListPage::disconnectRequested);
    page.findChild<QPushButton *>("devicePrimaryAction")->click();
    QCOMPARE(disconnectSpy.count(), 1);
    live.removeLast();
    page.setDevices(live, false, "Stopped");
    QCOMPARE(page.selectedDevice(), QString("local"));
  }
  void addingValidatesAndSeparatesSavedServersFromLayoutDrafts()
  {
    DeviceListPage page;
    page.setDevices(fixture, true, "Ready");
    QSignalSpy layout(&page, &DeviceListPage::layoutRequested);
    QSignalSpy server(&page, &DeviceListPage::addServerRequested);
    page.findChild<QPushButton *>("deviceAddButton")->click();
    auto submit = page.findChild<QPushButton *>("deviceAddSubmit");
    submit->click();
    QCOMPARE(layout.count(), 0);
    QVERIFY(!page.findChild<QLabel *>("deviceAddError")->text().isEmpty());
    page.findChild<QLineEdit *>("deviceAddName")->setText("new-peer");
    submit->click();
    QCOMPARE(layout.count(), 1);
    QCOMPARE(layout.first().at(0).toString(), QString("new-peer"));
    page.findChild<QComboBox *>("deviceAddKind")->setCurrentIndex(1);
    page.findChild<QLineEdit *>("deviceAddAddress")->setText("host:0");
    submit->click();
    QCOMPARE(server.count(), 0);
    page.findChild<QLineEdit *>("deviceAddAddress")->setText("new-host.local");
    submit->click();
    QCOMPARE(server.count(), 1);
    QCOMPARE(server.first().at(1).toString(), QString("new-host.local:24800"));
  }
  void navigationAndRealConfigurationAreIntegrated()
  {
    Settings::setValue(Settings::Gui::KnownServers, QVariantMap{{"192.0.2.30:24800", "studio"}});
    Settings::setValue(Settings::Client::RemoteHost, "192.0.2.30");
    MainWindow window;
    window.serverConfig().addClient("work");
    auto nav = window.findChild<QPushButton *>("btnDevices");
    nav->setChecked(true);
    auto page = window.findChild<DeviceListPage *>();
    QVERIFY(page);
    auto stack = window.findChild<QStackedWidget *>("mainPages");
    QCOMPARE(stack->currentWidget(), page);
    QVERIFY(nav->isChecked());
    QVERIFY(!window.findChild<QPushButton *>("btnHome")->isChecked());
    page->selectDevice("computer:work");
    QCOMPARE(page->findChild<QLabel *>("deviceDetailState")->text(), QString("Not connected"));
    page->findChild<QPushButton *>("deviceSettingsAction")->click();
    QVERIFY(qobject_cast<SettingsPage *>(stack->currentWidget()));
    QCOMPARE(window.findChild<QComboBox *>("settingsDeviceSelector")->currentText(), QString("work"));
    nav->setChecked(true);
    page->selectDevice("server:192.0.2.30:24800");
    QCOMPARE(page->findChild<QLabel *>("deviceDetailName")->text(), QString("studio"));
    // The configured hostname without an explicit port merges with the saved default-port endpoint.
    auto search = page->findChild<QLineEdit *>("deviceSearch");
    search->setText("192.0.2.30");
    QCOMPARE(deviceRows(*page), 1);
    search->clear();
    page->selectDevice("computer:work");
    page->findChild<QPushButton *>("deviceLayoutAction")->click();
    QVERIFY(qobject_cast<ScreenLayoutPage *>(stack->currentWidget()));
    window.findChild<QPushButton *>("btnHome")->setChecked(true);
    QCOMPARE(stack->currentWidget()->objectName(), QString("homeScroll"));
    nav->setChecked(true);
    page->findChild<QPushButton *>("deviceAddButton")->click();
    page->findChild<QComboBox *>("deviceAddKind")->setCurrentIndex(1);
    page->findChild<QLineEdit *>("deviceAddName")->setText("new-host");
    page->findChild<QLineEdit *>("deviceAddAddress")->setText("192.0.2.77");
    page->findChild<QPushButton *>("deviceAddSubmit")->click();
    QCOMPARE(
        Settings::value(Settings::Gui::KnownServers).toMap().value("192.0.2.77:24800").toString(), QString("new-host")
    );
    QCOMPARE(window.coreMode(), Settings::Server);
    QCOMPARE(page->findChild<QLabel *>("deviceDetailState")->text(), QString("Not connected"));
  }
  void detailsAndAddFormFitAtMinimumWidth()
  {
    DeviceListPage page;
    page.resize(545, 650);
    page.show();
    page.setDevices(fixture, true, "Ready");
    page.selectDevice("server:192.0.2.30:24800");
    QCoreApplication::processEvents();
    for (auto scroll : page.findChildren<QScrollArea *>())
      if (scroll->isVisible())
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
    page.findChild<QPushButton *>("deviceAddButton")->click();
    page.findChild<QComboBox *>("deviceAddKind")->setCurrentIndex(1);
    QCoreApplication::processEvents();
    for (auto scroll : page.findChildren<QScrollArea *>())
      if (scroll->isVisible())
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
  }
  void renderEvidence()
  {
    const auto output = qEnvironmentVariable("DESKFLOW_DEVICES_EVIDENCE");
    if (output.isEmpty())
      QSKIP("Set DESKFLOW_DEVICES_EVIDENCE to capture device-list screenshots.");
    QTranslator chinese;
    QVERIFY(chinese.load(qEnvironmentVariable("DESKFLOW_HOME_TRANSLATION")));
    qApp->installTranslator(&chinese);
    {
      MainWindow window;
      window.menuBar()->hide();
      window.findChild<QPushButton *>("btnDevices")->setChecked(true);
      auto page = window.findChild<DeviceListPage *>();
      auto data = fixture;
      data[0].name = QString::fromUtf8("我的 Mac mini");
      data[1].name = QString::fromUtf8("工作电脑");
      data[2].name = "MacBook Pro";
      data[3].name = QString::fromUtf8("工作室 Mac");
      for (const QSize size : {QSize(1060, 790), QSize(747, 840), QSize(820, 600)}) {
        window.resize(size);
        window.show();
        QCoreApplication::processEvents();
        page->setDevices(data, true, QString::fromUtf8("原型对照 · 测试数据 · 未启动共享服务"));
        page->selectDevice("local");
        QCoreApplication::processEvents();
        QVERIFY(window.grab().save(output + QString("/devices-%1.png").arg(size.width())));
        const auto split = page->findChild<QWidget *>("deviceSplitter");
        QVERIFY(split->width() <= page->width());
        for (auto scroll : page->findChildren<QScrollArea *>())
          if (scroll->isVisible())
            QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        page->selectDevice("server:192.0.2.30:24800");
        QCoreApplication::processEvents();
        for (auto scroll : page->findChildren<QScrollArea *>())
          if (scroll->isVisible())
            QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        page->findChild<QPushButton *>("deviceAddButton")->click();
        QCoreApplication::processEvents();
        for (auto scroll : page->findChildren<QScrollArea *>())
          if (scroll->isVisible())
            QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        page->selectDevice("local");
      }
      window.resize(1060, 790);
      page->selectDevice("server:192.0.2.30:24800");
      QCoreApplication::processEvents();
      QVERIFY(window.grab().save(output + "/devices-discovered.png"));
    }
    qApp->removeTranslator(&chinese);
  }
};

int main(int argc, char **argv)
{
  QTemporaryDir profile;
  qputenv("XDG_CONFIG_HOME", profile.path().toUtf8());
  qputenv("XDG_STATE_HOME", profile.path().toUtf8());
  QApplication app(argc, argv);
  Settings::setSettingsFile(profile.path() + "/Deskflow.conf");
  DeviceListPageTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "DeviceListPageTests.moc"
