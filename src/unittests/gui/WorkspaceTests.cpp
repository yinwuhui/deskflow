// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "common/Settings.h"
#include "gui/MainWindow.h"
#include "gui/TlsUtility.h"
#include "gui/core/ApplicationState.h"
#include "gui/dialogs/ActionDialog.h"
#include "gui/dialogs/ComputerSettingsDialog.h"
#include "gui/widgets/KeySequenceWidget.h"
#include "gui/widgets/LogWidget.h"
#include "gui/workspace/DeviceWorkspace.h"
#include "gui/workspace/SettingsWorkspace.h"
#include "gui/workspace/SetupAssistant.h"
#include "gui/workspace/WorkspaceUi.h"
#include "net/FingerprintDatabase.h"
#include "net/PeerIdentityStore.h"
#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QtTest>
class WorkspaceTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void persistedPauseDoesNotClaimAStoppedCoreIsRunning()
  {
    QTemporaryDir directory;
    Settings::setSettingsFile(directory.filePath("profile.conf"));
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Server);
    Settings::setValue(Settings::Core::SharingPaused, true);
    Settings::setValue(Settings::Gui::AutoStartCore, false);
    Settings::save(false);
    MainWindow window;
    auto *status = window.findChild<QLabel *>("workspaceStatus");
    QVERIFY(status);
    QVERIFY(status->text().contains("已停止"));
    QVERIFY(!status->text().contains("已暂停共享"));
    QVERIFY(Settings::value(Settings::Core::SharingPaused).toBool());
  }
  void changingAnotherSectionCannotUnlockManagedSharingFields()
  {
    QTemporaryDir directory;
    Settings::setSettingsFile(directory.filePath("profile.conf"));
    Settings::setValue(Settings::Server::EnableSwitchDelay, true);
    Settings::setValue(Settings::Server::SwitchDelay, 250);
    Settings::setValue(Settings::Log::ToFile, false);
    Settings::save(false);
    deskflow::gui::workspace::SettingsWorkspace settings;
    auto *delay = settings.findChild<QWidget *>("preference_" + Settings::Server::SwitchDelay);
    auto *logFile = settings.findChild<QWidget *>("preference_" + Settings::Log::File);
    QVERIFY(delay);
    QVERIFY(logFile);
    settings.setManagedExternally(false, true);
    QVERIFY(!delay->isEnabled());
    settings.setDraftValue(Settings::Log::ToFile, true);
    QVERIFY(logFile->isEnabled());
    QVERIFY(!delay->isEnabled());
    settings.setManagedExternally(true, false);
    settings.setDraftValue(Settings::Log::ToFile, false);
    QVERIFY(!logFile->isEnabled());
    QVERIFY(!delay->isEnabled());
    settings.setManagedExternally(false, false);
    QVERIFY(delay->isEnabled());
    QCOMPARE(settings.draftValue(Settings::Server::SwitchDelay).toInt(), 250);
  }
  void formSwitchIsKeyboardAccessibleAndDescribed()
  {
    using namespace deskflow::gui::workspace;
    auto *toggle = new Switch;
    std::unique_ptr<QWidget> field(row("共享剪贴板", "仅在共享已连接并允许剪贴板时生效", toggle));
    field->show();
    toggle->setFocus();
    auto *accessible = QAccessible::queryAccessibleInterface(toggle);
    QVERIFY(accessible);
    QCOMPARE(accessible->role(), QAccessible::CheckBox);
    QCOMPARE(accessible->text(QAccessible::Name), QString("共享剪贴板"));
    QVERIFY(!accessible->text(QAccessible::Description).isEmpty());
    QVERIFY(!accessible->state().checked);
    QTest::keyClick(toggle, Qt::Key_Space);
    QVERIFY(accessible->state().checked);
    QTest::keyClick(toggle, Qt::Key_Space);
    QVERIFY(!accessible->state().checked);
    toggle->setEnabled(false);
    QTest::keyClick(toggle, Qt::Key_Space);
    QVERIFY(!toggle->isChecked());
    QVERIFY(accessible->state().disabled);
  }
  void identityApprovalDoesNotOverwriteSynchronousConnection()
  {
    using Assistant = deskflow::gui::workspace::SetupAssistant;
    Assistant assistant;
    assistant.begin(false, false, {}, "192.0.2.10", 24800);
    const Fingerprint fingerprint{QCryptographicHash::Sha256, QByteArray(32, 'a')};
    int decisions = 0;
    assistant.requestIdentity(fingerprint, fingerprint, [&](bool accepted) {
      ++decisions;
      if (accepted)
        assistant.observeConnected("192.0.2.10");
      return accepted;
    });
    auto *confirm = assistant.findChild<QPushButton *>("confirmPeerIdentity");
    auto *ack = assistant.findChild<QCheckBox *>("setupIdentityAcknowledged");
    QVERIFY(confirm);
    QVERIFY(ack);
    QVERIFY(!confirm->isEnabled());
    ack->setChecked(true);
    confirm->click();
    QCOMPARE(decisions, 1);
    QCOMPARE(assistant.step(), Assistant::Step::Position);
    assistant.observeDisconnected("192.0.2.10");
    assistant.cancel();
    QCOMPARE(decisions, 1);
  }
  void addingDoesNotMistakeAnExistingPeerForTheNewDevice()
  {
    using Assistant = deskflow::gui::workspace::SetupAssistant;
    Assistant assistant;
    assistant.begin(true, false, {"existing"}, {}, 24800);
    const Fingerprint fp{QCryptographicHash::Sha256, QByteArray(32, 'b')};
    assistant.requestIdentity(fp, fp, [](bool accepted) { return accepted; });
    assistant.findChild<QCheckBox *>("setupIdentityAcknowledged")->setChecked(true);
    assistant.findChild<QPushButton *>("confirmPeerIdentity")->click();
    assistant.observeConnected("existing");
    QCOMPARE(assistant.step(), Assistant::Step::AwaitPeer);
    assistant.observeConnected("new-peer");
    QCOMPARE(assistant.step(), Assistant::Step::Position);
    assistant.cancel();
    QVERIFY(!assistant.active());
  }
  void anotherPeerCannotReplaceTheDeviceBeingVerified()
  {
    using Assistant = deskflow::gui::workspace::SetupAssistant;
    Assistant assistant;
    assistant.begin(true, false, {"existing"}, {}, 24800);
    QVERIFY(assistant.showIncomingDevice("new-peer"));
    QCOMPARE(assistant.step(), Assistant::Step::RegisterDevice);
    assistant.waitForPeer();
    assistant.observeConnected("new-peer");
    QCOMPARE(assistant.step(), Assistant::Step::Position);
    QPushButton *next = nullptr;
    for (auto *button : assistant.findChildren<QPushButton *>())
      if (button->text() == tr("继续验证输入"))
        next = button;
    QVERIFY(next);
    next->click();
    QCOMPARE(assistant.step(), Assistant::Step::Verify);
    assistant.observeConnected("another-peer");
    QCOMPARE(assistant.step(), Assistant::Step::Verify);
    QSignalSpy finished(&assistant, &Assistant::finished);
    auto *ack = assistant.findChild<QCheckBox *>("setupInputAcknowledged");
    QVERIFY(ack);
    ack->setChecked(true);
    QPushButton *verify = nullptr;
    for (auto *button : assistant.findChildren<QPushButton *>())
      if (button->text() == tr("记录验证并完成"))
        verify = button;
    QVERIFY(verify);
    verify->click();
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().at(0).toString(), QString("new-peer"));
  }
  void staleVerificationCannotApproveAReconnectedSession()
  {
    using Assistant = deskflow::gui::workspace::SetupAssistant;
    Assistant assistant;
    assistant.begin(true, false, {}, {}, 24800);
    QVERIFY(assistant.showIncomingDevice("peer"));
    assistant.waitForPeer();
    assistant.observeConnected("peer");
    QPushButton *next = nullptr;
    for (auto *button : assistant.findChildren<QPushButton *>())
      if (button->text() == tr("继续验证输入"))
        next = button;
    QVERIFY(next);
    next->click();
    assistant.findChild<QCheckBox *>("setupInputAcknowledged")->setChecked(true);
    QPushButton *oldVerify = nullptr;
    for (auto *button : assistant.findChildren<QPushButton *>())
      if (button->text() == tr("记录验证并完成"))
        oldVerify = button;
    QVERIFY(oldVerify);
    QSignalSpy finished(&assistant, &Assistant::finished);
    assistant.observeDisconnected("peer");
    assistant.observeConnected("peer");
    oldVerify->click();
    QCOMPARE(finished.count(), 0);
    QCOMPARE(assistant.step(), Assistant::Step::Position);
  }
  void identityWriteFailureDoesNotStartAConnectionRetry()
  {
    using Assistant = deskflow::gui::workspace::SetupAssistant;
    Assistant assistant;
    assistant.begin(false, false, {}, "192.0.2.10", 24800);
    const Fingerprint fp{QCryptographicHash::Sha256, QByteArray(32, 'a')};
    int decisions = 0;
    QSignalSpy connections(&assistant, &Assistant::connectRequested);
    assistant.requestIdentity(fp, fp, [&](bool accepted) {
      if (!accepted)
        return false;
      return ++decisions > 1;
    });
    assistant.findChild<QCheckBox *>("setupIdentityAcknowledged")->setChecked(true);
    assistant.findChild<QPushButton *>("confirmPeerIdentity")->click();
    QCOMPARE(assistant.step(), Assistant::Step::Identity);
    QVERIFY(assistant.identityPending());
    QCOMPARE(connections.count(), 0);
    QTest::qWait(10);
    assistant.findChild<QCheckBox *>("setupIdentityAcknowledged")->setChecked(true);
    assistant.findChild<QPushButton *>("confirmPeerIdentity")->click();
    QCOMPARE(decisions, 2);
    QCOMPARE(assistant.step(), Assistant::Step::AwaitPeer);
    QCOMPARE(connections.count(), 0);
  }
  void settingsSaveOnlyTheSelectedScope()
  {
    QTemporaryDir directory;
    Settings::setSettingsFile(directory.filePath("scope.conf"));
    Settings::setValue(Settings::Server::ClipboardSize, 10);
    Settings::setValue(Settings::Client::XScrollScale, 1.0);
    deskflow::gui::workspace::SettingsWorkspace settings;
    settings.setDraftValue(Settings::Server::ClipboardSize, 20);
    settings.setDraftValue(Settings::Client::XScrollScale, 2.0);
    QVERIFY(settings.hasUnsavedChanges());
    QVERIFY(settings.saveSection("receiver"));
    QCOMPARE(Settings::value(Settings::Server::ClipboardSize).toInt(), 10);
    QCOMPARE(Settings::value(Settings::Client::XScrollScale).toDouble(), 2.0);
    QCOMPARE(settings.draftValue(Settings::Server::ClipboardSize).toInt(), 20);
    QVERIFY(settings.hasUnsavedChanges());
    settings.reloadSaved();
    QVERIFY(!settings.hasUnsavedChanges());
  }
  void concurrentSettingsEditsCannotSilentlyOverwriteSavedValues()
  {
    QTemporaryDir directory;
    Settings::setSettingsFile(directory.filePath("concurrent.conf"));
    Settings::setValue(Settings::Server::ClipboardSize, 10);
    deskflow::gui::workspace::SettingsWorkspace first, second;
    first.setDraftValue(Settings::Server::ClipboardSize, 20);
    second.setDraftValue(Settings::Server::ClipboardSize, 30);
    QVERIFY(first.saveSection("sharing"));
    QVERIFY(!second.saveSection("sharing"));
    QCOMPARE(Settings::value(Settings::Server::ClipboardSize).toInt(), 20);
    QCOMPARE(second.draftValue(Settings::Server::ClipboardSize).toInt(), 30);
  }
  void identityGenerationKeepsTheEventLoopAvailable()
  {
    QTemporaryDir directory;
    Settings::setSettingsFile(directory.filePath("identity.conf"));
    Settings::setValue(Settings::Core::ComputerName, "local");
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Server);
    Settings::setValue(Settings::Security::TlsEnabled, false);
    Settings::setValue(Settings::Security::Certificate, directory.filePath("identity.pem"));
    Settings::save(false);
    MainWindow window("deskflow-identity-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    auto *settings = window.findChild<deskflow::gui::workspace::SettingsWorkspace *>();
    QVERIFY(settings);
    Q_EMIT settings->requestIdentityGeneration(2048);
    bool eventDelivered = false;
    QTimer::singleShot(0, &window, [&] { eventDelivered = true; });
    QTRY_VERIFY_WITH_TIMEOUT(eventDelivered, 1000);
    auto *page = settings->findChild<QWidget *>("settingsPage_certificates");
    QVERIFY(page);
    QTRY_VERIFY_WITH_TIMEOUT(page->isEnabled(), 15000);
    QVERIFY(deskflow::gui::TlsUtility::isCertValid(directory.filePath("identity.pem")));
  }
  void recordingCanStartFromKeyboardAndCancelWithoutLosingPreviousValue()
  {
    KeySequenceWidget record(nullptr);
    record.show();
    record.setFocus();
    QSignalSpy changed(&record, &KeySequenceWidget::keySequenceChanged);
    QTest::keyClick(&record, Qt::Key_Space);
    QTest::keyClick(&record, Qt::Key_K, Qt::ControlModifier);
    QVERIFY(record.valid());
    const auto previous = record.keySequence();
    QCOMPARE(changed.count(), 1);
    QTest::keyClick(&record, Qt::Key_Space);
    record.cancelRecording();
    QCOMPARE(record.keySequence(), previous);
    QCOMPARE(changed.count(), 1);
  }
  void movingBeyondTheTopLeftPreservesSavedLayoutAndIdentity()
  {
    QTemporaryDir directory;
    Settings::setSettingsFile(directory.filePath("layout.conf"));
    Settings::setValue(Settings::Core::ComputerName, "local");
    Settings::setValue(Settings::Server::GridWidth, 3);
    Settings::setValue(Settings::Server::GridHeight, 3);
    auto &profile = Settings::proxy().get();
    profile.beginWriteArray("internalConfig/screens", 9);
    profile.setArrayIndex(4);
    profile.setValue("name", "local");
    profile.setArrayIndex(5);
    profile.setValue("name", "remote");
    profile.endArray();
    ServerConfig saved;
    const auto original = saved;
    const auto identity = std::as_const(saved).computers()[5].stableId();
    deskflow::gui::workspace::DeviceWorkspace devices(saved, [] { return true; });
    auto *selection = devices.findChild<QComboBox *>();
    QVERIFY(selection);
    selection->setCurrentIndex(selection->findData(identity));
    devices.beginEditing();
    QVERIFY(devices.moveSelectedTo(-2, 0));
    QCOMPARE(devices.draftLayout().gridWidth(), 4);
    QCOMPARE(saved, original);
    QVERIFY(devices.hasUnsavedChanges());
    QVERIFY(!devices.moveSelectedTo(100, 0));
    QCOMPARE(devices.draftLayout().gridWidth(), 4);
    devices.discardLayout();
    QVERIFY(!devices.hasUnsavedChanges());
    QCOMPARE(std::as_const(devices.draftLayout()).computers()[5].stableId(), identity);
  }
  void sharedLogsAreBoundedAndWrittenOnce()
  {
    LogWidget writer;
    LogWidget mirror(nullptr, writer.document());
    writer.appendLine("one entry");
    QCOMPARE(writer.document()->toPlainText(), mirror.document()->toPlainText());
    QCOMPARE(writer.document()->toPlainText().count("one entry"), 1);
    for (int i = 0; i < 300; ++i)
      writer.appendLine(QString(16384, 'x'));
    QVERIFY(writer.document()->characterCount() <= 2 * 1024 * 1024 + 1);
    QCOMPARE(writer.document(), mirror.document());
  }
  void navigationDoesNotChangeRoleOrProfile()
  {
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    qputenv("XDG_CONFIG_HOME", QDir::tempPath().toUtf8() + "/deskflow-unit-tests");
    qputenv("XDG_STATE_HOME", QDir::tempPath().toUtf8() + "/deskflow-unit-test-state");
    Settings::setSettingsFile(profile.filePath("Deskflow.conf"));
    Settings::setValue(Settings::Core::ComputerName, "local-test");
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Server);
    Settings::setValue(Settings::Security::TlsEnabled, false);
    Settings::setValue(Settings::Server::GridWidth, 5);
    Settings::setValue(Settings::Server::GridHeight, 3);
    auto &fixture = Settings::proxy().get();
    fixture.beginWriteArray("internalConfig/screens", 15);
    for (const auto &entry : QList<QPair<int, QString>>{
             {2, "test-upper"},
             {6, "test-left"},
             {7, "local-test"},
             {8, "test-right"},
             {12, "test-lower"},
             {13, "test-extra"}
         }) {
      fixture.setArrayIndex(entry.first);
      fixture.setValue("name", entry.second);
    }
    fixture.endArray();
    Settings::setValue(Settings::Gui::LogExpanded, false);
    ServerConfig fixtureConfiguration;
    fixtureConfiguration.commit();
    Settings::save(false);
    const Fingerprint trustedFixture{QCryptographicHash::Sha256, QByteArray(32, char(0xaa))};
    QVERIFY(QDir().mkpath(Settings::tlsDir()));
    FingerprintDatabase trust;
    trust.addTrusted(trustedFixture);
    QVERIFY(trust.write(Settings::tlsTrustedClientsDb()));
    PeerIdentityStore identities(Settings::peerIdentityDb());
    QVERIFY(identities.accept(
        {"client",
         std::as_const(fixtureConfiguration).computers()[8].stableId(),
         "test-right",
         {},
         trustedFixture.data.toHex()},
        {}
    ));
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Light);
    QFile file(Settings::settingsFile());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto before = file.readAll();
    file.close();
    MainWindow window("deskflow-workspace-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    window.show();
    QTest::qWait(100);
    if (auto *address = window.findChild<QLabel *>("lblIpAddresses"))
      address->setText("Suggested IP: 192.0.2.10");
    auto *navigation = window.findChild<deskflow::gui::workspace::Navigation *>("workspaceNavigation");
    QVERIFY(navigation);
    auto *pages = window.findChild<QStackedWidget *>("workspacePages");
    QVERIFY(pages);
    QCOMPARE(pages->count(), 4);
    for (int row = 0; row < 4; ++row) {
      navigation->setCurrentRow(row);
      QCOMPARE(pages->currentIndex(), row);
      QCOMPARE(Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>(), Settings::CoreMode::Server);
    }
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), before);
    file.close();
    navigation->setCurrentRow(0);
    QTest::qWait(50);
    QVERIFY(window.grab().save("/tmp/deskflow-workspace-devices.png"));
    navigation->setCurrentRow(1);
    QTest::qWait(50);
    QVERIFY(window.grab().save("/tmp/deskflow-workspace-settings.png"));
    auto *settings = window.findChild<deskflow::gui::workspace::SettingsWorkspace *>();
    QVERIFY(settings);
    for (const auto &section :
         QStringList{"sharing", "rules", "receiver", "network", "certificates", "config", "maintenance", "logs"}) {
      settings->navigate(section);
      QTest::qWait(20);
      QVERIFY(window.grab().save("/tmp/deskflow-workspace-" + section + ".png"));
    }
    navigation->setCurrentRow(2);
    QTest::qWait(20);
    QVERIFY(window.grab().save("/tmp/deskflow-workspace-diagnostics.png"));
    navigation->setCurrentRow(3);
    QTest::qWait(20);
    QVERIFY(window.grab().save("/tmp/deskflow-workspace-help.png"));
    navigation->setCurrentRow(1);
    settings->navigate("trust");
    QTest::qWait(30);
    QVERIFY(window.grab().save("/tmp/deskflow-workspace-trust.png"));
    settings->navigate("general");
    window.resize(800, 600);
    QTest::qWait(50);
    QVERIFY(window.grab().save("/tmp/deskflow-workspace-narrow.png"));
    window.resize(1100, 780);
    const auto originalPalette = qApp->palette();
    auto darkPalette = originalPalette;
    darkPalette.setColor(QPalette::Window, QColor("#21252B"));
    qApp->setPalette(darkPalette);
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    deskflow::gui::workspace::applyTheme(window.centralWidget());
    QVERIFY(deskflow::gui::workspace::isDark());
    QTest::qWait(100);
    QVERIFY(window.grab().save("/tmp/deskflow-workspace-dark.png"));
    navigation->setCurrentRow(0);
    QTest::qWait(30);
    QVERIFY(window.grab().save("/tmp/deskflow-workspace-devices-dark.png"));
    navigation->setCurrentRow(1);
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Light);
    qApp->setPalette(originalPalette);
    deskflow::gui::workspace::applyTheme(window.centralWidget());
    Computer computer("test-peer");
    ComputerSettingsDialog editor(&window, &computer);
    editor.show();
    QTest::qWait(50);
    QVERIFY(editor.grab().save("/tmp/deskflow-device-details.png"));
    auto *detailPages = editor.findChild<QStackedWidget *>("deviceDetailPages");
    QVERIFY(detailPages);
    QCOMPARE(detailPages->count(), 4);
    editor.reject();
    Action action;
    Hotkey hotkey;
    ActionDialog actionEditor(&window, fixtureConfiguration, hotkey, action);
    actionEditor.show();
    QTest::qWait(30);
    QVERIFY(actionEditor.grab().save("/tmp/deskflow-action-editor.png"));
    actionEditor.reject();
    auto *assistant = window.findChild<deskflow::gui::workspace::SetupAssistant *>("setupAssistant");
    QVERIFY(assistant);
    assistant->begin(false, false, {}, "192.0.2.10", 24800);
    const Fingerprint fp{QCryptographicHash::Sha256, QByteArray(32, 'a')};
    assistant->requestIdentity(fp, fp, [](bool) { return false; });
    assistant->setIdentityContext("test-peer", QByteArray(64, 'b'));
    navigation->setCurrentRow(0);
    QTest::qWait(50);
    QVERIFY(window.grab().save("/tmp/deskflow-workspace-identity.png"));
    assistant->cancel();
  }
};
QTEST_MAIN(WorkspaceTests)
#include "WorkspaceTests.moc"
