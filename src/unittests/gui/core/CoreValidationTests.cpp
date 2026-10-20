// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "base/EventQueue.h"
#include "common/CoreControl.h"
#include "common/LayoutReload.h"
#include "common/Settings.h"
#include "gui/config/ServerConfig.h"
#include "gui/core/ConfigurationBackup.h"
#include "gui/core/CoreProcess.h"
#include "server/Config.h"
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#include <sstream>
class CoreValidationTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void coreControlAcceptsLayoutReloadAndRejectsMalformedRequests()
  {
    const auto encode = [](const QJsonObject &request) {
      return QString::fromLatin1(QJsonDocument(request)
                                     .toJson(QJsonDocument::Compact)
                                     .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    };
    QTemporaryDir directory;
    QJsonObject request{
        {"id", "attempt-1"}, {"command", "reloadLayout"}, {"argument", directory.filePath("candidate.conf")}
    };
    const auto accepted = deskflow::decodeCoreControlRequest(encode(request));
    QVERIFY(accepted);
    QCOMPARE(*accepted, request);
    request["argument"] = "relative.conf";
    QVERIFY(!deskflow::decodeCoreControlRequest(encode(request)));
    request["command"] = "pause";
    request["argument"] = true;
    QVERIFY(!deskflow::decodeCoreControlRequest(encode(request)));
    request["argument"] = "1";
    QVERIFY(deskflow::decodeCoreControlRequest(encode(request)));
    request["id"] = 42;
    QVERIFY(!deskflow::decodeCoreControlRequest(encode(request)));
    request["id"] = "attempt-2";
    request["command"] = "arbitraryCommand";
    QVERIFY(!deskflow::decodeCoreControlRequest(encode(request)));
    request["command"] = "revoke";
    request["argument"] = QString(64, 'a');
    QVERIFY(deskflow::decodeCoreControlRequest(encode(request)));
    request["argument"] = QString(65, 'a');
    QVERIFY(!deskflow::decodeCoreControlRequest(encode(request)));
    QVERIFY(!deskflow::decodeCoreControlRequest("invalid!!"));
    QVERIFY(!deskflow::decodeCoreControlRequest(QString(9000, 'a')));
  }
  void layoutReloadCannotApplyOtherRuntimeChanges()
  {
    QTemporaryDir directory;
    QSettings active(directory.filePath("active.conf"), QSettings::IniFormat);
    QSettings candidate(directory.filePath("candidate.conf"), QSettings::IniFormat);
    for (auto *settings : {&active, &candidate}) {
      settings->setValue(Settings::Core::CoreMode, QVariant::fromValue(Settings::CoreMode::Server));
      settings->setValue(Settings::Core::Port, 24800);
      settings->setValue("computer_existing/stableId", "existing-id");
      settings->setValue("computer_existing/modifiers", "unchanged");
    }
    candidate.setValue("internalConfig/screens/1/name", "existing");
    candidate.setValue("internalConfig/screens/2/name", "new");
    candidate.setValue("computer_new/stableId", "new-id");
    candidate.setValue(Settings::Server::GridWidth, 20);
    QVERIFY(deskflow::permitsLayoutReload(active, candidate));
    candidate.setValue(Settings::Core::Port, 24801);
    QVERIFY(!deskflow::permitsLayoutReload(active, candidate));
    candidate.setValue(Settings::Core::Port, 24800);
    candidate.setValue("computer_existing/modifiers", "changed");
    QVERIFY(!deskflow::permitsLayoutReload(active, candidate));
    candidate.setValue("computer_existing/modifiers", "unchanged");
    candidate.setValue("internalConfig/hotkeys/size", 1);
    QVERIFY(!deskflow::permitsLayoutReload(active, candidate));
  }
  void candidateGraphReadsItsOwnDeviceDefinitions()
  {
    QTemporaryDir directory;
    Settings::setSettingsFile(directory.filePath("profile.conf"));
    Settings::setValue(Settings::Core::ComputerName, "local");
    QMap<QString, QVariant> profile{
        {"computer_local/name", "local"},
        {"computer_new/name", "new"},
        {"computer_new/stableId", "new-id"},
        {"computer_new/aliases", QStringList{"new-alias"}}
    };
    deskflow::server::Config candidate(nullptr, profile);
    std::istringstream stream("section: links\nlocal:\nright = new\nnew:\nleft = local\nend\n");
    deskflow::server::ConfigReadContext context(stream);
    candidate.read(context);
    deskflow::server::Config active(nullptr, profile);
    QVERIFY(active.addComputer("local"));
    auto *filter = active.getInputFilter();
    filter->addFilterRule(InputFilter::Rule());
    active.applyLayoutFrom(candidate);
    QVERIFY(active.isComputer("new"));
    QCOMPARE(active.getInputFilter(), filter);
    QCOMPARE(filter->getNumRules(), uint32_t(1));

    QVERIFY(candidate.isComputer("new"));
    QCOMPARE(candidate.deviceStableId("new"), QString("new-id"));
    QVERIFY(!Settings::knownComputers().contains("new"));
  }
  void fullConfigurationArchiveRetainsDefinitionsAndParsesAfterRestore()
  {
    using deskflow::gui::ConfigurationBackup;
    ConfigurationBackup original;
    original.settings = {
        {Settings::Core::CoreMode, QVariant::fromValue(Settings::CoreMode::Server)},
        {Settings::Core::ComputerName, "local"},
        {Settings::Security::TlsEnabled, false},
        {"computer_local/name", "local"},
        {"computer_remote/name", "remote"},
        {Settings::Computer::Aliases.arg("remote"), QStringList{"remote-alias"}},
        {Settings::Computer::ModifierAlt.arg("remote"), "super"},
        {Settings::Computer::SwitchCornerTopLeft.arg("remote"), true},
        {Settings::Computer::SwitchCornerSize.arg("remote"), 12},
        {Settings::Server::ClipboardSize, 17},
        {"extension/unknown", QByteArray("preserved")}
    };
    original.serverConfig = "section: links\nlocal:\nright = remote\nremote:\nleft = local\nend\n";
    ConfigurationBackup restored;
    QString error;
    QVERIFY2(ConfigurationBackup::decode(original.encode(), restored, error), qPrintable(error));
    QCOMPARE(restored.settings, original.settings);
    deskflow::server::Config config(nullptr, restored.settings);
    std::istringstream stream(restored.serverConfig.toStdString());
    deskflow::server::ConfigReadContext context(stream);
    config.read(context);
    QCOMPARE(config.getCanonicalName("remote-alias"), std::string("remote"));
    QTemporaryDir directory;
    const auto graph = directory.filePath("restored.conf");
    QFile graphFile(graph);
    QVERIFY(graphFile.open(QIODevice::WriteOnly));
    QCOMPARE(graphFile.write(restored.serverConfig), restored.serverConfig.size());
    graphFile.close();
    const auto profilePath = directory.filePath("profile.conf");
    QSettings profile(profilePath, QSettings::IniFormat);
    for (auto it = restored.settings.cbegin(); it != restored.settings.cend(); ++it)
      profile.setValue(it.key(), it.value());
    profile.setValue(Settings::Server::ExternalConfig, true);
    profile.setValue(Settings::Server::ExternalConfigFile, graph);
    profile.sync();
    QProcess parser;
    parser.start(
        QString::fromUtf8(DESKFLOW_TEST_CORE_PATH), {"server", "--validate-config", "--settings", profilePath}
    );
    QVERIFY(parser.waitForFinished(5000));
    QVERIFY2(parser.exitCode() == 0, parser.readAllStandardOutput().constData());
  }
  void fileCheckUsesRealParserWithoutStartingSharedInput()
  {
    QTemporaryDir directory;
    Settings::setSettingsFile(directory.filePath("profile.conf"));
    Settings::setValue(Settings::Core::ComputerName, "local");
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Server);
    Settings::setValue(Settings::Security::TlsEnabled, false);
    ServerConfig configuration;
    configuration.commit();
    Settings::save(false);
    const auto graph = directory.filePath("graph.conf");
    QVERIFY(configuration.save(graph));
    deskflow::gui::CoreProcess core(configuration, QString::fromUtf8(DESKFLOW_TEST_CORE_PATH));
    QSignalSpy result(&core, &deskflow::gui::CoreProcess::fileValidationFinished);
    core.validateExternalFile(graph);
    QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 7000);
    QVERIFY2(result.first().at(1).toBool(), qPrintable(result.first().at(2).toString()));
    QCOMPARE(core.processState(), deskflow::core::ProcessState::Stopped);
    QFile bad(graph);
    QVERIFY(bad.open(QIODevice::WriteOnly));
    bad.write("invalid configuration");
    bad.close();
    core.validateExternalFile(graph);
    QTRY_COMPARE_WITH_TIMEOUT(result.count(), 2, 7000);
    QVERIFY(!result.last().at(1).toBool());
  }
  void sourceRecoveryUsesOldSnapshotWithoutDiscardingSavedCandidate()
  {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
    qputenv("XDG_STATE_HOME", directory.path().toUtf8());
    Settings::setSettingsFile(directory.filePath("profile.conf"));
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Client);
    Settings::setValue(Settings::Client::RemoteHost, "192.0.2.10");
    Settings::setValue(Settings::Security::TlsEnabled, false);
    ServerConfig configuration;
    deskflow::gui::CoreProcess core(configuration, QString::fromUtf8(DESKFLOW_TEST_CORE_PATH));
    core.setMode(Settings::CoreMode::Client);
    QVERIFY(core.prepareConfiguration());
    const auto original = core.runtimeConfiguration();
    Settings::setValue(Settings::Client::RemoteHost, "192.0.2.20");
    QVERIFY(core.prepareConfiguration());
    QVERIFY(core.runtimeSettingsFile() != original.settingsFile);
    QVERIFY(core.restoreRuntimeConfiguration(original));
    QCOMPARE(core.runtimeSettingsFile(), original.settingsFile);
    QCOMPARE(core.runtimeConfiguration().source, QString("192.0.2.10"));
    QCOMPARE(Settings::value(Settings::Client::RemoteHost).toString(), QString("192.0.2.20"));
    auto missing = original;
    missing.settingsFile = directory.filePath("missing.conf");
    QVERIFY(!core.restoreRuntimeConfiguration(missing));
    QCOMPARE(core.runtimeSettingsFile(), original.settingsFile);
  }
