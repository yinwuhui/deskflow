// SPDX-License-Identifier: MIT
#include "ProductDisplayRuntime.h"
#include "ScreenInventoryService.h"
#include <QGuiApplication>
#include "common/Settings.h"
#include "common/UsageInputState.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QSaveFile>
#include <QSslCertificate>
#include <QStandardPaths>
namespace deskflow::display {
namespace {
bool save(const QString& path,const QByteArray& bytes){
  QSaveFile file(path);if(!file.open(QIODevice::WriteOnly))return false;
  file.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
  return file.write(bytes)==bytes.size()&&file.commit();
}
Availability availability(const QJsonValue& value){return value=="supported"?Availability::Supported:value=="unsupported"?Availability::Unsupported:Availability::Unknown;}
QJsonArray addresses(){
  QJsonArray result;
  for(const auto& interface:QNetworkInterface::allInterfaces()){
    if(!(interface.flags()&QNetworkInterface::IsUp)||interface.flags()&QNetworkInterface::IsLoopBack)continue;
    for(const auto& entry:interface.addressEntries())if(entry.ip().protocol()==QAbstractSocket::IPv4Protocol&&!entry.ip().isLinkLocal())result.append(entry.ip().toString());
  }
  return result;
}
QString address(const QJsonArray& remote){
  // P0 media is direct LAN only. Never derive trust from these addresses.
  for(const auto& item:remote){QHostAddress candidate(item.toString());
    if(candidate.protocol()!=QAbstractSocket::IPv4Protocol||candidate.isLoopback()||candidate.isMulticast()||candidate.isNull())continue;
    for(const auto& interface:QNetworkInterface::allInterfaces())for(const auto& entry:interface.addressEntries())
      if(entry.ip().protocol()==QAbstractSocket::IPv4Protocol&&entry.prefixLength()>0&&candidate.isInSubnet(entry.ip(),entry.prefixLength()))return candidate.toString();
  }
  return {};
}
SessionId session(const QJsonValue& value){
  const auto text=value.toString();SessionId result{};if(text.size()!=32||text.toLower()!=text)return result;
  const auto bytes=QByteArray::fromHex(text.toLatin1());if(bytes.size()==16&&bytes.toHex()==text.toLatin1())std::copy(bytes.begin(),bytes.end(),result.begin());return result;
}
Reason reason(const QJsonValue& value){
  for(auto r:{Reason::PeerRejected,Reason::PeerBusy,Reason::Cancelled,Reason::ConnectionLost,Reason::PermissionDenied,Reason::UserStopped,Reason::ApplicationExit,Reason::CleanupFailed,Reason::InputReleaseFailed,Reason::CapabilityCheckRequired,Reason::SessionUnavailable,Reason::NetworkUnavailable,Reason::UnsupportedPlatform,Reason::BluetoothOnlyMode})
    if(value==reasonName(r))return r;
  return Reason::SwitchFailed;
}
}
ProductDisplayRuntime::ProductDisplayRuntime(PeerUsageService* usage,DisplayService* display,UsageDisplayBridge* bridge,CoreInputChannel* channel,QObject* parent,const ProductDisplayConfiguration* configuration)
 :QObject(parent),m_usage(usage),m_display(display),m_bridge(bridge),m_channel(channel){
  const auto executable=QCoreApplication::applicationDirPath();
  m_root=QDir(executable).absoluteFilePath("../share/busymouse/display");
  const auto bundleResources=QDir(executable).absoluteFilePath("../Resources/display");
  if(QFileInfo::exists(bundleResources+"/display_session.py"))m_root=bundleResources;
  if(!QFileInfo::exists(m_root+"/display_session.py"))m_root=QStringLiteral(DESKFLOW_DISPLAY_RUNTIME_SOURCE);
  if(configuration&&!configuration->scriptDirectory.isEmpty())m_root=configuration->scriptDirectory;
  m_runtime.python=QStandardPaths::findExecutable("python3");m_runtime.sessionScript=m_root+"/display_session.py";
  m_runtime.nativeViewer=executable+"/busymouse-display-viewer";
  m_runtime.macosProbe=executable+"/busymouse-display-probe";
#ifdef Q_OS_MACOS
  m_runtime.source="macos";
#else
  m_runtime.source="gnome";
#endif
  m_runtime.bindAddress="0.0.0.0";
  if(configuration)m_runtime=configuration->media;
  // Keep monitor names, scale and coordinates in the same Qt platform as the GUI.
  if(qobject_cast<QGuiApplication*>(QCoreApplication::instance()))m_runtime.environment.insert("QT_QPA_PLATFORM",QGuiApplication::platformName());
  const auto config=configuration?configuration->resources:QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)+"/display-runtime";
  QDir().mkpath(config);QFile::setPermissions(config,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
  m_runtime.identityDirectory=config+"/identity";QDir().mkpath(m_runtime.identityDirectory);
  QFile::setPermissions(m_runtime.identityDirectory,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
  QFile pem(configuration?configuration->certificate:Settings::value(Settings::Security::Certificate).toString());
  if(pem.open(QIODevice::ReadOnly)&&pem.size()<=65536){
    const auto bytes=pem.readAll();const auto certs=QSslCertificate::fromData(bytes);
    if(certs.size()==1){
      m_publicCertificate=QString::fromLatin1(certs.first().toPem());
      if(!save(m_runtime.identityDirectory+"/cert.pem",certs.first().toPem())||!save(m_runtime.identityDirectory+"/key.pem",bytes))m_publicCertificate.clear();
    }
  }
  connect(this,&ProductDisplayRuntime::outboundControl,channel,&CoreInputChannel::sendPeerControl);
  m_inputStateFile=configuration?configuration->resources+"/usage-input.ini":UsageInputState::filePath();
  m_media=new MediaSupervisor(display,m_runtime,config+"/resources",this);
  m_media->setCoordinatedCleanup(true);
  if(qobject_cast<QGuiApplication*>(QCoreApplication::instance())){
    m_screens=ScreenInventoryService::applicationService();m_screens->attach(channel,usage);
    m_media->setReceiverSelectionValidator([this](QString& name,QString& mode){
      if(!m_screens->resolveLocalSelection(m_receiverSelection,&name))return false;
      mode=m_receiverSelection.value("presentationMode").toString();return true;
    });
  }
  connect(m_media,&MediaSupervisor::localCleanupFinished,this,&ProductDisplayRuntime::localCleanup);
  connect(display,&DisplayService::snapshotChanged,this,&ProductDisplayRuntime::resourceState);
  resourceState(display->snapshot());
  if(display->snapshot().value("state")=="CleanupRequired")bridge->adoptCleanup();
  connect(channel,&CoreInputChannel::authenticatedConnectionObserved,this,&ProductDisplayRuntime::observed);
  connect(channel,&CoreInputChannel::peerControlReceived,this,&ProductDisplayRuntime::received);
  connect(display,&DisplayService::backendCommand,this,&ProductDisplayRuntime::effect);
  connect(bridge,&UsageDisplayBridge::receiverInputReady,this,[this]{m_receiverReady=true;send(m_peer,wire("accepted"));});
  connect(m_media,&MediaSupervisor::listening,this,[this](const QJsonObject&){send(m_peer,wire("prepare"));});
  connect(usage,&PeerUsageService::connectionChanged,this,[this](const QJsonObject& value){
    const auto peer=value.value("peerId").toString();
    if(value.value("baseConnected").toBool())return;
    if(m_connections.contains(peer)&&m_connections.value(peer).value("baseConnectionId")!=value.value("baseConnectionId"))return;
    m_connections.remove(peer);m_remote.remove(peer);m_display->forgetPeer(peer);
    if(peer==m_peer){m_display->stopManagedSession(m_display->currentKey(),Reason::ConnectionLost);finishCleanup();}
  });
  connect(display,&DisplayService::snapshotChanged,this,[this](const QJsonObject& snapshot){
    if(snapshot.value("localDisplayRole")=="host"&&snapshot.value("state")=="Authorizing"&&snapshot.value("permissions").toObject().value("capture")=="required"&&!m_permissionPending){
      m_permissionPending=true;probe(true);
    }
    if(snapshot.value("state")=="Idle"){m_permissionPending=false;m_receiverReady=false;}
  });
  connect(usage,&PeerUsageService::snapshotChanged,this,[this](const QJsonObject& value){
    if(value.value("peerId")!=m_peer||m_display->snapshot().value("localDisplayRole")!="receiver")return;
    if(value.value("usageState")=="CleanupRequired"||(!m_receiverReady&&value.value("targetMode")!="extended"&&value.value("reasonCode")!="None"))
      m_display->stopManagedSession(m_display->currentKey(),Reason::InputReleaseFailed);
  });
  connect(QCoreApplication::instance(),&QCoreApplication::aboutToQuit,this,[this]{
    m_display->stopManagedSession(m_display->currentKey(),Reason::ApplicationExit);
  });
  m_probeTimer.setInterval(5000);connect(&m_probeTimer,&QTimer::timeout,this,[this]{probe();});m_probeTimer.start();
  QTimer::singleShot(0,this,[this]{probe();});
}
ProductDisplayRuntime::~ProductDisplayRuntime(){
  m_probeTimer.stop();
  if(m_probe){m_probe->disconnect(this);m_probe->kill();m_probe->waitForFinished(1000);}
}
namespace {
Reason probeReason(const QJsonValue& value) {
  if(value=="None")return Reason::None;
  if(value=="SessionUnavailable")return Reason::SessionUnavailable;
  if(value=="UnsupportedPlatform")return Reason::UnsupportedPlatform;
  return Reason::CapabilityCheckRequired;
}
}
void ProductDisplayRuntime::probe(bool permission){
  if(m_probe){if(permission)QTimer::singleShot(250,this,[this]{
    if(m_permissionPending&&m_display->snapshot().value("state")=="Authorizing"&&m_display->snapshot().value("permissions").toObject().value("capture")=="required")probe(true);
  });return;}
  const auto permissionOwner=m_display->currentKey();
  auto process=new QProcess(this);m_probe=process;
  QStringList arguments{m_root+"/product_probe.py","--viewer",m_runtime.nativeViewer,"--macos-probe",m_runtime.macosProbe};
  if(permission)arguments<<"--request-permission";
  process->setProgram(m_runtime.python);process->setArguments(arguments);process->setProcessEnvironment(m_runtime.environment);
  auto output=std::make_shared<QByteArray>();
  connect(process,&QProcess::readyReadStandardOutput,this,[process,output]{*output+=process->readAllStandardOutput();if(output->size()>65536)process->kill();});
  auto timer=new QTimer(process);timer->setSingleShot(true);connect(timer,&QTimer::timeout,process,&QProcess::kill);timer->start(permission?60000:25000);
  auto done=[this,process,output,permission,permissionOwner](bool success){
    if(m_probe!=process)return;*output+=process->readAllStandardOutput();const auto doc=QJsonDocument::fromJson(*output);const auto report=doc.object();
    m_local={};m_local.send=success?availability(report.value("send")):Availability::Unknown;
    m_local.receive=success?availability(report.value("receive")):Availability::Unknown;
    m_local.sessionAvailable=success&&report.value("sessionAvailable").toBool()&&!m_publicCertificate.isEmpty();
    m_local.sessionReason=success?probeReason(report.value("sessionReason")):Reason::CapabilityCheckRequired;
    m_local.sendReason=success?probeReason(report.value("sendReason")):Reason::CapabilityCheckRequired;
    m_local.receiveReason=success?probeReason(report.value("receiveReason")):Reason::CapabilityCheckRequired;
    if(m_publicCertificate.isEmpty())m_local.sessionReason=Reason::CapabilityCheckRequired;
    m_local.fullscreen=m_local.receive==Availability::Supported&&m_local.sessionAvailable&&report.value("fullscreen").toBool();
    m_local.layout=m_local.send==Availability::Supported&&m_local.sessionAvailable;
    m_local.minimize=m_local.fullscreen&&report.value("minimize").toBool();
    m_permission=report.value("permission").toString("unknown");m_display->updateCapabilities(m_local);
    if(report.value("sessionChecked").toBool()&&!m_local.sessionAvailable)for(const auto& peer:m_connections.keys())m_usage->invalidateRollback(peer);
    if(!m_local.sessionAvailable&&m_display->snapshot().value("state")!="Idle")m_display->stopManagedSession(m_display->currentKey(),Reason::SessionUnavailable);
    if(permission&&permissionOwner==m_display->currentKey()&&m_display->snapshot().value("localDisplayRole")=="host")m_display->permissionResult(m_display->currentKey(),m_permission=="granted"?Permission::Granted:Permission::Denied);
    m_probe=nullptr;process->deleteLater();
    for(const auto& peer:m_connections.keys()){hello(peer);capabilities(peer);}
  };
  connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[done](int code,QProcess::ExitStatus status){done(code==0&&status==QProcess::NormalExit);});
  connect(process,&QProcess::errorOccurred,this,[done](QProcess::ProcessError error){if(error==QProcess::FailedToStart)done(false);});
  process->start();
}
void ProductDisplayRuntime::observed(const QJsonObject& connection){
  const auto peer=connection.value("peerId").toString();const auto old=m_connections.value(peer);
  m_connections[peer]=connection;
  if(old.value("baseConnectionId")!=connection.value("baseConnectionId")){m_remote.remove(peer);}
  hello(peer);capabilities(peer);
}
void ProductDisplayRuntime::hello(const QString& peer){
  if(m_publicCertificate.isEmpty())return;
  const auto policy=m_connections.value(peer).value("displayNetworkPolicy").toString("unknown");
  send(peer,{{"type","hello"},{"displayNetworkPolicy",policy},{"capabilities",m_display->localCapabilities()},
    {"certificate",m_publicCertificate},{"addresses",policy=="networkAllowed"?addresses():QJsonArray{}}});
}
void ProductDisplayRuntime::send(const QString& peer,QJsonObject body){
  const auto connection=m_connections.value(peer).value("baseConnectionId").toString();if(connection.isEmpty())return;
  body.insert("protocol","usage-display/1");Q_EMIT outboundControl(peer,connection,body);
}
void ProductDisplayRuntime::capabilities(const QString& peer){
  const auto remote=m_remote.value(peer);const auto caps=remote.value("capabilities").toObject();
  const auto localPolicy=m_connections.value(peer).value("displayNetworkPolicy").toString("unknown");
  const auto remotePolicy=remote.value("displayNetworkPolicy").toString("unknown");
  const auto policyReason=(localPolicy=="bluetoothOnly"||remotePolicy=="bluetoothOnly")?Reason::BluetoothOnlyMode:
    (localPolicy=="networkAllowed"&&remotePolicy=="networkAllowed")?Reason::None:Reason::CapabilityCheckRequired;
  const bool network=!address(remote.value("addresses").toArray()).isEmpty();
  const auto combine=[&](Availability local,const QJsonValue& value){const auto other=availability(value);
    if(local==Availability::Unsupported||other==Availability::Unsupported)return Availability::Unsupported;
    if(local!=Availability::Supported||other!=Availability::Supported)return Availability::Unknown;return Availability::Supported;};
  const auto readiness=[&](bool outgoing){
    if(!m_local.sessionAvailable)return m_local.sessionReason;
    if(!caps.value("sessionAvailable").toBool())return probeReason(caps.value("sessionReason"));
    const auto local=outgoing?m_local.send:m_local.receive;
    const auto other=availability(caps.value(outgoing?"receive":"send"));
    if(local!=Availability::Supported)return outgoing?m_local.sendReason:m_local.receiveReason;
    if(other!=Availability::Supported)return probeReason(caps.value(outgoing?"receiveReason":"sendReason"));
    return Reason::None;
  };
  m_usage->updateDisplayCapabilities(peer,combine(m_local.send,caps.value("receive")),combine(m_local.receive,caps.value("send")),network,policyReason,readiness(true),readiness(false));
  m_display->setPeerNetworkPolicy(peer,policyReason==Reason::None);
}
QJsonObject ProductDisplayRuntime::wire(const QString& type)const{auto result=m_wire;result.insert("type",type);return result;}
bool ProductDisplayRuntime::current(const QString& peer,const QJsonObject& body)const{
  const auto snapshot=m_display->snapshot();
  return peer==m_peer&&body.value("sessionId")==m_wire.value("sessionId")&&body.value("requestId")==m_wire.value("requestId")&&
    body.value("generation")==m_wire.value("generation")&&body.value("displayEpoch")==m_wire.value("displayEpoch")&&
    snapshot.value("sessionId")==m_wire.value("sessionId")&&snapshot.value("requestId")==m_wire.value("requestId");
}
void ProductDisplayRuntime::received(const QJsonObject& event){
  const auto peer=event.value("peerId").toString();const auto connection=m_connections.value(peer);
  if(connection.isEmpty()||connection.value("baseConnectionId")!=event.value("baseConnectionId"))return;
  const auto body=event.value("body").toObject();if(body.value("protocol")!="usage-display/1")return;
  const auto type=body.value("type").toString();
  if(type=="hello"){
    const auto certs=QSslCertificate::fromData(body.value("certificate").toString().toLatin1());
    if(certs.size()!=1||certs.first().digest(QCryptographicHash::Sha256).toHex()!=peer.toLatin1()||body.value("addresses").toArray().size()>32)return;
    const auto caps=body.value("capabilities").toObject();
    if(body.value("displayNetworkPolicy")!="networkAllowed"&&body.value("displayNetworkPolicy")!="bluetoothOnly")return;
    if(caps.value("protocolVersion")!=1||!caps.value("sessionAvailable").isBool())return;
    const bool first=!m_remote.contains(peer);m_remote[peer]=body;
    const auto path=m_runtime.identityDirectory+"/peer-"+peer+".pem";
    if(save(path,certs.first().toPem())){
      Peer known{peer.toStdString(),connection.value("peerName").toString().toStdString(),{}};
      known.capabilities.send=availability(caps.value("send"));known.capabilities.receive=availability(caps.value("receive"));known.capabilities.sessionAvailable=caps.value("sessionAvailable").toBool();
      known.capabilities.sessionReason=probeReason(caps.value("sessionReason"));known.capabilities.sendReason=probeReason(caps.value("sendReason"));known.capabilities.receiveReason=probeReason(caps.value("receiveReason"));
      const auto host=address(body.value("addresses").toArray());
      if(!host.isEmpty()&&body.value("displayNetworkPolicy")=="networkAllowed"&&connection.value("displayNetworkPolicy")=="networkAllowed")m_media->addTrustedEndpoint({known,path,host,24810});
    }
    capabilities(peer);if(first)hello(peer);return;
  }
  if(!m_remote.contains(peer))return;
  if(type=="request"){
    if(session(body.value("sessionId"))==SessionId{}||session(body.value("requestId"))==SessionId{}||body.value("displayEpoch")!=1)return;
    bool valid=false;const auto generation=body.value("generation").toString().toULongLong(&valid);
    if(!valid||!generation||QString::number(generation)!=body.value("generation"))return;
    const auto caps=m_remote.value(peer).value("capabilities").toObject();
    Peer known{peer.toStdString(),connection.value("peerName").toString().toStdString(),{}};
    known.capabilities.send=availability(caps.value("send"));known.capabilities.receive=availability(caps.value("receive"));known.capabilities.sessionAvailable=caps.value("sessionAvailable").toBool();
      known.capabilities.sessionReason=probeReason(caps.value("sessionReason"));known.capabilities.sendReason=probeReason(caps.value("sendReason"));known.capabilities.receiveReason=probeReason(caps.value("receiveReason"));
    if(connection.value("displayNetworkPolicy")!="networkAllowed"||m_remote.value(peer).value("displayNetworkPolicy")!="networkAllowed"){
      auto reply=body;reply.insert("type","rejected");reply.insert("reasonCode","BluetoothOnlyMode");send(peer,reply);return;
    }
    if(m_screens&&!m_screens->resolveLocalSelection(body.value("screenSelection").toObject())){
      auto reply=body;reply.insert("type","rejected");reply.insert("reasonCode","CapabilityCheckRequired");send(peer,reply);return;
    }
    m_receiverSelection=body.value("screenSelection").toObject();
    auto result=m_display->incomingAuthenticated(known,session(body.value("sessionId")),session(body.value("requestId")));
    if(!result.accepted){auto reply=body;reply.insert("type","rejected");reply.insert("reasonCode",reasonName(result.reason));send(peer,reply);return;}
    m_peer=peer;m_wire={};for(auto key:{"sessionId","requestId","generation","displayEpoch"})m_wire.insert(key,body.value(key));
    m_receiverReady=false;m_displayBase=connection.value("baseConnectionId").toString();
    m_haveLocalCleanup=m_havePeerCleanup=m_localCleanup=m_peerCleanup=false;m_localResourceGone=false;m_peerTermination=false;m_cleanupReply={};return;
  }
  if(type=="cleanupProbe"){
    const auto snapshot=m_display->snapshot();const bool sameResource=snapshot.value("peerId")==peer&&snapshot.value("sessionId")==body.value("sessionId")&&snapshot.value("requestId")==body.value("requestId")&&snapshot.value("displayEpoch")==body.value("displayEpoch");
    if(m_cleanupProofs.contains(proofKey(peer,body))&&(!sameResource||snapshot.value("state")=="Idle"||(m_haveLocalCleanup&&m_localCleanup))){
      auto response=body;response.insert("type","cleaned");response.insert("cleanupVerified",true);send(peer,response);return;
    }
    if(sameResource&&(snapshot.value("state")=="Stopping"||snapshot.value("state")=="CleanupRequired")){
      m_cleanupReply=body;
      if(body.value("cleanupVerified").toBool()){m_havePeerCleanup=true;m_peerCleanup=true;finishCleanup();}
      if(snapshot.value("state")=="CleanupRequired")m_usage->retryManagedCleanup(peer);
    }
    return;
  }
  if(!current(peer,body))return;
  const auto key=m_display->currentKey();
  if(type=="cleaned"){
    if(body.value("cleanupVerified").isBool()){m_havePeerCleanup=true;m_peerCleanup=body.value("cleanupVerified").toBool();finishCleanup();}return;
  }
  if(type=="accepted")m_display->peerResponse(key,true,Reason::None);
  else if(type=="rejected")m_display->peerResponse(key,false,reason(body.value("reasonCode")));
  else if(type=="prepare"&&m_receiverReady)m_display->peerReady(key);
  else if(type=="stop"||type=="cancel"){
    m_peerTermination=true;
    const auto cause=body.value("reasonCode").toString();
    const bool cancel=type=="cancel"||(cause!="UserStopped"&&cause!="ApplicationExit"&&cause!="ConnectionLost"&&cause!="SessionUnavailable"&&cause!="IdentityChanged"&&cause!="InputPermissionDenied"&&cause!="BluetoothOnlyMode");
    m_usage->peerStopped(peer,cancel);m_display->stopManagedSession(key,reason(body.value("reasonCode")));
  }
}
void ProductDisplayRuntime::effect(const QJsonObject& value){
  const auto op=value.value("operation").toString();const auto snapshot=m_display->snapshot();
  if(op=="SendRequest"){
    m_peer=snapshot.value("peerId").toString();m_wire={};for(auto key:{"sessionId","requestId","generation","displayEpoch"})m_wire.insert(key,value.value(key));
    m_displayBase=m_connections.value(m_peer).value("baseConnectionId").toString();
    m_haveLocalCleanup=m_havePeerCleanup=m_localCleanup=m_peerCleanup=false;m_localResourceGone=false;m_peerTermination=false;m_cleanupReply={};
    auto request=wire("request");
    if(m_screens){
      const auto selection=m_screens->selectionForPeer(m_peer);
      if(selection.isEmpty()){m_display->stopManagedSession(m_display->currentKey(),Reason::CapabilityCheckRequired);return;}
      request.insert("screenSelection",selection);
    }
    send(m_peer,request);
  }else if(op=="SendAccepted"){
    const auto outcome=m_bridge->adoptIncoming();
    if(!outcome.accepted)m_display->stopManagedSession(m_display->currentKey(),outcome.reason);
  }else if(op=="SendRejected"){auto message=wire("rejected");message.insert("reasonCode",value.value("reasonCode"));send(m_peer,message);}
  else if((op=="StopPeer"||op=="CancelRequest")&&!m_peerTermination){auto message=wire(op=="StopPeer"?"stop":"cancel");message.insert("reasonCode",value.value("reasonCode"));send(m_peer,message);}
}
QString ProductDisplayRuntime::proofKey(const QString& peer,const QJsonObject& value)const{
  return peer+":"+value.value("sessionId").toString()+":"+value.value("requestId").toString()+":"+QString::number(value.value("displayEpoch").toInt());
}
void ProductDisplayRuntime::rememberProof(const QString& peer,const QJsonObject& value){
  if(session(value.value("sessionId"))==SessionId{}||session(value.value("requestId"))==SessionId{})return;
  if(m_cleanupProofs.size()>=128)m_cleanupProofs.erase(m_cleanupProofs.begin());m_cleanupProofs.insert(proofKey(peer,value));
}
void ProductDisplayRuntime::resourceState(const QJsonObject& value){
  const auto peer=value.value("peerId").toString(),state=value.value("state").toString();if(peer.size()!=64)return;
  if(state=="Preparing"||state=="Streaming"||state=="Stopping"||state=="CleanupRequired"){
    m_resourcePeer=peer;
    if(m_peer.isEmpty()){m_peer=peer;for(auto key:{"sessionId","requestId","generation","displayEpoch"})m_wire.insert(key,value.value(key));}
    if(state=="Preparing"||state=="Streaming")m_localResourceGone=false;
    if(!UsageInputState::setDisplayOwned(peer,!m_localResourceGone,m_inputStateFile))m_display->stopManagedSession(m_display->currentKey(),Reason::CleanupFailed);
  }else if(state=="Idle"&&value.value("cleanupStatus")=="verified"){
    UsageInputState::setDisplayOwned(peer,false,m_inputStateFile);rememberProof(peer,value);if(m_resourcePeer==peer)m_resourcePeer.clear();
  }
}
void ProductDisplayRuntime::localCleanup(const QJsonObject& value){
  const auto key=m_display->currentKey();
  if(session(value.value("sessionId"))!=key.session||session(value.value("requestId"))!=key.request||value.value("generation").toString()!=QString::number(key.generation))return;
  if(m_cleanupKey!=key&&m_havePeerCleanup&&!m_peerCleanup)m_havePeerCleanup=false;
  m_cleanupKey=key;m_haveLocalCleanup=true;m_localCleanup=value.value("cleanupVerified").toBool();
  if(m_localCleanup){m_localResourceGone=true;UsageInputState::setDisplayOwned(m_peer,false,m_inputStateFile);rememberProof(m_peer,value);}
  auto message=wire("cleaned");message.insert("cleanupVerified",m_localCleanup);send(m_peer,message);
  if(!m_cleanupReply.isEmpty()){
    auto response=m_cleanupReply;m_cleanupReply={};response.insert("type","cleaned");response.insert("cleanupVerified",m_localCleanup);send(m_peer,response);
  }
  if(m_localCleanup){auto probe=wire("cleanupProbe");probe.insert("cleanupVerified",true);send(m_peer,probe);}
  finishCleanup();
}
void ProductDisplayRuntime::finishCleanup(){
  if(!m_haveLocalCleanup||m_display->currentKey()!=m_cleanupKey)return;
  const auto state=m_display->snapshot().value("state").toString();if(state!="Stopping"&&state!="CleanupRequired")return;
  const bool sameConnection=!m_displayBase.isEmpty()&&m_connections.value(m_peer).value("baseConnectionId")==m_displayBase;
  if(!m_localCleanup||(sameConnection&&m_havePeerCleanup&&!m_peerCleanup)){m_display->cleanupFinished(m_cleanupKey,false);return;}
  // A disconnected base cannot authorize restoration; verify local ownership
  // independently so cleanup never depends on automatically reconnecting.
  if(!sameConnection||m_peerCleanup)m_display->cleanupFinished(m_cleanupKey,true);
}

}
