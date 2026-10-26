// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "gui/core/PeerUsageModel.h"
#include "gui/widgets/DisplayRequestPanel.h"
#include "gui/widgets/PeerUsageControl.h"
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>
#ifdef DESKFLOW_DISPLAY
#include "display/DisplayService.h"
#endif
class PeerUsageControlTests : public QObject
{
  Q_OBJECT
  const QString peer = QString(64, 'a');
  QJsonObject state(int revision, QString effective = "input", QString usage = "InputActive", QString target = "none")
  {
    return {
        {"peerId", peer},
        {"baseConnectionId",QString(32,'c')},{"baseConnectionGeneration","1"},{"baseConnected",true},
        {"revision", QString::number(revision)},
        {"effectiveMode", effective},
        {"usageState", usage},
        {"targetMode", target},
        {"reasonCode", "None"},
        {"operationId", QString(32, 'b')},
        {"operationGeneration", "7"},
        {"switchStage", usage == "Switching" ? "AwaitingPermission" : "None"},
        {"inputCapabilities", QJsonObject{{"input", "supported"}, {"extended", "supported"}}},
        {"availableActions", usage == "Switching" ? QJsonArray{"cancel", "stop"} : QJsonArray{"switch", "stop"}}
    };
  }
  void bind(PeerUsageModel &model, QSignalSpy &commands)
  {
    model.setConnectedDevices({"computer:work"});
    model.bindAuthenticatedDevice("computer:work", peer);
    auto subscribe = commands.last()[0].toJsonObject();
    model.receiveReply({{"id", subscribe["id"]}, {"accepted", true}, {"snapshot", state(1)}});
  }
private Q_SLOTS:
  void incomingConsentStaysInsideMainWindow()
  {
#ifdef DESKFLOW_DISPLAY
    using namespace deskflow::display;
    auto service = DisplayService::applicationService();
    Capabilities caps;
    caps.send = caps.receive = Availability::Supported;
    caps.sessionAvailable = true;
    service->updateCapabilities(caps);
    QWidget mainWindow;
    mainWindow.resize(1000, 700);
    DisplayRequestPanel panel(&mainWindow);
    QVERIFY(panel.isHidden());
    SessionId session{}, request{};
    session[0] = 1;
    request[0] = 2;
    QVERIFY(service->incomingAuthenticated(Peer{peer.toStdString(), "work", caps}, session, request).accepted);
    QVERIFY(!panel.isHidden());
    QCOMPARE(panel.parentWidget(), &mainWindow);
    QVERIFY(!panel.isWindow());
    panel.findChild<QPushButton *>("allowDisplayRequest")->click();
    QTRY_VERIFY(panel.isHidden());
    QCOMPARE(service->snapshot().value("firstFramePresented").toBool(), false);
    service->stopManagedSession(service->currentKey());
#endif
  }
  void reconnectKeepsDirtyLayoutGuard()
  {
    PeerUsageModel model;QSignalSpy commands(&model,&PeerUsageModel::commandRequested);bind(model,commands);
    auto dirty=state(3,"extended","CleanupRequired");dirty.insert("availableActions",QJsonArray{"retryCleanup"});model.receiveSnapshot(dirty);
    QVERIFY(model.displayLayoutBlocked());model.retryCleanup("computer:work");
    const auto cleanup=commands.last()[0].toJsonObject();QCOMPARE(cleanup.value("operation").toString(),QString("retryPeerUsageCleanup"));
    QCOMPARE(cleanup.value("args").toObject().value("baseConnectionId").toString(),QString(32,'c'));
    model.receiveConnection({{"peerId",peer},{"baseConnected",true},{"baseConnectionId",QString(32,'d')},{"baseConnectionGeneration","2"},{"deviceKeys",QJsonArray{"computer:work"}}});
    QVERIFY(model.snapshot("computer:work").isEmpty());QVERIFY(model.displayLayoutBlocked());
    auto current=state(4,"none","Inactive");current.insert("baseConnectionId",QString(32,'d'));current.insert("baseConnectionGeneration","2");model.receiveSnapshot(current);
    QVERIFY(!model.displayLayoutBlocked());
  }
  void unavailableDevicesCannotIssueCommands()
  {
    PeerUsageModel model;
    QSignalSpy commands(&model, &PeerUsageModel::commandRequested);
    PeerUsageControl control(nullptr, &model);
    control.setDevice("computer:work");
    QVERIFY(!control.findChild<QPushButton *>("usageExtended")->isEnabled());
    model.setConnectedDevices({"computer:work"});
    model.bindAuthenticatedDevice("computer:work", "work"); // A name is not an identity.
    model.choose("computer:work", "extended");
    QCOMPARE(commands.count(), 0);
  }
  void pagesSharePendingAndConfirmedState()
  {
    PeerUsageModel model;
    QSignalSpy commands(&model, &PeerUsageModel::commandRequested);
    PeerUsageControl devices(nullptr, &model), arrangement(nullptr, &model);
    for (auto control : {&devices, &arrangement})
      control->setDevice("computer:work");
    bind(model, commands);
    auto button = devices.findChild<QPushButton *>("usageExtended");
    QVERIFY(button->isEnabled());
    button->click();
    QCOMPARE(commands.count(), 2);
    auto command = commands.last()[0].toJsonObject();
    QCOMPARE(command["operation"].toString(), QString("switchPeerUsageMode"));
    auto args = command["args"].toObject();
    QCOMPARE(args["targetMode"].toString(), QString("extended"));
    QCOMPARE(args["peerId"].toString(), peer);
    QVERIFY(!args.contains("shareKeyboard"));
    QVERIFY(!arrangement.findChild<QPushButton *>("usageExtended")->isEnabled());
    QCOMPARE(model.snapshot("computer:work")["effectiveMode"].toString(), QString("input"));
    auto pending = state(2, "input", "Switching", "extended");
    model.receiveReply({{"id", command["id"]}, {"accepted", true}, {"snapshot", pending}});
    for (auto control : {&devices, &arrangement}) {
      QVERIFY(control->findChild<QPushButton *>("usageExtended")->isChecked());
      QVERIFY(!control->findChild<QPushButton *>("usageCancel")->isHidden());
      QVERIFY(control->findChild<QLabel *>("usageStatus")->text().contains("permission"));
    }
    arrangement.findChild<QPushButton *>("usageCancel")->click();
    auto cancel = commands.last()[0].toJsonObject();
    QCOMPARE(cancel["operation"].toString(), QString("cancelPeerUsageOperation"));
    QCOMPARE(cancel["args"].toObject()["operationGeneration"].toString(), QString("7"));
    model.receiveReply({{"id", cancel["id"]}, {"accepted", true}, {"snapshot", state(3)}});
    QVERIFY(devices.findChild<QPushButton *>("usageInput")->isChecked());
    model.receiveSnapshot(state(4, "extended", "DisplayActive"));
    QVERIFY(!devices.findChild<QPushButton *>("usageStop")->isHidden());
    QVERIFY(arrangement.findChild<QPushButton *>("usageExtended")->isChecked());
    model.receiveSnapshot(state(2)); // Delayed input event cannot overwrite streaming.
    QVERIFY(arrangement.findChild<QPushButton *>("usageExtended")->isChecked());
  }
  void disconnectInvalidatesIdentityAndLateReply()
  {
    PeerUsageModel model;
    QSignalSpy commands(&model, &PeerUsageModel::commandRequested);
    bind(model, commands);
    model.choose("computer:work", "extended");
    auto command = commands.last()[0].toJsonObject();
    model.setConnectedDevices({});
    model.setConnectedDevices({"computer:work"});
    model.receiveReply({{"id", command["id"]}, {"accepted", true}, {"snapshot", state(5, "extended", "DisplayActive")}}
    );
    QVERIFY(model.snapshot("computer:work").isEmpty());
    model.choose("computer:work", "extended");
    QCOMPARE(commands.count(), 2);
  }
};
QTEST_MAIN(PeerUsageControlTests)
#include "PeerUsageControlTests.moc"
