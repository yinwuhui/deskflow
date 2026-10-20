// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "common/Settings.h"
#include "gui/ComputerSetupModel.h"
#include "gui/config/ServerConfig.h"
#include <QTemporaryDir>
#include <QtTest>
class LayoutMigrationTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void resizingPreservesIdentityAndOccupiedCells()
  {
    ComputerList computers(2);
    computers.resize(4);
    computers[0] = Computer("local");
    computers[3] = Computer("remote");
    const auto identity = computers[3].stableId();
    ComputerSetupModel model(computers, 2, 2);
    QVERIFY(!model.resizeGrid(1, 2));
    QCOMPARE(computers[3].stableId(), identity);
    QVERIFY(model.resizeGrid(100, 100));
    QCOMPARE(computers[101].stableId(), identity);
    QVERIFY(model.moveComputer(1, 1, 0, 0));
    QCOMPARE(computers[0].stableId(), identity);
    QCOMPARE(computers[101].name(), QString("local"));
    QVERIFY(!model.moveComputer(0, 0, 100, 0));
  }
  void disconnectedAndDiagonalDevicesAreReportedWithoutDroppingUnknownFields()
  {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
    qputenv("XDG_STATE_HOME", directory.path().toUtf8());
    Settings::setSettingsFile(directory.filePath("profile.conf"));
    Settings::setValue(Settings::Core::ComputerName, "local");
    Settings::setValue(Settings::Server::GridWidth, 3);
    Settings::setValue(Settings::Server::GridHeight, 3);
    auto &settings = Settings::proxy().get();
    settings.setValue("future/unknown", "preserved");
    settings.beginGroup("internalConfig");
    settings.setValue("futureOption", 42);
    settings.beginWriteArray("screens", 9);
    for (const auto &entry : QList<QPair<int, QString>>{{0, "local"}, {1, "adjacent"}, {8, "diagonal"}}) {
      settings.setArrayIndex(entry.first);
      settings.setValue("name", entry.second);
    }
    settings.endArray();
    settings.endGroup();
    ServerConfig config;
    QCOMPARE(config.unreachableComputers(), QStringList{"diagonal"});
    const auto identity = std::as_const(config).computers()[8].stableId();
    config.commit();
    Settings::save(false);
    ServerConfig restored;
    QCOMPARE(std::as_const(restored).computers()[8].stableId(), identity);
    QCOMPARE(settings.value("future/unknown").toString(), QString("preserved"));
    QCOMPARE(settings.value("internalConfig/futureOption").toInt(), 42);
  }
};
QTEST_MAIN(LayoutMigrationTests)
#include "LayoutMigrationTests.moc"