#ifndef Q_OS_WIN
  void explicitStopCancelsQueuedRestart()
  {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
    qputenv("XDG_STATE_HOME", directory.path().toUtf8());
    Settings::setSettingsFile(directory.filePath("profile.conf"));
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Client);
    Settings::setValue(Settings::Core::ProcessMode, Settings::ProcessMode::Desktop);
    Settings::setValue(Settings::Client::RemoteHost, "192.0.2.10");
    Settings::setValue(Settings::Security::TlsEnabled, false);
    // Harmless substitute process: no networking, IPC, clipboard or input.
    QFile process(directory.filePath("fake-core"));
    QVERIFY(process.open(QIODevice::WriteOnly));
    process.write("#!/bin/sh\nexec /bin/sleep 20\n");
    process.close();
    QVERIFY(process.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    ServerConfig configuration;
    deskflow::gui::CoreProcess core(configuration, process.fileName());
    core.setMode(Settings::CoreMode::Client);
    core.setAddress("192.0.2.10");
    core.start();
    QVERIFY(core.isStarted());
    core.restart();
    core.stop();
    QTRY_COMPARE(core.processState(), deskflow::core::ProcessState::Stopped);
    QTest::qWait(150);
    QCOMPARE(core.processState(), deskflow::core::ProcessState::Stopped);
    // Also cancel a restart queued while already stopped.
    core.restart();
    core.stop();
    QTest::qWait(150);
    QCOMPARE(core.processState(), deskflow::core::ProcessState::Stopped);
  }
