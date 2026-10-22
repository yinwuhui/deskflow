// SPDX-FileCopyrightText: (C) 2026 busymouse contributors
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "common/Settings.h"
#include "common/SettingsMigration.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class SettingsMigrationTests : public QObject
{
  Q_OBJECT

  static bool write(const QString &path, const QByteArray &bytes)
  {
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
      return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
  }

  static QByteArray read(const QString &path)
  {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
  }

private Q_SLOTS:
  void copiesProfileAndPreservesIdentity()
  {
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const auto oldDir = fixture.filePath("Deskflow");
    const auto newDir = fixture.filePath("busymouse");
    QVERIFY(write(oldDir + "/tls/deskflow.pem", "original-private-key-and-certificate"));
    QVERIFY(write(oldDir + "/tls/trusted-servers", "trusted-server-fingerprint"));
    QVERIFY(write(oldDir + "/tls/trusted-clients", "trusted-client-fingerprint"));
    QVERIFY(write(oldDir + "/deskflow-server.conf", "section: screens\n  laptop:\nend\n"));
    QVERIFY(write(fixture.filePath("Deskflow.state"), "[General]\nwindow=old-state\n"));
    {
      QSettings source(oldDir + "/Deskflow.conf", QSettings::IniFormat);
      source.setValue(Settings::Core::ComputerName, "my-laptop");
      source.setValue(Settings::Daemon::ConfigFile, oldDir + "/Deskflow.conf");
      source.setValue(Settings::Log::File, fixture.filePath("custom-log.log"));
      source.setValue(Settings::Gui::AutoUpdateCheck, true);
      source.setValue(Settings::Gui::UpdateCheckUrl, "https://api.deskflow.org/version");
    }
    const auto original = read(oldDir + "/Deskflow.conf");
    QVERIFY(
        migrateDeskflowProfile(oldDir, newDir, fixture.filePath("Deskflow.state"), fixture.filePath("busymouse.state"))
    );
    QSettings result(newDir + "/busymouse.conf", QSettings::IniFormat);
    QCOMPARE(result.value(Settings::Core::ComputerName).toString(), "my-laptop");
    QCOMPARE(result.value(Settings::Security::Certificate).toString(), newDir + "/tls/deskflow.pem");
    QCOMPARE(result.value(Settings::Server::ExternalConfigFile).toString(), newDir + "/deskflow-server.conf");
    QCOMPARE(result.value(Settings::Daemon::ConfigFile).toString(), newDir + "/busymouse.conf");
    QCOMPARE(result.value(Settings::Log::File).toString(), fixture.filePath("custom-log.log"));
    QVERIFY(!result.value(Settings::Gui::AutoUpdateCheck).toBool());
    QVERIFY(result.value(Settings::Gui::UpdateCheckUrl).toString().isEmpty());
    QCOMPARE(read(newDir + "/tls/deskflow.pem"), read(oldDir + "/tls/deskflow.pem"));
    QCOMPARE(read(newDir + "/tls/trusted-servers"), read(oldDir + "/tls/trusted-servers"));
    QCOMPARE(read(newDir + "/tls/trusted-clients"), read(oldDir + "/tls/trusted-clients"));
    QCOMPARE(read(fixture.filePath("busymouse.state")), read(fixture.filePath("Deskflow.state")));
    QCOMPARE(read(oldDir + "/Deskflow.conf"), original);
    QVERIFY(!migrateDeskflowProfile(oldDir, newDir));
  }

  void preservesExistingProfileAndState()
  {
    QTemporaryDir fixture;
    const auto oldDir = fixture.filePath("Deskflow");
    const auto newDir = fixture.filePath("busymouse");
    QVERIFY(write(oldDir + "/Deskflow.conf", "[core]\ncomputerName=legacy\n"));
    QVERIFY(write(newDir + "/busymouse.conf", "existing"));
    QVERIFY(!migrateDeskflowProfile(oldDir, newDir));
    QCOMPARE(read(newDir + "/busymouse.conf"), "existing");

    const auto otherDir = fixture.filePath("other");
    QVERIFY(write(fixture.filePath("old.state"), "legacy-state"));
    QVERIFY(write(fixture.filePath("new.state"), "existing-state"));
    QVERIFY(migrateDeskflowProfile(oldDir, otherDir, fixture.filePath("old.state"), fixture.filePath("new.state")));
    QCOMPARE(read(fixture.filePath("new.state")), "existing-state");
  }

  void remapsExplicitCertificateButPreservesCustomPaths()
  {
    QTemporaryDir fixture;
    const auto oldDir = fixture.filePath("Deskflow");
    const auto newDir = fixture.filePath("busymouse");
    QVERIFY(QDir().mkpath(oldDir));
    {
      QSettings source(oldDir + "/Deskflow.conf", QSettings::IniFormat);
      source.setValue(Settings::Security::Certificate, oldDir + "/tls/custom.pem");
      source.setValue(Settings::Server::ExternalConfigFile, fixture.filePath("external/server.conf"));
    }
    QVERIFY(migrateDeskflowProfile(oldDir, newDir));
    QSettings result(newDir + "/busymouse.conf", QSettings::IniFormat);
    QCOMPARE(result.value(Settings::Security::Certificate).toString(), newDir + "/tls/custom.pem");
    QCOMPARE(result.value(Settings::Server::ExternalConfigFile).toString(), fixture.filePath("external/server.conf"));
  }

  void missingProfileDoesNotCreateDestination()
  {
    QTemporaryDir fixture;
    QVERIFY(!migrateDeskflowProfile(fixture.filePath("missing"), fixture.filePath("busymouse")));
    QVERIFY(!QFileInfo::exists(fixture.filePath("busymouse")));
  }

#ifndef Q_OS_WIN
  void symbolicLinksAbortWithoutPublishingPartialProfile()
  {
    QTemporaryDir fixture;
    const auto oldDir = fixture.filePath("Deskflow");
    const auto newDir = fixture.filePath("busymouse");
    QVERIFY(write(oldDir + "/Deskflow.conf", "legacy"));
    QVERIFY(write(fixture.filePath("outside.pem"), "outside-key"));
    QVERIFY(QFile::link(fixture.filePath("outside.pem"), oldDir + "/linked.pem"));
    QVERIFY(!migrateDeskflowProfile(oldDir, newDir));
    QVERIFY(!QFileInfo::exists(newDir));
    QCOMPARE(read(fixture.filePath("outside.pem")), "outside-key");
  }
#endif
};

QTEST_GUILESS_MAIN(SettingsMigrationTests)
#include "SettingsMigrationTests.moc"
