// SPDX-License-Identifier: MIT
// Manual integration: creates real GNOME outputs and native viewer windows.
// Input acknowledgements are simulated; this does not certify input hardware.
#include "display/ProductDisplayRuntime.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSslCertificate>
#include <QCryptographicHash>
#include <QThread>
#include <QUuid>
#include <iostream>
#include <stdexcept>
using namespace deskflow::display;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void wait(const std::function<bool()>& condition,int timeout,const char* message){
  QElapsedTimer timer;timer.start();while(!condition()&&timer.elapsed()<timeout){QCoreApplication::processEvents();QThread::msleep(10);}require(condition(),message);
}
struct Side {
  PeerUsageService usage;
  DisplayService display{{},{}};
  UsageDisplayBridge bridge{&usage,&display};
  InputRouteAdapter input{&usage,&display};
  CoreInputChannel channel;
  std::unique_ptr<ProductDisplayRuntime> runtime;
  QString peer,id;
  bool consent=false;
  uint32_t sequence=0;bool suspended=false,received=false,ready=false;unsigned resultMode=0;
  Side(QString peerId,QString conn,const ProductDisplayConfiguration& config):channel(&input,&usage,nullptr,config.resources+"/unused-core",false),peer(peerId),id(conn){
    runtime=std::make_unique<ProductDisplayRuntime>(&usage,&display,&bridge,&channel,nullptr,&config);
    UsageCapabilities caps{Availability::Supported,Availability::Supported,Availability::Supported,Availability::Unknown,Availability::Supported,Availability::Supported,Availability::Supported,false};
    PeerUsageService::BaseConnection connection;connection.id=conn.toStdString();connection.generation=1;connection.connected=true;
    connection.role=conn==QString(32,'a')?PeerUsageService::InputRole::Source:PeerUsageService::InputRole::Receiver;
    connection.inputActive=true;connection.permission=Permission::Granted;connection.focusOwner="local";
    require(usage.syncConnectedPeer({peer.toStdString(),"runtime-smoke",{}},caps,connection).accepted,"connection registration failed");
    QObject::connect(&input,&InputRouteAdapter::coreCommand,&input,[this](const QJsonObject& command){
      auto response=command;const auto kind=command.value("kind").toString();auto requested=command.value("sequence").toString().toUInt();
      if(kind=="probe"){sequence=requested;received=true;ready=true;resultMode=3;}
      else if(kind=="suspend"){sequence=requested;suspended=true;received=true;ready=false;resultMode=1;}
      else if(kind=="resume"){sequence=requested;received=true;ready=false;resultMode=2;}
      else if(kind=="commitResume")suspended=false;
      response.insert("accepted",true);response.insert("connected",true);response.insert("supported",true);
      response.insert("suspended",suspended);response.insert("localFocus",true);
      response.insert("acknowledgementReceived",requested==sequence&&received);
      response.insert("acknowledged",requested==sequence&&resultMode==(command.value("enabled").toBool()?2U:1U));
      response.insert("inputPermissionsReady",requested==sequence&&ready);response.insert("otherRoutesIsolated",true);
      response.insert("lastSequence",QString::number(sequence));input.coreResult(response);
    });
    QObject::connect(&usage,&PeerUsageService::commandFinished,&usage,[this](const QJsonObject& reply){if(!reply.value("accepted").toBool())std::cerr<<id.at(0).toLatin1()<<" rejected "<<QJsonDocument(reply).toJson(QJsonDocument::Compact).constData()<<"\n";});
    QObject::connect(&display,&DisplayService::snapshotChanged,&display,[this](const QJsonObject& snapshot){
      std::cerr<<id.at(0).toLatin1()<<" display "<<snapshot.value("state").toString().toStdString()<<" "<<snapshot.value("reasonCode").toString().toStdString()<<"\n";
      if(snapshot.value("localDisplayRole")=="receiver"&&snapshot.value("state")=="Authorizing"&&!consent){
        consent=true;QJsonObject args;for(auto key:{"sessionId","requestId","generation","displayEpoch"})args.insert(key,snapshot.value(key));args.insert("allow",true);
        display.execute({{"id","smoke-consent"},{"operation","respondToDisplayRequest"},{"args",args}});
      }
    });
  }
  void observe(const QString& policy="networkAllowed"){channel.authenticatedConnectionObserved({{"peerId",peer},{"peerName","runtime-smoke"},{"baseConnectionId",id},{"baseConnected",true},{"displayNetworkPolicy",policy}});}
};
QString certificateId(const QString& path){QFile file(path);require(file.open(QIODevice::ReadOnly),"certificate missing");const auto certificates=QSslCertificate::fromData(file.readAll());require(certificates.size()==1,"invalid certificate");return certificates.first().digest(QCryptographicHash::Sha256).toHex();}
}
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);if(argc!=4)return 2;
 try{
  const QString root=QString::fromLocal8Bit(argv[1]),scripts=QString::fromLocal8Bit(argv[2]),viewer=QString::fromLocal8Bit(argv[3]);
  auto config=[&](const QString& side){ProductDisplayConfiguration c;c.resources=root+"/"+side+"/product";c.certificate=root+"/"+side+"/combined.pem";c.scriptDirectory=scripts;
    c.media.python="/usr/bin/python3";c.media.sessionScript=scripts+"/display_session.py";c.media.nativeViewer=viewer;c.media.source="gnome";c.media.bindAddress="0.0.0.0";return c;};
  Side a(certificateId(root+"/b/cert.pem"),QString(32,'a'),config("a")),b(certificateId(root+"/a/cert.pem"),QString(32,'b'),config("b"));
  bool holdProof=false;QList<QJsonObject> delayedProofs;bool localCleanupVerified=false;
  QObject::connect(a.runtime->findChild<MediaSupervisor*>(),&MediaSupervisor::localCleanupFinished,&a.display,[&](const QJsonObject& proof){localCleanupVerified=proof.value("cleanupVerified").toBool();});
  auto relay=[&](Side& from,Side& to){QObject::connect(from.runtime.get(),&ProductDisplayRuntime::outboundControl,&to.channel,[&,target=&to](const QString&,const QString&,const QJsonObject& body){
    if(holdProof&&target==&a&&(body.value("type")=="cleaned"||body.value("type")=="cleanupProbe")){delayedProofs.append(body);return;}
    target->channel.peerControlReceived({{"peerId",target->peer},{"baseConnectionId",target->id},{"body",body}});
  },Qt::QueuedConnection);};relay(a,b);relay(b,a);a.observe();b.observe();
  wait([&]{return a.usage.snapshot(a.peer).value("inputCapabilities").toObject().value("extended")=="supported"&&b.usage.snapshot(b.peer).value("inputCapabilities").toObject().value("extended")=="supported";},15000,"capabilities not ready");
  a.observe("bluetoothOnly");
  wait([&]{return b.usage.snapshot(b.peer).value("inputCapabilities").toObject().value("displayUnavailableReason")=="BluetoothOnlyMode";},5000,"remote policy not propagated");
  require(!a.usage.snapshot(a.peer).value("inputCapabilities").toObject().value("displayNetworkAllowed").toBool(),"local policy ignored");
  SessionId blocked{};blocked[0]=9;require(b.display.requestManagedSession(b.peer,blocked).reason==Reason::BluetoothOnlyMode,"remote policy bypassed lower API");
  a.observe();wait([&]{return a.usage.snapshot(a.peer).value("inputCapabilities").toObject().value("displayNetworkAllowed").toBool()&&b.usage.snapshot(b.peer).value("inputCapabilities").toObject().value("displayNetworkAllowed").toBool();},5000,"network policy not restored");
  for(auto* host:{&a,&b}){
    a.consent=b.consent=false;
    host->usage.execute({{"id","smoke-start"},{"operation",host->usage.snapshot(host->peer).value("effectiveMode")=="none"?"startPeerUsage":"switchPeerUsageMode"},{"args",QJsonObject{{"peerId",host->peer},{host->usage.snapshot(host->peer).value("effectiveMode")=="none"?"mode":"targetMode","extended"},{"operationId",QUuid::createUuid().toString(QUuid::Id128)},{"baseConnectionId",host->id},{"baseConnectionGeneration","1"}}}});
    wait([&]{return a.usage.snapshot(a.peer).value("effectiveMode")=="extended"&&b.usage.snapshot(b.peer).value("effectiveMode")=="extended";},20000,"bidirectional first frame missing");
    require(a.display.snapshot().value("firstFramePresented").toBool()&&b.display.snapshot().value("firstFramePresented").toBool(),"false streaming status");
    auto layoutCommand=[&](const QString& operation,QJsonObject args){
      const auto id=QUuid::createUuid().toString(QUuid::Id128);QJsonObject result;
      auto subscription=QObject::connect(&host->display,&DisplayService::commandFinished,&host->display,[&](const QJsonObject& reply){if(reply.value("id")==id)result=reply;});
      const auto current=host->display.snapshot();for(auto field:{"sessionId","requestId","generation","displayEpoch"})args.insert(field,current.value(field));
      host->display.execute({{"id",id},{"operation",operation},{"args",args}});
      wait([&]{return !result.isEmpty();},6000,"layout confirmation timed out");QObject::disconnect(subscription);
      if(!result.value("accepted").toBool())std::cerr<<operation.toStdString()<<" "<<QJsonDocument(result).toJson(QJsonDocument::Compact).constData()<<"\n";
      return result;
    };
    auto topology=layoutCommand("queryDisplayLayout",{});require(topology.value("accepted").toBool(),"topology unavailable");
    const auto before=topology.value("data").toObject().value("layout").toObject();
    // Place below primary, then restore. Mutter rejects gaps/overlaps itself.
    QJsonObject primary;for(const auto& item:topology.value("data").toObject().value("displays").toArray())if(item.toObject().value("primary").toBool())primary=item.toObject();
    require(!primary.isEmpty(),"primary display unavailable");
    auto moved=layoutCommand("setDisplayLayout",{{"layout",QJsonObject{{"x",0},{"y",primary.value("height")}}}});
    require(moved.value("accepted").toBool(),"system did not move owned display");
    require(host->display.snapshot().value("confirmedLayout").toObject().value("layout").toObject().value("y")==primary.value("height"),"unconfirmed layout published");
    require(layoutCommand("setDisplayLayout",{{"layout",QJsonObject{{"x",before.value("x")},{"y",before.value("y")}}}}).value("accepted").toBool(),"layout restore failed");
    require(host->display.snapshot().value("streamConfig").toObject().value("codec")=="h264","negotiated stream missing");
    auto* receiver=host==&a?&b:&a;
    auto windowCommand=[&](const QString& operation,QJsonObject args){
      const auto id=QUuid::createUuid().toString(QUuid::Id128);QJsonObject result;
      auto subscription=QObject::connect(&receiver->display,&DisplayService::commandFinished,&receiver->display,[&](const QJsonObject& reply){if(reply.value("id")==id)result=reply;});
      const auto current=receiver->display.snapshot();for(auto field:{"sessionId","requestId","generation","displayEpoch"})args.insert(field,current.value(field));
      receiver->display.execute({{"id",id},{"operation",operation},{"args",args}});
      wait([&]{return !result.isEmpty();},5000,"window confirmation timed out");QObject::disconnect(subscription);if(!result.value("accepted").toBool()&&args.value("receiverScreenId")!="absent-screen")std::cerr<<operation.toStdString()<<" "<<QJsonDocument(result).toJson(QJsonDocument::Compact).constData()<<"\n";return result;
    };
    require(windowCommand("presentReceiverWindow",{}).value("accepted").toBool(),"window not restored");
    require(windowCommand("setReceiverPresentation",{{"presentationMode","window"},{"scaleMode","actual"}}).value("accepted").toBool(),"actual pixel size not confirmed");
    require(windowCommand("setReceiverPresentation",{{"presentationMode","window"},{"scaleMode","fit"}}).value("accepted").toBool(),"fit scale not confirmed");
    require(windowCommand("setReceiverPresentation",{{"presentationMode","fullscreen"}}).value("accepted").toBool(),"fullscreen not confirmed");
    require(receiver->display.snapshot().value("presentationMode")=="fullscreen","fullscreen snapshot missing");
    require(windowCommand("setReceiverPresentation",{{"presentationMode","window"},{"windowState","minimized"}}).value("accepted").toBool(),"minimize not confirmed");
    require(windowCommand("presentReceiverWindow",{}).value("accepted").toBool(),"restore after minimizing failed");
    require(!windowCommand("setReceiverPresentation",{{"presentationMode","fullscreen"},{"receiverScreenId","absent-screen"}}).value("accepted").toBool(),"selected an absent screen");
    if(host==&a){
      holdProof=true;localCleanupVerified=false;
      host->usage.execute({{"id","smoke-return-input"},{"operation","switchPeerUsageMode"},{"args",QJsonObject{{"peerId",host->peer},{"targetMode","input"},{"operationId",QUuid::createUuid().toString(QUuid::Id128)},{"baseConnectionId",host->id},{"baseConnectionGeneration","1"}}}});
      wait([&]{return localCleanupVerified;},12000,"local cleanup did not finish");
      QElapsedTimer barrier;barrier.start();while(barrier.elapsed()<150){QCoreApplication::processEvents();QThread::msleep(10);}
      require(host->usage.snapshot(host->peer).value("effectiveMode")!="input","input resumed before peer cleanup");
      holdProof=false;for(const auto& body:delayedProofs)a.channel.peerControlReceived({{"peerId",a.peer},{"baseConnectionId",a.id},{"body",body}});delayedProofs.clear();
      wait([&]{return host->usage.snapshot(host->peer).value("effectiveMode")=="input";},6000,"input did not resume after both cleanup proofs");
    }
    if(host==&b)host->observe("bluetoothOnly");
    else host->usage.execute({{"id","smoke-stop"},{"operation","stopPeerUsage"},{"args",QJsonObject{{"peerId",host->peer},{"baseConnectionId",host->id},{"baseConnectionGeneration","1"}}}});
    wait([&]{return a.display.snapshot().value("state")=="Idle"&&b.display.snapshot().value("state")=="Idle"&&a.usage.snapshot(a.peer).value("effectiveMode")=="none"&&b.usage.snapshot(b.peer).value("effectiveMode")=="none";},20000,"cleanup not verified");
    // Native routes remain suspended, so the next opposite-direction session has a verified clear gate.
  }
  std::cout<<"{\"passed\":true,\"directions\":2,\"nativePresentation\":true,\"inputHardwareVerified\":false}\n";return 0;
 }catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}
}
