// SPDX-License-Identifier: MIT
#include "common/LocalTransportIdentity.h"
#include "common/InputRouteStatus.h"
#include "deskflow/InputControlCommand.h"
#include "common/PeerConnectionRegistry.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#ifndef Q_OS_WIN
#include "arch/unix/ArchNetworkBSD.h"
#endif
struct RecordingInputRoute {
  int changes=0;bool matches=true;
  bool matchesUsageConnection(const QString&,const QString&)const{return matches;}
  bool sendUsageControl(const QString&,const QString&,const QJsonObject&){return true;}
  bool requestInputProbe(const std::string&,uint32_t){++changes;return true;}
  bool requestInputRoute(const std::string&,uint32_t,bool){++changes;return true;}
  bool commitInputResume(const std::string&,uint32_t){++changes;return true;}
  InputRouteStatus inputRouteStatus(const std::string&,uint32_t,bool)const{InputRouteStatus result;result.connected=result.supported=true;return result;}
};
class LocalTransportIdentityTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void staleCoreCommandCannotChangeReplacementConnection(){
    QJsonObject request{{"id","test"},{"kind","suspend"},{"peerId",QString(64,'a')},{"baseConnectionId",QString(32,'b')},
      {"baseConnectionGeneration","1"},{"sequence","1"},{"operationId",QString(32,'c')},{"operationGeneration","1"},{"enabled",false}};
    RecordingInputRoute route;route.matches=false;
    auto execute=[&]{Event event(EventTypes::ServerInputControlCommand,this,new deskflow::input::InputControlCommandData(QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact))));return deskflow::input::executeInputControl(event,&route);};
    auto rejected=execute();QVERIFY(!rejected.value("accepted").toBool());QCOMPARE(rejected.value("reasonCode"),QJsonValue("ConnectionLost"));
    QCOMPARE(rejected.value("id"),request.value("id"));QCOMPARE(rejected.value("baseConnectionId"),request.value("baseConnectionId"));QCOMPARE(route.changes,0);
    route.matches=true;QVERIFY(execute().value("accepted").toBool());QCOMPARE(route.changes,1);
    request.remove("baseConnectionId");QVERIFY(!execute().value("accepted").toBool());QCOMPARE(route.changes,1);
  }
  void managedTransportWithoutPolicyFailsClosed()
  {
    const QString peer(64,'d'),id(32,'d');
    QVERIFY(LocalTransportIdentity::bind(2222,3333,"policy-pending",peer.toStdString()));
    PeerConnectionRegistry::update({{"peerId",peer},{"baseConnectionId",id}});
    auto policy=[&]{for(const auto& item:PeerConnectionRegistry::snapshot().value("connections").toArray())if(item.toObject().value("baseConnectionId")==id)return item.toObject().value("displayNetworkPolicy").toString();return QString();};
    QCOMPARE(policy(),QString("unknown"));
    PeerConnectionRegistry::uiContext(peer,{{"displayNetworkPolicy","bluetoothOnly"}});
    QCOMPARE(policy(),QString("bluetoothOnly"));
    LocalTransportIdentity::release("policy-pending");PeerConnectionRegistry::remove(id);
  }
  void cannotClaimAnotherLiveSocket()
  {
    const std::string first(64,'a'),second(64,'b');
    QVERIFY(LocalTransportIdentity::bind(1234,5678,"owner",first));
    QVERIFY(!LocalTransportIdentity::bind(1234,5678,"other",second));
    QCOMPARE(LocalTransportIdentity::lookup(1234,5678),first);
    QVERIFY(LocalTransportIdentity::lookup(5678,1234).empty());
    LocalTransportIdentity::release("other");QCOMPARE(LocalTransportIdentity::lookup(1234,5678),first);
    LocalTransportIdentity::release("owner");QVERIFY(LocalTransportIdentity::lookup(1234,5678).empty());
    QVERIFY(LocalTransportIdentity::bind(1234,5678,"new",second));LocalTransportIdentity::release("new");
    QVERIFY(!LocalTransportIdentity::bind(1234,5678,"new","not a verified fingerprint"));
  }
  void usesActualLoopbackSocketEndpoints()
  {
#ifndef Q_OS_WIN
    QTcpServer listener;QVERIFY(listener.listen(QHostAddress::LocalHost,0));
    QTcpSocket connector;connector.connectToHost(QHostAddress::LocalHost,listener.serverPort());QVERIFY(connector.waitForConnected());
    QVERIFY(listener.waitForNewConnection());QScopedPointer<QTcpSocket> accepted(listener.nextPendingConnection());
    ArchNetworkBSD network;ArchSocketImpl socket{static_cast<int>(accepted->socketDescriptor()),1};
    uint16_t local=0,peer=0;QVERIFY(network.getLoopbackPorts(&socket,local,peer));
    QCOMPARE(local,listener.serverPort());QCOMPARE(peer,connector.localPort());
    QVERIFY(LocalTransportIdentity::bind(local,peer,"real-socket",std::string(64,'a')));
    QCOMPARE(LocalTransportIdentity::lookup(local,peer),std::string(64,'a'));
    LocalTransportIdentity::release("real-socket");
#endif
  }
  void controlRequiresMatchingAuthenticatedConnection()
  {
    const QString id(32,'b'),peer(64,'a');
    QVERIFY(!PeerConnectionRegistry::controlReceived(peer,id,{{"type","request"}}));
    PeerConnectionRegistry::update({{"baseConnectionId",id},{"peerId",peer}});
    QVERIFY(!PeerConnectionRegistry::controlReceived(QString(64,'c'),id,{{"type","request"}}));
    QVERIFY(PeerConnectionRegistry::controlReceived(peer,id,{{"type","request"}}));
    auto snapshot=PeerConnectionRegistry::snapshot();QCOMPARE(snapshot.value("events").toArray().size(),1);
    QVERIFY(PeerConnectionRegistry::snapshot().value("events").toArray().isEmpty());
    PeerConnectionRegistry::remove(id);QVERIFY(!PeerConnectionRegistry::controlReceived(peer,id,{{"type","request"}}));
  }
};
QTEST_GUILESS_MAIN(LocalTransportIdentityTests)
#include "LocalTransportIdentityTests.moc"