#endif
  void snapshotRemainsImmutableAndValidationFailureKeepsIt()
  {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
    qputenv("XDG_STATE_HOME", directory.path().toUtf8());
    Settings::setSettingsFile(directory.filePath("profile.conf"));
    Settings::setValue(Settings::Core::ComputerName, "local");
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Server);
    Settings::setValue(Settings::Security::TlsEnabled, false);
    Settings::setValue(Settings::Server::ClipboardSize, 3);
    ServerConfig configuration;
    configuration.commit();
    Settings::save(false);
    deskflow::gui::CoreProcess core(configuration, QString::fromUtf8(DESKFLOW_TEST_CORE_PATH));
    QVERIFY2(core.prepareConfiguration(), qPrintable(core.configurationError()));
    const auto path = core.runtimeSettingsFile();
    QSettings frozen(path, QSettings::IniFormat);
    QCOMPARE(frozen.value(Settings::Server::ClipboardSize).toInt(), 3);
    Settings::setValue(Settings::Server::ClipboardSize, 42);
    Settings::save(false);
    frozen.sync();
    QCOMPARE(frozen.value(Settings::Server::ClipboardSize).toInt(), 3);
    QFile bad(directory.filePath("bad.conf"));
    QVERIFY(bad.open(QIODevice::WriteOnly));
    bad.write("section: invalid\nend\n");
    bad.close();
    Settings::setValue(Settings::Server::ExternalConfig, true);
    Settings::setValue(Settings::Server::ExternalConfigFile, bad.fileName());
    QVERIFY(!core.prepareConfiguration());
    QCOMPARE(core.runtimeSettingsFile(), path);
    QVERIFY(QFile::exists(path));
    QVERIFY(!core.isStarted());
  }
  void validatesWithoutRunningInputBackend_data()
  {
    QTest::addColumn<QByteArray>("graph");
    QTest::addColumn<bool>("valid");
    QTest::newRow("valid") << QByteArray("section: links\n local:\nend\nsection: options\nend\n") << true;
    QTest::newRow("invalid") << QByteArray("section: invalid\nend\n") << false;
  }
  void validatesWithoutRunningInputBackend()
  {
    QFETCH(QByteArray, graph);
    QFETCH(bool, valid);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile file(directory.filePath("server.conf"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(graph);
    file.close();
    const auto path = directory.filePath("profile.conf");
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue("core/computerName", "local");
    settings.setValue("core/coreMode", 2);
    settings.setValue("core/port", 24800);
    settings.setValue("server/externalConfig", true);
    settings.setValue("server/externalConfigFile", file.fileName());
    settings.setValue("computer_local/name", "local");
    settings.sync();
    QProcess validator;
    validator.start(QString::fromUtf8(DESKFLOW_TEST_CORE_PATH), {"server", "--validate-config", "--settings", path});
    QVERIFY(validator.waitForFinished(10000));
    QCOMPARE(validator.exitStatus(), QProcess::NormalExit);
    QCOMPARE(validator.exitCode() == 0, valid);
    const auto output = validator.readAllStandardOutput();
    QVERIFY2(output.contains(valid ? "\"success\":true" : "\"success\":false"), output.constData());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), graph);
  }
};
QTEST_MAIN(CoreValidationTests)
#include "CoreValidationTests.moc"
