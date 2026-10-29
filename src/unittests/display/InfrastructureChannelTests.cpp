// SPDX-License-Identifier: MIT
#include "display/InfrastructureControlChannel.h"
#include "display/UsageDisplayBridge.h"
#include "display/control/ControlBodyRegistry.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLocalServer>
#include <QTemporaryDir>
#include <QThread>
#include <QScopeGuard>
#include <iostream>
#include <stdexcept>
using namespace deskflow::display;
using namespace deskflow::display::infra;
static void require(bool value, const char *message)
{
  if (!value)
    throw std::runtime_error(message);
}
static void until(const std::function<bool()> &ready)
{
  QElapsedTimer clock;
  clock.start();
  while (!ready() && clock.elapsed() < 3000) {
    QCoreApplication::processEvents();
    QThread::msleep(1);
  }
  require(ready(), "core IPC fixture deadline");
}
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir directory;
    PeerUsageService usage; // Keep synthetic input pause state out of product preferences.
    const Capabilities supported{Availability::Supported,Availability::Supported,false,false,true,1};
    DisplayService display(supported, directory.filePath("display.ini"));
    UsageDisplayBridge bridge(&usage,&display);
    InputRouteAdapter input(&usage, &display);
    const QString local(64, 'a'), peer(64, 'b'), connection(32, 'c');
    QLocalServer server;
    require(server.listen(directory.filePath("core")), "listen");
    auto core=std::make_unique<CoreInputChannel>(&input, &usage, nullptr, server.fullServerName());
    InfrastructureControlChannel channel(local, core.get(), &usage);
    ControlBinding remote(peer, local, "remote.connection");
    QLocalSocket *socket = nullptr;
    QByteArray buffer;
    int ready = 0, lost = 0, received = 0;
    QList<QJsonObject> sent;
    uint32_t inputSequence=0;
    bool suspended=true,inputAck=false,inputPermission=true;
    unsigned inputMode=1;
    QObject::connect(
        &channel, &InfrastructureControlChannel::bindingReady,
        [&](const QString &, const QString &, const QString &id) {
          require(id == remote.id(), "transcript differs");
          ++ready;
        }
    );
    QObject::connect(&channel, &InfrastructureControlChannel::bindingLost, [&] { ++lost; });
    QObject::connect(&channel, &InfrastructureControlChannel::envelopeReceived, [&] { ++received; });
    auto event = [&](const QJsonObject &body, const QString &id = QString()) {
      QJsonObject value{{"peerId", peer}, {"baseConnectionId", id.isEmpty() ? connection : id}, {"body", body}};
      socket->write("peerUsageControlEvent=" + QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n');
    };
    QObject::connect(&server, &QLocalServer::newConnection, [&] {
      socket = server.nextPendingConnection();
      QObject::connect(socket, &QLocalSocket::readyRead, [&] {
        buffer += socket->readAll();
        while (buffer.contains('\n')) {
          auto line = buffer.left(buffer.indexOf('\n'));
          buffer.remove(0, line.size() + 1);
          if (line.startsWith("hello=")) {
            socket->write(line + '\n');
            QJsonObject record{
                {"peerId", peer},
                {"baseConnectionId", connection},
                {"baseConnected", true},
                {"inputControlSupported", true},
                {"inputActive", false},
                {"inputPermissionsReady", true},
                {"originalInputRole", "source"},
                {"displayNetworkPolicy", "networkAllowed"}
            };
            socket->write("peerUsageConnection=" + QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n');
          } else if (line.startsWith("inputControl=")) {
            const auto request = QJsonDocument::fromJson(line.mid(13)).object();
            const auto body = request.value("body").toObject();
            if (body.value("protocol") == "display-infra-bind/1") {
              ControlError error;
              require(
                  remote.acceptHello(
                      QJsonDocument(body).toJson(QJsonDocument::Compact), local, "remote.connection", error
                  ),
                  "remote hello"
              );
              event(QJsonDocument::fromJson(remote.hello()).object());
            } else if (body.value("protocol") == "display-infra/1")
              sent.append(body);
            else if(!request.value("kind").toString().isEmpty()) {
              auto response=request;const auto kind=request.value("kind").toString();
              const auto sequence=request.value("sequence").toString().toUInt();
              if(kind=="probe"){inputSequence=sequence;inputAck=true;inputPermission=true;inputMode=3;}
              else if(kind=="suspend"){inputSequence=sequence;suspended=true;inputAck=true;inputPermission=false;inputMode=1;}
              else if(kind=="resume"){inputSequence=sequence;inputAck=true;inputPermission=false;inputMode=2;}
              else if(kind=="commitResume")suspended=false;
              response.insert("accepted",true);response.insert("connected",true);response.insert("supported",true);
              response.insert("suspended",suspended);response.insert("localFocus",true);
              response.insert("acknowledgementReceived",sequence==inputSequence&&inputAck);
              response.insert("acknowledged",sequence==inputSequence&&inputMode==(request.value("enabled").toBool()?2U:1U));
              response.insert("inputPermissionsReady",sequence==inputSequence&&inputPermission);
              response.insert("otherRoutesIsolated",true);response.insert("lastSequence",QString::number(inputSequence));
              socket->write("inputControlResult="+QJsonDocument(response).toJson(QJsonDocument::Compact)+'\n');
            }
          }
        }
      });
    });
    auto disconnectFixture = qScopeGuard([&] {
      // Callbacks capture local fixture state. Disconnect before stack unwind,
      // including assertion failures, rather than invoking dangling captures.
      server.disconnect();
      if(socket)socket->disconnect();
    });
    require(!channel.lease(connection), "unbound lease minted");
    until([&] { return ready == 1; });
    const auto lease = channel.lease(connection);
    require(
        lease && lease->active() && lease->localIdentity() == local && lease->peerIdentity() == peer &&
            lease->connection() == connection && lease->binding() == remote.id(),
        "authenticated lease scope"
    );
    Envelope stop{
        "Stop",
        QString(32, 'd'),
        QString(32, 'e'),
        QString(64, 'f'),
        1,
        0,
        {{"operationId", QString(32, '1')}, {"reason", "Cancelled"}, {"lastEpoch", 1}, {"lastSequence", "0"}}
    };
    ControlError error;
    const auto wire = remote.encode(stop, error);
    require(!wire.isEmpty(), "remote stop encode");
    event(QJsonDocument::fromJson(wire).object(), QString(32, '2'));
    QCoreApplication::processEvents();
    require(received == 0, "wrong connection admitted");
    event(QJsonDocument::fromJson(wire).object());
    until([&] { return received == 1; });
    event(QJsonDocument::fromJson(wire).object());
    WorkerMediaSignal offer;
    offer.description = QString(20000, 'x');
    Envelope huge = stop;
    huge.type = "MediaSignal";
    huge.body = encodeWebRtcSignalBody({1, offer}, error);
    require(!huge.body.isEmpty(), "oversized fixture malformed");
    require(!channel.send(connection, huge), "wrapped core budget bypass");
    require(channel.send(connection, stop), "valid control send");
    until([&] { return sent.size() == 1; });
    require(sent.front().value("sequence") == "1", "rejected send consumed sequence");
    require(received == 1, "replay admitted");
    // Drive the existing product usage transaction through the core socket,
    // including permission and release/suspend acknowledgements. The local
    // fixture represents core observations; it is not real device/TLS acceptance.
    PeerUsageService::BaseConnection base;
    base.id=connection.toStdString();base.generation=1;base.connected=true;
    base.role=PeerUsageService::InputRole::Source;base.permission=Permission::Granted;base.focusOwner="local";
    const UsageCapabilities usageCaps{Availability::Supported,Availability::Supported,Availability::Supported,
      Availability::Supported,Availability::Supported,Availability::Supported,Availability::Supported,true};
    require(usage.syncConnectedPeer({peer.toStdString(),"Ubuntu",supported},usageCaps,base).accepted,"usage binding");
    display.authenticatedPeer({peer.toStdString(),"Ubuntu",supported});
    usage.updateDisplayCapabilities(peer,Availability::Supported,Availability::Supported,true);
    auto command=[&](QString operation,QString request,QJsonObject args){
      args.insert("peerId",peer);args.insert("operationId",request);args.insert("baseConnectionId",connection);
      args.insert("baseConnectionGeneration","1");
      usage.execute({{"id","native-auth"},{"operation",operation},{"args",args}});
    };
    command("startPeerUsage",QString(32,'4'),{{"mode","input"}});
    until([&]{return usage.snapshot(peer).value("effectiveMode")=="input";});
    command("switchPeerUsageMode",QString(32,'5'),{{"targetMode","extended"}});
    until([&]{return display.snapshot().value("state")=="Connecting";});
    const auto displayKey=display.currentKey();
    require(!display.authorizationValid(displayKey),"unauthorized session admitted");
    display.peerResponse(displayKey,true,Reason::None);
    require(!display.authorizationValid(displayKey),"missing OS permission admitted");
    display.permissionResult(displayKey,Permission::Granted);
    std::optional<InputIsolationProof> isolation;
    until([&]{
      const auto snapshot=usage.snapshot(peer);UsageKey key;
      const auto bytes=QByteArray::fromHex(snapshot.value("operationId").toString().toLatin1());
      if(bytes.size()!=16)return false;
      std::copy(bytes.begin(),bytes.end(),key.operation.begin());
      key.generation=snapshot.value("operationGeneration").toString().toULongLong();
      isolation=input.isolationProof(peer,key);return isolation.has_value();
    });
    const auto authorization=bridge.nativeAuthorization(*lease,*isolation);
    require(authorization&&authorization->active()&&authorization->role()==Role::Host&&
      authorization->displayKey()==displayKey,"verified facts did not authorize native owner");
    require(bridge.nativeAuthorization(*lease,*isolation)->active(),"same scope grant not reusable");
    usage.invalidateRollback(peer);
    require(display.stopManagedSession(displayKey,Reason::UserStopped).accepted,"native grant stop");
    require(!authorization->active()&&!bridge.nativeAuthorization(*lease,*isolation),"stop retained native authorization");
    display.cleanupFinished(display.currentKey(),true);
    until([&]{return usage.snapshot(peer).value("usageState")=="Inactive";});
    QCoreApplication::processEvents();
    command("startPeerUsage",QString(32,'8'),{{"mode","input"}});
    until([&]{return usage.snapshot(peer).value("effectiveMode")=="input";});
    SessionId incomingSession{},incomingRequest{};incomingSession.fill(6);incomingRequest.fill(7);
    require(display.incomingAuthenticated({peer.toStdString(),"Ubuntu",supported},incomingSession,incomingRequest).accepted,
      "incoming native authorization fixture");
    const auto adopted=bridge.adoptIncoming();
    if(!adopted.accepted)std::cerr<<"incoming adoption reason="<<reasonName(adopted.reason)<<" usage="
      <<QJsonDocument(usage.snapshot(peer)).toJson(QJsonDocument::Compact).constData()<<'\n';
    require(adopted.accepted,"incoming usage adoption");
    isolation.reset();
    until([&]{
      const auto snapshot=usage.snapshot(peer);UsageKey key;
      const auto bytes=QByteArray::fromHex(snapshot.value("operationId").toString().toLatin1());
      if(bytes.size()!=16)return false;
      std::copy(bytes.begin(),bytes.end(),key.operation.begin());
      key.generation=snapshot.value("operationGeneration").toString().toULongLong();
      isolation=input.isolationProof(peer,key);return isolation.has_value();
    });
    require(!bridge.nativeAuthorization(*lease,*isolation),"incoming adoption forged user consent");
    const auto incoming=display.snapshot();QJsonObject allow{{"allow",true}};
    for(const auto* field:{"sessionId","requestId","generation","displayEpoch"})allow.insert(field,incoming.value(field));
    display.execute({{"id","allow-native"},{"operation","respondToDisplayRequest"},{"args",allow}});
    until([&]{return display.authorizationValid(display.currentKey());});
    const auto receiverAuthorization=bridge.nativeAuthorization(*lease,*isolation);
    require(receiverAuthorization&&receiverAuthorization->active()&&receiverAuthorization->role()==Role::Receiver,
      "receiver consent did not authorize native owner");
    if(argc==2&&QString::fromUtf8(argv[1])=="--destroy-core")core.reset();
    else socket->disconnectFromServer();
    until([&] { return lost == 1; });
    require(!channel.send(connection, stop), "disconnected binding retained");
    require(!lease->active() && !channel.lease(connection), "disconnected lease stayed active");
    require(!receiverAuthorization->active(),"disconnect retained receiver authorization");
    require(!isolation->active(),"core loss retained input isolation");
    std::cout << "core socket binding, receive/replay, wrapped budget and disconnect passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
