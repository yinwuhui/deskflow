// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "common/Settings.h"
#include "gui/Diagnostic.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
class MaintenanceTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void onlyManagedFilesAreRemoved()
  {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
    qputenv("XDG_STATE_HOME", directory.path().toUtf8());
    Settings::setSettingsFile(directory.filePath("profile.conf"));
    const auto notePath = directory.filePath("personal.txt"), externalPath = directory.filePath("external.conf");
    for (const auto &path : {notePath, externalPath}) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("preserve");
    }
    Settings::setValue(Settings::Server::ExternalConfig, true);
    Settings::setValue(Settings::Server::ExternalConfigFile, externalPath);
    Settings::save(false);
    const auto managed = deskflow::gui::diagnostic::managedProfileFiles();
    QVERIFY(managed.contains(Settings::settingsFile()));
    QVERIFY(!managed.contains(notePath));
    QVERIFY(!managed.contains(externalPath));
    QStringList failed;
    QVERIFY(deskflow::gui::diagnostic::clearSettings(false, &failed));
    QVERIFY(failed.isEmpty());
    for (const auto &path : {notePath, externalPath}) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::ReadOnly));
      QCOMPARE(file.readAll(), QByteArray("preserve"));
    }
  }
};
QTEST_MAIN(MaintenanceTests)
#include "MaintenanceTests.moc"
