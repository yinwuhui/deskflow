/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "SettingsTests.h"

#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

void SettingsTests::initTestCase()
{
  QFile oldSettings(m_settingsFile);
  if (oldSettings.exists())
    oldSettings.remove();
}

void SettingsTests::setSettingsFile()
{
  Settings::setSettingsFile(m_settingsFile);
}

void SettingsTests::setStateFile()
{
  Settings::setStateFile(m_stateFile);
}

void SettingsTests::settingsFile()
{
  QVERIFY(Settings::settingsFile().endsWith(m_settingsFile));
}

void SettingsTests::settingsPath()
{
  QVERIFY(Settings::settingsPath().endsWith(m_settingsPath));
}

void SettingsTests::tlsDir()
{
  QVERIFY(Settings::tlsDir().endsWith(m_expectedTlsDir));
}

void SettingsTests::serverConfigurationPaths()
{
  const auto oldMode = Settings::value(Settings::Core::ProcessMode);
  const auto oldExternal = Settings::value(Settings::Server::ExternalConfig);
  const auto oldFile = Settings::value(Settings::Server::ExternalConfigFile);
  Settings::setValue(Settings::Server::ExternalConfig, false);
  Settings::setValue(Settings::Core::ProcessMode, Settings::ProcessMode::Desktop);
#ifdef Q_OS_WIN
  QCOMPARE(QFileInfo(Settings::serverConfigFile()).absolutePath(), QFileInfo(Settings::settingsFile()).absolutePath());
  QCOMPARE(Settings::tlsDir(), QDir(QFileInfo(Settings::settingsFile()).absolutePath()).filePath("tls"));
  QCOMPARE(QFileInfo(Settings::tlsTrustedClientsDb()).absolutePath(), Settings::tlsDir());
  Settings::setValue(Settings::Core::ProcessMode, Settings::ProcessMode::Service);
  QCOMPARE(QFileInfo(Settings::serverConfigFile()).absolutePath(), Settings::SystemDir);
  QCOMPARE(Settings::tlsDir(), QDir(Settings::SystemDir).filePath("tls"));
#else
  QCOMPARE(QFileInfo(Settings::serverConfigFile()).absolutePath(), Settings::settingsPath());
#endif
  Settings::setValue(Settings::Server::ExternalConfigFile, "custom/server.conf");
  Settings::setValue(Settings::Server::ExternalConfig, true);
  QCOMPARE(Settings::serverConfigFile(), QString("custom/server.conf"));
  Settings::setValue(Settings::Core::ProcessMode, oldMode);
  Settings::setValue(Settings::Server::ExternalConfig, oldExternal);
  Settings::setValue(Settings::Server::ExternalConfigFile, oldFile);
}

void SettingsTests::tlsTrustedServersDb()
{
  QVERIFY(Settings::tlsTrustedServersDb().endsWith(m_expectedTlsServerDB));
}

void SettingsTests::tlsTrustedClientsDb()
{
  QVERIFY(Settings::tlsTrustedClientsDb().endsWith(m_expectedTlsClientDB));
}

void SettingsTests::checkValidSettings()
{
  QSignalSpy spy(Settings::instance(), &Settings::settingsChanged);
  QVERIFY(spy.isValid());

  const auto keysToCheck = QRegularExpression(QLatin1String("[^%1]").arg(Settings::Core::ComputerName));
  const auto validKeys = Settings::validKeys().filter(keysToCheck);
  for (const auto &setting : validKeys) {
    const auto value = Settings::value(setting).toString();
    QCOMPARE(Settings::defaultValue(setting).toString(), value);

    Settings::setValue(setting, "NEW_VALUE");
    QCOMPARE(spy.count(), 1);
    QCOMPARE(qvariant_cast<QString>(spy.first().at(0)), setting);
    QCOMPARE(Settings::value(setting).toString(), "NEW_VALUE");

    Settings::setValue(setting);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(Settings::value(setting).toString(), value);

    // Reset the spy for the next loop
    spy.clear();
    QCOMPARE(spy.count(), 0);
  }
}

void SettingsTests::checkCleanComputerName()
{
  const auto input = QStringLiteral("--!_ _-C@o#mpu$t%e^&*(r)= +Name\n[1]2|3?4--5>6<,7`~/8*90\\.lan--..    ..");
  const auto expected = QStringLiteral("Computer_Name_1234--567890.lan");

  Settings::setValue(Settings::Core::ComputerName, input);

  QCOMPARE(Settings::value(Settings::Core::ComputerName).toString(), expected);
}

void SettingsTests::checkCleanComputerName_LongName()
{
  QString input;
  input.fill('f', 300);
  input.prepend('.');

  QString expected;
  expected.fill('f', 255);

  Settings::setValue(Settings::Core::ComputerName, input);

  QCOMPARE(Settings::value(Settings::Core::ComputerName).toString(), expected);
}

void SettingsTests::checkClearState()
{
  Settings::setValue(Settings::Gui::WindowGeometry, QRect(1, 2, 3, 4));
  QVERIFY(Settings::value(Settings::Gui::WindowGeometry).toRect().isValid());

  Settings::clearState();

  QVERIFY(!Settings::value(Settings::Gui::WindowGeometry).toRect().isValid());
}

QTEST_MAIN(SettingsTests)

void SettingsTests::autoBackupMigration()
{
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto path = directory.filePath("migration.conf");
  {
    QSettings raw(path, QSettings::IniFormat);
    raw.setValue(Settings::Connection::AutoFailover, false);
    raw.sync();
  }
  Settings::setSettingsFile(path);
  QVERIFY(!Settings::value(Settings::Connection::AutoBackup).toBool());
  Settings::setValue(Settings::Connection::AutoBackup, true);
  Settings::setSettingsFile(path);
  QVERIFY(Settings::value(Settings::Connection::AutoBackup).toBool());
  const auto fresh = directory.filePath("fresh.conf");
  Settings::setSettingsFile(fresh);
  QVERIFY(Settings::value(Settings::Connection::AutoBackup).toBool());
  Settings::setSettingsFile(m_settingsFile);
}
