/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "common/Settings.h"
#include "gui/LoginStartup.h"
#include "gui/MainWindow.h"
#include "gui/dialogs/ActionDialog.h"
#include "gui/widgets/KeySequenceWidget.h"
#include "gui/widgets/SettingsPage.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTranslator>
#include <QXmlStreamReader>
#include <QtTest>

class SettingsPageTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void init()
  {
    Settings::restoreDefaultSettings();
    Settings::setValue(Settings::Security::TlsEnabled, false);
    Settings::setValue(Settings::Gui::AutoStartCore, false);
    Settings::setValue(Settings::Client::AutoConnect, false);
    Settings::setValue(Settings::Gui::LogExpanded, false);
    Settings::setValue(Settings::Core::ComputerName, "settings-test");
    Settings::setValue(Settings::Core::CoreMode, Settings::Server);
  }

  void loginStartupRegistrationIsReversibleAndEscaped()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString executable = QCoreApplication::applicationFilePath();
#ifndef Q_OS_WIN
    const auto link = dir.filePath("Deskflow test & app");
    QVERIFY(QFile::link(executable, link));
    executable = link;
#endif
    LoginStartup startup(executable, dir.filePath("registration"), "org.deskflow.test");
    QVERIFY(!startup.isEnabled());
    QString error;
    QVERIFY2(startup.setEnabled(true, error), qPrintable(error));
    QVERIFY(startup.isEnabled());
