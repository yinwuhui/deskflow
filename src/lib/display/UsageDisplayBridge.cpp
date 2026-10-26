// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "UsageDisplayBridge.h"
#include <QTimer>
#include <stdexcept>
namespace deskflow::display {
namespace {
SessionId id(const QString& text){auto bytes=QByteArray::fromHex(text.toLatin1());SessionId result{};if(bytes.size()==16)std::copy(bytes.begin(),bytes.end(),result.begin());return result;}
}
UsageDisplayBridge::UsageDisplayBridge(PeerUsageService* usage,DisplayService* display,QObject* parent)
 :QObject(parent),m_usage(usage),m_display(display){
  if(!usage||!display||usage->thread()!=thread()||display->thread()!=thread())throw std::invalid_argument("bridge owners");
  connect(usage,&PeerUsageService::backendCommand,this,&UsageDisplayBridge::effect);
  connect(display,&DisplayService::snapshotChanged,this,[this]{QTimer::singleShot(0,this,[this]{displayChanged();});});
}
Outcome UsageDisplayBridge::adoptCleanup(){
  const auto snapshot=m_display->snapshot();
  if(m_active||snapshot.value("state")!="CleanupRequired")return {false,Reason::InvalidRequest};
  m_peer=snapshot.value("peerId").toString();m_request=m_display->currentKey().request;
  auto result=m_usage->restoreDisplayCleanup({m_peer.toStdString(),snapshot.value("peerName").toString().toStdString(),{}},m_request);
  if(result.accepted){m_active=true;m_incoming=snapshot.value("localDisplayRole")=="receiver";
    const auto usage=m_usage->snapshot(m_peer);m_usageKey={id(usage.value("operationId").toString()),usage.value("operationGeneration").toString().toULongLong()};}
  return result;
}
Outcome UsageDisplayBridge::adoptIncoming(){
  if(m_active)return {false,Reason::PeerBusy};
  const auto snapshot=m_display->snapshot();
  if(snapshot.value("localDisplayRole")!="receiver"||snapshot.value("state")!="Authorizing")return {false,Reason::InvalidRequest};
  m_active=true;m_incoming=true;m_peer=snapshot.value("peerId").toString();m_request=m_display->currentKey().request;
  auto result=m_usage->beginIncomingDisplay(m_peer,m_request);
  const auto usage=m_usage->snapshot(m_peer);
  m_usageKey={id(usage.value("operationId").toString()),usage.value("operationGeneration").toString().toULongLong()};
  if(!result.accepted){m_active=false;m_incoming=false;}
  return result;
}
void UsageDisplayBridge::effect(const QJsonObject& command){
  auto operation=command.value("operation").toString();auto peer=command.value("peerId").toString();
  UsageKey key{id(command.value("operationId").toString()),command.value("operationGeneration").toString().toULongLong()};
  if(operation=="RequestDisplayPermission"){
    if(m_active&&m_incoming&&m_peer==peer&&key.operation==m_request){
      m_usageKey=key;m_usage->complete(peer,key,PeerUsageService::Completion::Permission);return;
    }
    if(m_active){m_usage->complete(peer,key,PeerUsageService::Completion::Permission,Reason::PeerBusy);return;}
    m_active=true;m_incoming=false;m_peer=peer;m_usageKey=key;m_request=key.operation;
    auto outcome=m_display->requestManagedSession(peer,m_request);
    if(!outcome.accepted){m_active=false;m_usage->complete(peer,key,PeerUsageService::Completion::Permission,outcome.reason);}
  }else if(operation=="StartDisplay"){
    if(!m_active||m_peer!=peer||m_usageKey!=key){m_usage->complete(peer,key,PeerUsageService::Completion::DisplayFailed,Reason::InvalidRequest,true);return;}
    if(m_incoming){Q_EMIT receiverInputReady();return;}
    auto outcome=m_display->resumeManagedSession(m_display->currentKey());
    if(!outcome.accepted){m_usage->complete(peer,key,PeerUsageService::Completion::DisplayFailed,outcome.reason,false);}
  }else if(operation=="StopDisplay"||operation=="CancelDisplayRequest"){
    if(!m_active||m_peer!=peer){
      if(operation=="StopDisplay")m_usage->complete(peer,key,PeerUsageService::Completion::DisplayStopped,Reason::None,true);
      return;
    }
    m_usageKey=key;
    if(m_display->snapshot().value("state")=="CleanupRequired")m_display->retryManagedCleanup(m_display->currentKey());
    else m_display->stopManagedSession(m_display->currentKey(),command.value("reasonCode")=="BluetoothOnlyMode"?Reason::BluetoothOnlyMode:command.value("reasonCode")=="UserStopped"?Reason::UserStopped:Reason::Cancelled);
    displayChanged();
  }
}
void UsageDisplayBridge::displayChanged(){
  if(!m_active)return;auto display=m_display->snapshot();auto key=m_display->currentKey();
  if(key.request!=m_request||display.value("peerId")!=m_peer)return;
  const auto state=display.value("state").toString();
  const auto usage=m_usage->snapshot(m_peer);const auto stage=usage.value("switchStage").toString();
  if(!m_incoming&&state=="Authorizing" && display.value("permissions").toObject().value("capture")=="granted"){
    m_usage->complete(m_peer,m_usageKey,PeerUsageService::Completion::Permission);return;
  }
  if(state=="Streaming"&&display.value("firstFramePresented").toBool()){
    m_usage->complete(m_peer,m_usageKey,PeerUsageService::Completion::DisplayPresented);return;
  }
  if(state!="Idle"&&state!="CleanupRequired")return;
  auto peer=m_peer;auto usageKey=m_usageKey;const bool verified=state=="Idle"&&display.value("cleanupStatus")=="verified";
  Reason reason=Reason::SwitchFailed;
  auto reasonText=display.value("terminalReasonCode").toString();
  for(auto r:{Reason::BluetoothOnlyMode,Reason::PeerRejected,Reason::RequestExpired,Reason::Cancelled,Reason::PermissionDenied,Reason::SessionUnavailable,Reason::CapabilityCheckRequired,Reason::UnsupportedPlatform,Reason::NetworkUnavailable,Reason::ConnectionLost,Reason::UserStopped,Reason::CleanupFailed,Reason::PresentationFailed})
    if(reasonText==reasonName(r))reason=r;
  // Clear ownership before callbacks: a callback can schedule the next intent.
  if(verified)m_active=false;
  if(stage=="AwaitingPermission")m_usage->complete(peer,usageKey,PeerUsageService::Completion::Permission,reason);
  else if(stage=="StartingDisplay")m_usage->complete(peer,usageKey,PeerUsageService::Completion::DisplayFailed,reason,verified);
  else if(stage=="StoppingDisplay"||stage=="CleaningDisplay")m_usage->complete(peer,usageKey,PeerUsageService::Completion::DisplayStopped,reason,verified);
  else if(usage.value("effectiveMode")=="extended")m_usage->complete(peer,usageKey,PeerUsageService::Completion::DisplayEnded,reason,verified);
}
}
