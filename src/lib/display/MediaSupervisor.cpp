// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "MediaSupervisor.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSslCertificate>
#include <QCryptographicHash>
#include <algorithm>
namespace deskflow::display {
namespace {
QString hex(const SessionId& id) { return QByteArray(reinterpret_cast<const char*>(id.data()),16).toHex(); }
Key parseKey(const QJsonObject& object) {
  Key key;auto session=QByteArray::fromHex(object.value("sessionId").toString().toLatin1());
  auto request=QByteArray::fromHex(object.value("requestId").toString().toLatin1());
  if(session.size()!=16||request.size()!=16)return {};
  std::copy(session.begin(),session.end(),key.session.begin());std::copy(request.begin(),request.end(),key.request.begin());
  key.generation=object.value("generation").toString().toULongLong();key.epoch=static_cast<uint32_t>(object.value("displayEpoch").toDouble());return key;
}
QJsonObject identity(const Key& key) {
  return {{"sessionId",hex(key.session)},{"requestId",hex(key.request)},
    {"generation",QString::number(key.generation)},{"displayEpoch",static_cast<double>(key.epoch)}};
}
bool executable(const QString& path) { return QFileInfo(path).isAbsolute() && QFileInfo(path).isFile() && QFileInfo(path).isExecutable(); }
}
MediaSupervisor::MediaSupervisor(DisplayService* service,MediaRuntime runtime,const QString& resources,QObject* parent)
  :QObject(parent),m_service(service),m_runtime(std::move(runtime)),m_journal(resources) {
  connect(service,&DisplayService::backendCommand,this,&MediaSupervisor::command);
  m_forceTimer.setSingleShot(true);
  connect(&m_forceTimer,&QTimer::timeout,this,[this]{if(m_process&&m_process->state()!=QProcess::NotRunning)m_process->kill();});
  if(m_journal.status()==ResourceJournal::Status::Pending){
    const auto& record=*m_journal.record();
    Peer peer{record.peerIdentity.toStdString(),"Previous display session",{}};
    service->restorePendingResource(peer,record.owner,record.role,record.displayId);
  }
}
MediaSupervisor::~MediaSupervisor(){
  // Abrupt parent exit is also watched by the media process and native helper.
  // Keep the dirty ownership record; never report cleanup from process death alone.
  if(m_process&&m_process->state()!=QProcess::NotRunning)m_process->kill();
}
Outcome MediaSupervisor::addTrustedEndpoint(TrustedMediaEndpoint endpoint) {
  if(!m_service || m_service->snapshot().value("state")!="Idle")return {false,Reason::PeerBusy};
  const auto certs=QSslCertificate::fromPath(endpoint.certificate);
  if(certs.size()!=1 || certs.first().digest(QCryptographicHash::Sha256).toHex().toStdString()!=endpoint.peer.id ||
     endpoint.address.isEmpty() || endpoint.address.size()>253 || !endpoint.port)return {false,Reason::IdentityChanged};
  const auto id=QString::fromStdString(endpoint.peer.id);m_service->authenticatedPeer(endpoint.peer);
  m_endpoints.insert(id,std::move(endpoint));return {true,Reason::None};
}
void MediaSupervisor::command(const QJsonObject& value) {
  if(!m_service)return;
  const auto op=value.value("operation").toString();const auto key=parseKey(value);
  if(op=="PrepareHost")start(key,Role::Host);
  else if(op=="PrepareReceiver")start(key,Role::Receiver);
  else if(op=="CleanupResources")cleanup(key);
}
void MediaSupervisor::start(const Key& key,Role role) {
  if(!m_service || parseKey(m_service->snapshot())!=key || m_process)return;
  const auto peer=m_service->snapshot().value("peerId").toString();
  if(!m_endpoints.contains(peer)){m_service->backendFailed(key,Reason::IdentityChanged);return;}
  if(!executable(m_runtime.python)||!QFileInfo(m_runtime.sessionScript).isAbsolute()||!QFileInfo(m_runtime.sessionScript).isFile()||
     (role==Role::Receiver&&!executable(m_runtime.nativeViewer))||
     (role==Role::Host&&m_runtime.source!="gnome"&&m_runtime.source!="macos")||
     (role==Role::Host&&m_runtime.source=="macos"&&!executable(m_runtime.macosProbe))){
    m_service->backendFailed(key,Reason::UnsupportedPlatform);return;
  }
  OwnedResource record{key,peer,role,{}};
  if(!m_journal.claim(record)){m_service->backendFailed(key,Reason::CleanupFailed);return;}
  m_owner=key;m_stdout.clear();m_stderr.clear();m_cleanupVerified=false;m_exited=false;
  auto process=new QProcess(this);m_process=process;
  const auto endpoint=m_endpoints.value(peer);
  QStringList args{m_runtime.sessionScript,role==Role::Host?"serve":"receive","--identity",m_runtime.identityDirectory,
    "--peer-cert",endpoint.certificate,"--product-events","--parent-pid",QString::number(QCoreApplication::applicationPid()),
    "--session-id",hex(key.session),"--request-id",hex(key.request)};
  if(role==Role::Host){
    args<<"--source"<<m_runtime.source<<"--duration"<<"3600"<<"--bind"<<m_runtime.bindAddress<<"--port"<<QString::number(m_runtime.port);
    if(m_runtime.source=="macos")args<<"--macos-probe"<<m_runtime.macosProbe;
  } else args<<"--host"<<endpoint.address<<"--port"<<QString::number(endpoint.port)<<"--native-viewer"<<m_runtime.nativeViewer;
  process->setProcessEnvironment(m_runtime.environment);
  process->setProgram(m_runtime.python);process->setArguments(args);
  connect(process,&QProcess::readyReadStandardError,this,[this,process,key]{consume(process,key);});
  connect(process,&QProcess::readyReadStandardOutput,this,[this,process,key]{
    m_stdout+=process->readAllStandardOutput();
    if(m_stdout.size()>65536 && m_service){m_service->backendFailed(key,Reason::InvalidRequest);process->kill();}
  });
  connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,process,key,role](int,QProcess::ExitStatus){finish(process,key,role);});
  connect(process,&QProcess::errorOccurred,this,[this,process,key](QProcess::ProcessError error){
    if(error==QProcess::FailedToStart){m_exited=true;m_cleanupVerified=true;
      if(m_service){m_service->backendFailed(key,Reason::DisplayCreationFailed);finishCleanup(parseKey(m_service->snapshot()),true);}
      m_process=nullptr;process->deleteLater();}
  });
  process->start();
}
void MediaSupervisor::consume(QProcess* process,const Key& owner) {
  if(!m_service || process!=m_process || owner!=m_owner)return;
  m_stderr+=process->readAllStandardError();
  if(m_stderr.size()>65536){m_service->backendFailed(owner,Reason::InvalidRequest);process->kill();m_stderr.clear();return;}
  while(m_stderr.contains('\n')){
    const auto end=m_stderr.indexOf('\n');const auto line=m_stderr.left(end);m_stderr.remove(0,end+1);
    const auto doc=QJsonDocument::fromJson(line);if(!doc.isObject())continue;
    const auto event=doc.object();const auto name=event.value("event").toString();
    if(name=="listening"){
      auto descriptor=identity(owner);descriptor.insert("port",event.value("port"));Q_EMIT listening(descriptor);
    } else if(name=="displayCreated"){
      const auto id=event.value("virtualDisplayId").toString();
      if(id.isEmpty()||!m_journal.updateDisplay(owner,id)){m_service->backendFailed(owner,Reason::CleanupFailed);continue;}
      m_service->backendPrepared(owner,id);
    } else if(name=="receiverPrepared")m_service->backendPrepared(owner,{});
    else if(name=="firstFramePresented")m_service->firstFramePresented(owner);
    else if(name=="viewerClosed")m_service->execute({{"id","viewer-close"},{"operation","stopDisplaySession"},{"args",identity(parseKey(m_service->snapshot()))}});
    else if(name=="sourceStopped" || name=="receiverStopped")m_cleanupVerified=event.value("cleanupVerified").isBool()&&event.value("cleanupVerified").toBool();
  }
}
void MediaSupervisor::finish(QProcess* process,const Key& owner,Role) {
  if(process!=m_process || owner!=m_owner)return;
  consume(process,owner);m_stdout+=process->readAllStandardOutput();m_exited=true;m_forceTimer.stop();
  if(m_service){
    const auto current=m_service->snapshot();
    if(current.value("state")!="Stopping"&&current.value("state")!="CleanupRequired")
      m_service->backendFailed(owner,Reason::ConnectionLost);
    finishCleanup(parseKey(m_service->snapshot()),m_cleanupVerified);
  }
  m_process=nullptr;process->deleteLater();
}
void MediaSupervisor::finishCleanup(const Key& key,bool verified) {
  if(!m_service)return;
  if(verified && m_journal.record())verified=m_journal.clearVerified(m_journal.record()->owner,true);
  m_service->cleanupFinished(key,verified);
}
void MediaSupervisor::cleanup(const Key& key) {
  if(!m_service || parseKey(m_service->snapshot())!=key)return;
  if(!m_process){verifyResidual(key);return;}
  if(key.session!=m_owner.session || key.request!=m_owner.request)return;
  if(m_process->state()==QProcess::NotRunning){finishCleanup(key,m_cleanupVerified);return;}
  m_process->terminate();
  const auto reason=m_service->snapshot().value("terminalReasonCode").toString();
  m_forceTimer.start(reason=="UserStopped"||reason=="Cancelled"||reason=="ApplicationExit"?4000:14000);
}
void MediaSupervisor::verifyResidual(const Key& cleanupKey) {
  // A surviving output may have been reused by another application. This adapter
  // only verifies absence; it never deletes an output or kills a persisted PID.
  if(!m_journal.record()){m_service->cleanupFinished(cleanupKey,m_journal.status()==ResourceJournal::Status::Empty);return;}
  const auto record=*m_journal.record();
  if(record.displayId.isEmpty()){m_service->cleanupFinished(cleanupKey,false);return;}
  auto process=new QProcess(this);QStringList args;
  if(m_runtime.source=="macos")process->setProgram(m_runtime.macosProbe);
  else {process->setProgram(m_runtime.python);args<<QFileInfo(m_runtime.sessionScript).dir().filePath("virtual_display_probe.py")<<"--query-topology";}
  process->setArguments(args);process->setProcessEnvironment(m_runtime.environment);
  auto output=std::make_shared<QByteArray>();
  connect(process,&QProcess::readyReadStandardOutput,this,[process,output]{*output+=process->readAllStandardOutput();if(output->size()>65536)process->kill();});
  auto deadline=new QTimer(process);deadline->setSingleShot(true);deadline->start(4000);
  connect(deadline,&QTimer::timeout,process,&QProcess::kill);
  connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,process,output,record,cleanupKey](int code,QProcess::ExitStatus){
    *output+=process->readAllStandardOutput();const auto doc=QJsonDocument::fromJson(*output);
    bool verified=code==0&&doc.isObject();
    if(verified){
      const auto values=doc.object().value(m_runtime.source=="macos"?"displaysBefore":"displayIds");
      verified=values.isArray();
      for(const auto& id:values.toArray())if((id.isString()?id.toString():QString::number(id.toDouble(),'f',0))==record.displayId)verified=false;
    }
    if(m_service&&parseKey(m_service->snapshot())==cleanupKey)finishCleanup(cleanupKey,verified);
    process->deleteLater();
  });
  connect(process,&QProcess::errorOccurred,this,[this,process,cleanupKey](QProcess::ProcessError error){
    if(error==QProcess::FailedToStart){if(m_service)m_service->cleanupFinished(cleanupKey,false);process->deleteLater();}
  });
  process->start();
}
} // namespace deskflow::display