#ifdef Q_OS_MACOS
    QFile file(startup.registrationPath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QXmlStreamReader xml(file.readAll());
    QStringList strings;
    while (!xml.atEnd()) {
      xml.readNext();
      if (xml.isStartElement() && xml.name() == QStringLiteral("string"))
        strings.append(xml.readElementText());
    }
    QVERIFY(!xml.hasError());
    QVERIFY(strings.contains(executable));
    QVERIFY(strings.contains("org.deskflow.test.login"));
#endif
    QVERIFY(startup.setEnabled(false, error));
    QVERIFY(!startup.isEnabled());
    QVERIFY(startup.setEnabled(false, error));
  }

  void editsAreDraftsAcrossCategoriesAndCanBeDiscarded()
  {
    QTemporaryDir dir;
    ServerConfig config;
    SettingsPage page(config, nullptr, LoginStartup({}, dir.path(), "org.deskflow.test"));
    const auto port = Settings::value(Settings::Core::Port).toInt();
    page.findChild<QSpinBox *>(Settings::Core::Port)->setValue(port + 1);
    QVERIFY(page.isModified());
    QCOMPARE(Settings::value(Settings::Core::Port).toInt(), port);
    page.activate(SettingsPage::Network);
    page.activate(SettingsPage::General);
    QCOMPARE(page.findChild<QSpinBox *>(Settings::Core::Port)->value(), port + 1);
    page.discard();
    QVERIFY(!page.isModified());
    QCOMPARE(page.findChild<QSpinBox *>(Settings::Core::Port)->value(), port);
  }

  void savePersistsOnlyAfterApplyingAndReportsRuntimeChanges()
  {
    QTemporaryDir dir;
    LoginStartup startup({}, dir.path(), "org.deskflow.test");
    ServerConfig config;
    SettingsPage page(config, nullptr, startup);
    QSignalSpy saved(&page, &SettingsPage::saved);
    auto close = page.findChild<QComboBox *>(Settings::Gui::CloseToTray);
    close->setCurrentIndex(close->findData(false));
    auto autostart = page.findChild<QCheckBox *>("@loginStartup");
    autostart->setChecked(true);
    QVERIFY(!startup.isEnabled());
    QVERIFY(page.save());
    QCOMPARE(saved.count(), 1);
    QVERIFY(!saved.at(0).at(0).toBool());
    QVERIFY(startup.isEnabled());
    QCOMPARE(Settings::value(Settings::Gui::CloseToTray).toBool(), false);
    QVERIFY(!page.isModified());
    page.findChild<QSpinBox *>(Settings::Core::Port)->setValue(24817);
    page.findChild<QCheckBox *>(Settings::Server::EnableClipboard)->setChecked(false);
    QVERIFY(page.save());
    QVERIFY(saved.at(1).at(0).toBool());
    QCOMPARE(Settings::value(Settings::Core::Port).toInt(), 24817);
    QCOMPARE(Settings::value(Settings::Server::EnableClipboard).toBool(), false);
    QString error;
    QVERIFY(startup.setEnabled(false, error));
  }

  void failedStartupRegistrationDoesNotApplyOtherSettings()
  {
    QTemporaryDir dir;
    QFile blocker(dir.filePath("not-a-directory"));
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    ServerConfig config;
    SettingsPage page(config, nullptr, LoginStartup({}, blocker.fileName() + "/child", "org.deskflow.test"));
    const auto port = Settings::value(Settings::Core::Port).toInt();
    page.findChild<QSpinBox *>(Settings::Core::Port)->setValue(port + 2);
    page.findChild<QCheckBox *>("@loginStartup")->setChecked(true);
    QVERIFY(!page.save());
    QCOMPARE(Settings::value(Settings::Core::Port).toInt(), port);
    QVERIFY(page.isModified());
    QVERIFY(!page.findChild<QLabel *>("settingsSaveState")->text().isEmpty());
  }

  void deviceEditsAreDraftedAndConcurrentChangesAreProtected()
  {
    QTemporaryDir dir;
    ServerConfig config;
    config.addClient("peer-a");
    SettingsPage page(config, nullptr, LoginStartup({}, dir.path(), "org.deskflow.test"));
    page.activate(SettingsPage::Input);
    auto devices = page.findChild<QComboBox *>("settingsDeviceSelector");
    devices->setCurrentIndex(devices->findText("peer-a"));
    const auto before = config;
    auto fields = page.findChild<QWidget *>("settingsDeviceFields");
    const auto combos = fields->findChildren<QComboBox *>();
    QVERIFY(!combos.isEmpty());
    combos.first()->setCurrentIndex((combos.first()->currentIndex() + 1) % 6);
    QVERIFY(page.isModified());
    QVERIFY(config == before);
    config.addClient("peer-b");
    QVERIFY(!page.save());
    QVERIFY(config.computerExists("peer-b"));
    page.discard();
    QVERIFY(!page.isModified());
  }

  void rejectsEmptyCommandsAndIncompleteHotkeys()
  {
    QTemporaryDir dir;
    ServerConfig config;
    SettingsPage page(config, nullptr, LoginStartup({}, dir.path(), "org.deskflow.test"));
    page.findChild<QCheckBox *>(Settings::Core::EnableEnterCommand)->setChecked(true);
    page.findChild<QLineEdit *>(Settings::Core::ComputerEnterCommand)->clear();
    QVERIFY(!page.save());
    QVERIFY(!Settings::value(Settings::Core::EnableEnterCommand).toBool());
    page.discard();
    KeySequence seq;
    QVERIFY(seq.appendKey(Qt::Key_Right, Qt::ControlModifier | Qt::AltModifier));
    page.findChild<QPushButton *>("addHotkeyRule")->click();
    page.findChild<KeySequenceWidget *>("settingsKeySequence")->setKeySequence(seq);
    page.findChild<QPushButton *>("confirmHotkeyRule")->click();
    QCOMPARE(page.findChild<QListWidget *>("settingsHotkeys")->count(), 1);
    QVERIFY(!page.save());
    page.discard();
    QCOMPARE(page.findChild<QListWidget *>("settingsHotkeys")->count(), 0);
  }

  void inlineHotkeyActionsPersistAndRemainEmbedded()
  {
    QTemporaryDir dir;
    ServerConfig config;
    SettingsPage page(config, nullptr, LoginStartup({}, dir.path(), "org.deskflow.test"));
    KeySequence seq;
    QVERIFY(seq.appendKey(Qt::Key_Right, Qt::ControlModifier | Qt::AltModifier));
    page.findChild<QPushButton *>("addHotkeyRule")->click();
    page.findChild<KeySequenceWidget *>("settingsKeySequence")->setKeySequence(seq);
    page.findChild<QPushButton *>("confirmHotkeyRule")->click();
    page.findChild<QPushButton *>("addHotkeyAction")->click();
    auto editor = page.findChild<ActionDialog *>();
    QVERIFY(editor);
    QVERIFY(!editor->isWindow());
    QVERIFY(!page.save());
    editor->findChild<QComboBox *>("comboActionType")->setCurrentIndex(ActionDialog::ActionTypes::SwitchInDirection);
    editor->findChild<QComboBox *>("comboSwitchInDirection")->setCurrentIndex(1);
    editor->findChild<QDialogButtonBox *>("buttonBox")->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(page.findChild<QListWidget *>("settingsHotkeyActions")->count(), 1);
    QVERIFY(page.save());
    const ServerConfig &savedConfig = config;
    QCOMPARE(savedConfig.hotkeys().size(), 1);
    QCOMPARE(savedConfig.hotkeys().first().actions().first().switchDirection(), 1);
    ServerConfig reloaded;
    const ServerConfig &loadedConfig = reloaded;
    QCOMPARE(loadedConfig.hotkeys().size(), 1);
    QCOMPARE(
        loadedConfig.hotkeys().first().keySequence().sequence(), savedConfig.hotkeys().first().keySequence().sequence()
    );
    QCOMPARE(
        loadedConfig.hotkeys().first().actions().first().type(), savedConfig.hotkeys().first().actions().first().type()
    );
    QCOMPARE(loadedConfig.hotkeys().first().actions().first().switchDirection(), 1);
  }

  void invalidCertificateDoesNotApplyUnrelatedChanges()
  {
    QTemporaryDir dir;
    ServerConfig config;
    SettingsPage page(config, nullptr, LoginStartup({}, dir.path(), "org.deskflow.test"));
    const auto port = Settings::value(Settings::Core::Port).toInt();
    page.findChild<QSpinBox *>(Settings::Core::Port)->setValue(port + 3);
    page.findChild<QCheckBox *>(Settings::Security::TlsEnabled)->setChecked(true);
    page.findChild<QLineEdit *>(Settings::Security::Certificate)->setText(dir.filePath("missing.pem"));
    QVERIFY(!page.save());
    QCOMPARE(Settings::value(Settings::Core::Port).toInt(), port);
    QVERIFY(!Settings::value(Settings::Security::TlsEnabled).toBool());
    QVERIFY(!QFile::exists(dir.filePath("missing.pem")));
  }

  void settingsAreEmbeddedAndHomeRestoresWithoutLosingDrafts()
  {
    MainWindow window;
    window.show();
    // Accessibility activation toggles the navigation control without emitting clicked().
    window.findChild<QPushButton *>("btnSettings")->setChecked(true);
    auto page = window.findChild<SettingsPage *>();
    QVERIFY(page);
    QVERIFY(!page->isWindow());
    QVERIFY(page->isVisible());
    QCOMPARE(page->findChild<QTabBar *>("settingsTabs")->count(), 7);
    page->findChild<QSpinBox *>(Settings::Core::Port)->setValue(24819);
    window.findChild<QPushButton *>("btnHome")->click();
    QVERIFY(!page->isVisible());
    window.findChild<QPushButton *>("btnSettings")->click();
    QCOMPARE(page->findChild<QSpinBox *>(Settings::Core::Port)->value(), 24819);
    for (const auto size : {QSize(1060, 790), QSize(747, 840), QSize(740, 600)}) {
      window.resize(size);
      QCoreApplication::processEvents();
      auto save = page->findChild<QPushButton *>("saveSettings");
      QVERIFY(page->rect().contains(save->mapTo(page, save->rect().bottomRight())));
      QCOMPARE(window.size(), size);
    }
    page->discard();
  }

  void renderEvidence()
  {
    const auto output = qEnvironmentVariable("DESKFLOW_SETTINGS_EVIDENCE");
    if (output.isEmpty())
      QSKIP("No screenshot output requested");
    QTranslator chinese;
    QVERIFY(chinese.load(qEnvironmentVariable("DESKFLOW_SETTINGS_TRANSLATION")));
    qApp->installTranslator(&chinese);
    {
      MainWindow window;
      window.resize(1060, 820);
      window.show();
      window.findChild<QPushButton *>("btnSettings")->click();
      auto page = window.findChild<SettingsPage *>();
      QDir().mkpath(output);
      for (int i = 0; i < 7; ++i) {
        page->activate(static_cast<SettingsPage::Category>(i));
        QCoreApplication::processEvents();
        QVERIFY(window.grab().save(output + QString("/settings-%1.png").arg(i)));
      }
      page->activate();
      window.resize(747, 840);
      QCoreApplication::processEvents();
      QVERIFY(window.grab().save(output + "/settings-prototype-width.png"));
    }
    qApp->removeTranslator(&chinese);
    QCoreApplication::processEvents();
  }
};

int main(int argc, char **argv)
{
  QTemporaryDir profile;
  qputenv("XDG_CONFIG_HOME", profile.path().toUtf8());
  qputenv("XDG_STATE_HOME", profile.path().toUtf8());
  QApplication app(argc, argv);
  Settings::setSettingsFile(profile.filePath("settings.conf"));
  Settings::setStateFile(profile.filePath("state.conf"));
  SettingsPageTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "SettingsPageTests.moc"
