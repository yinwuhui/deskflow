// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "common/Settings.h"
#include "gui/core/ConfigurationBackup.h"
#include <QtTest>
using deskflow::gui::ConfigurationBackup;
class ConfigurationBackupTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void preservesTypesAndUnknownFields()
  {
    ConfigurationBackup original;
    original.settings = {
        {"computer_peer/aliases", QStringList{"别名", "alias"}},
        {"server/clipboardSize", 0},
        {"core/coreMode", QVariant::fromValue(Settings::CoreMode::Server)},
        {"extension/data", QByteArray("\0\xff", 2)},
        {"core/enterCommand", "echo \"$example\""}
    };
    original.peerIdentities = R"({"version":1,"peers":[]})";
    original.trustedServers = "v2:sha256:example\n";
    original.serverConfig = "section: links\nend\n# unknown text preserved\n";
    ConfigurationBackup restored;
    QString error;
    QVERIFY(ConfigurationBackup::decode(original.encode(), restored, error));
    QCOMPARE(restored.settings, original.settings);
    QCOMPARE(restored.serverConfig, original.serverConfig);
    QCOMPARE(restored.trustedServers, original.trustedServers);
    QCOMPARE(restored.peerIdentities, original.peerIdentities);
  }
  void corruptedBackupDoesNotMutateDestination()
  {
    ConfigurationBackup original;
    original.settings.insert("example", true);
    auto data = original.encode();
    data.replace("\"version\": 2", "\"version\": 99");
    ConfigurationBackup restored;
    restored.serverConfig = "unchanged";
    QString error;
    QVERIFY(!ConfigurationBackup::decode(data, restored, error));
    QCOMPARE(restored.serverConfig, QByteArray("unchanged"));
  }
};
QTEST_MAIN(ConfigurationBackupTests)
#include "ConfigurationBackupTests.moc"
