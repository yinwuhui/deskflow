// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "InputRouteAdapter.h"
#include <QUuid>
#include <limits>
namespace deskflow::display {
namespace {
SessionId parse(const QString& value){auto bytes=QByteArray::fromHex(value.toLatin1());SessionId result{};if(bytes.size()==16)std::copy(bytes.begin(),bytes.end(),result.begin());return result;}
QString hex(const SessionId& value){return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(value.data()),16).toHex());}
}
InputRouteAdapter::InputRouteAdapter(PeerUsageService* usage,DisplayService* display,QObject* parent)
 :QObject(parent),m_usage(usage),m_display(display){
  connect(usage,&PeerUsageService::backendCommand,this,&InputRouteAdapter::effect);
  m_timer.setInterval(100);connect(&m_timer,&QTimer::timeout,this,&InputRouteAdapter::tick);m_timer.start();
}
void InputRouteAdapter::send(const QString& peer,Pending& p,const QString& kind){
  p.kind=kind;p.requestId=QUuid::createUuid().toString(QUuid::WithoutBraces);
  Q_EMIT coreCommand(QJsonObject{{"id",p.requestId},{"kind",kind},{"peerId",peer},{"sequence",QString::number(p.sequence)},
    {"baseConnectionId",p.connectionId},{"baseConnectionGeneration",p.connectionGeneration},{"operationId",hex(p.key.operation)},{"operationGeneration",QString::number(p.key.generation)},{"enabled",p.enabled}});
}
void InputRouteAdapter::effect(const QJsonObject& command){
  const auto effect=command.value("operation").toString();
  if(effect!="PreflightInput"&&effect!="PreflightDisplay"&&effect!="RequestInputPermission"&&effect!="ReleaseInputs"&&
    effect!="StopInput"&&effect!="StartInput"&&effect!="RestoreInput"&&effect!="RevokeInput"&&effect!="VerifyRollbackAuthorization")return;
  auto peer=command.value("peerId").toString();UsageKey key{parse(command.value("operationId").toString()),command.value("operationGeneration").toString().toULongLong()};
  auto existing=m_pending.find(peer);
  if(existing!=m_pending.end()&&existing->second.key==key&&existing->second.effect=="RevokeInput"&&effect=="ReleaseInputs")return;
  Pending pending;pending.key=key;pending.effect=effect;pending.enabled=effect=="StartInput"||effect=="RestoreInput";
  const auto binding=m_usage->snapshot(peer);pending.connectionId=binding.value("baseConnectionId").toString();pending.connectionGeneration=binding.value("baseConnectionGeneration").toString();
  pending.elapsed.start();
  if(effect=="StopInput"){
    auto released=m_released.find(peer);
    if(released==m_released.end()||released->second.first!=key){m_usage->complete(peer,key,PeerUsageService::Completion::InputStopped,Reason::InputReleaseFailed,false);return;}
    pending.sequence=released->second.second;
  }
  m_pending[peer]=pending;send(peer,m_pending.at(peer),"status");
}
void InputRouteAdapter::fail(const QString& peer,Reason reason){
  auto it=m_pending.find(peer);if(it==m_pending.end())return;auto p=it->second;m_pending.erase(it);
  if(p.effect=="PreflightInput"||p.effect=="PreflightDisplay")m_usage->complete(peer,p.key,PeerUsageService::Completion::Preflight,reason);
  else if(p.effect=="RequestInputPermission")m_usage->complete(peer,p.key,PeerUsageService::Completion::Permission,reason);
  else if(p.effect=="VerifyRollbackAuthorization")m_usage->complete(peer,p.key,PeerUsageService::Completion::RollbackAuthorization,reason,false);
  else if(p.effect=="ReleaseInputs")m_usage->complete(peer,p.key,PeerUsageService::Completion::InputsReleased,reason,false);
  else if(p.effect=="StopInput")m_usage->complete(peer,p.key,PeerUsageService::Completion::InputStopped,reason,false);
  else if(p.effect=="StartInput"||p.effect=="RestoreInput")m_usage->complete(peer,p.key,PeerUsageService::Completion::InputStarted,reason,false);
  // Failed revocation deliberately keeps CleanupRequired. No false cleanup ack.
}
void InputRouteAdapter::coreResult(const QJsonObject& result){
  auto peer=result.value("peerId").toString();auto it=m_pending.find(peer);if(it==m_pending.end())return;auto& p=it->second;
  if(result.value("baseConnectionId")!=p.connectionId||result.value("baseConnectionGeneration")!=p.connectionGeneration||result.value("id")!=p.requestId||result.value("operationId")!=hex(p.key.operation)||
    result.value("operationGeneration")!=QString::number(p.key.generation)||result.value("sequence")!=QString::number(p.sequence)||
    result.value("kind")!=p.kind||result.value("enabled")!=p.enabled)return;
  if(!result.value("accepted").isBool()||!result.value("connected").isBool()||!result.value("supported").isBool())return;
  auto current=m_usage->snapshot(peer);
  if(current.value("operationId")!=hex(p.key.operation)||current.value("operationGeneration")!=QString::number(p.key.generation)){m_pending.erase(it);return;}
  if(!result.value("accepted").toBool()||!result.value("connected").toBool()||!result.value("supported").toBool()){
    fail(peer,!result.value("connected").toBool()?Reason::ConnectionLost:Reason::InputIsolationUnsupported);return;
  }
  for(auto field:{"suspended","localFocus","acknowledged","acknowledgementReceived","inputPermissionsReady","otherRoutesIsolated"})if(!result.value(field).isBool())return;
  bool numberValid=false;auto last=result.value("lastSequence").toString().toULongLong(&numberValid);
  if(!numberValid||last>std::numeric_limits<uint32_t>::max())return;
  m_sequence[peer]=std::max(m_sequence[peer],static_cast<uint32_t>(last));
  const bool local=result.value("localFocus").toBool();auto key=p.key;auto effect=p.effect;
  if(effect=="PreflightInput"||effect=="PreflightDisplay"){
    const bool isolated=effect!="PreflightDisplay"||result.value("otherRoutesIsolated").toBool();
    if(effect=="PreflightDisplay"&&isolated&&result.value("suspended").toBool())m_display->setInputRouting(Routing::Clear);
    m_pending.erase(it);m_usage->complete(peer,key,PeerUsageService::Completion::Preflight,isolated?Reason::None:Reason::InputIsolationUnsupported);return;
  }
  if(p.kind=="status" && p.sequence==0){
    if(m_sequence[peer]==std::numeric_limits<uint32_t>::max()){fail(peer,Reason::SwitchFailed);return;}
    p.sequence=++m_sequence[peer];
    const bool probe=effect=="RequestInputPermission"||effect=="VerifyRollbackAuthorization";
    send(peer,p,probe?"probe":p.enabled?"resume":"suspend");return;
  }
  if(effect=="RequestInputPermission"||effect=="VerifyRollbackAuthorization"){
    if(!result.value("acknowledgementReceived").toBool())return;
    const bool ready=result.value("inputPermissionsReady").toBool();m_pending.erase(it);
    m_usage->inputStatus(peer,key,ready?Permission::Granted:Permission::Denied,local?"local":QString());
    m_usage->complete(peer,key,effect=="RequestInputPermission"?PeerUsageService::Completion::Permission:PeerUsageService::Completion::RollbackAuthorization,
      ready?Reason::None:Reason::InputPermissionDenied,ready);return;
  }
  if(!result.value("acknowledgementReceived").toBool())return;
  if(!result.value("acknowledged").toBool()||!local){fail(peer,Reason::InputReleaseFailed);return;}
  if(effect=="ReleaseInputs"||effect=="RevokeInput"||effect=="StopInput"){
    if(!result.value("suspended").toBool()){fail(peer,Reason::InputReleaseFailed);return;}
    m_released[peer]={key,p.sequence};m_pending.erase(it);
    // Preserve permission truth; release itself cannot grant input permission.
    const auto permission=current.value("inputPermissionsReady").toBool()?Permission::Granted:Permission::Unknown;
    m_usage->inputStatus(peer,key,permission,"local");
    if(effect=="StopInput"&&result.value("otherRoutesIsolated").toBool())m_display->setInputRouting(Routing::Clear);
    m_usage->complete(peer,key,effect=="StopInput"?PeerUsageService::Completion::InputStopped:effect=="RevokeInput"?
      (current.value("usageState")=="CleanupRequired"?PeerUsageService::Completion::InputCleaned:PeerUsageService::Completion::InputsReleased):PeerUsageService::Completion::InputsReleased,Reason::None,true);return;
  }
  if(!p.committing){p.committing=true;send(peer,p,"commitResume");return;}
  if(result.value("suspended").toBool()){fail(peer,Reason::InputReleaseFailed);return;}
  m_pending.erase(it);m_released.erase(peer);m_display->setInputRouting(Routing::ConflictingPeer);
  m_usage->complete(peer,key,PeerUsageService::Completion::InputStarted,Reason::None,true);
}
void InputRouteAdapter::tick(){
  std::vector<QString> peers;for(const auto& [peer,pending]:m_pending)peers.push_back(peer);
  for(const auto& peer:peers){auto it=m_pending.find(peer);if(it==m_pending.end())continue;
    if(it->second.elapsed.elapsed()>=4000){fail(peer,Reason::InputReleaseFailed);continue;}
    // Poll with a fresh request id; delayed replies from a previous poll are
    // ignored rather than being allowed to complete a newer operation.
    send(peer,it->second,"status");
  }
}
void InputRouteAdapter::coreUnavailable(){std::vector<QString> peers;for(const auto& [peer,p]:m_pending)peers.push_back(peer);for(const auto& peer:peers)fail(peer,Reason::ConnectionLost);}
}
