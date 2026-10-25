// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "PeerUsageService.h"
#include "UsageDisplayBridge.h"
#include "CoreInputChannel.h"
#include <QCoreApplication>
#include <QPointer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <stdexcept>
namespace deskflow::display {
namespace {
QString hex(const SessionId& id){return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(id.data()),16).toHex());}
SessionId parseId(const QJsonValue& value){
  if(!value.isString())throw std::invalid_argument("operationId");
  auto text=value.toString();if(text.size()!=32)throw std::invalid_argument("operationId");
  for(auto c:text)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))throw std::invalid_argument("operationId");
  auto bytes=QByteArray::fromHex(text.toLatin1());SessionId id{};std::copy(bytes.begin(),bytes.end(),id.begin());
  if(id==SessionId{})throw std::invalid_argument("zero operationId");return id;
}
SessionId randomId(){auto bytes=QUuid::createUuid().toRfc4122();SessionId id{};std::copy(bytes.begin(),bytes.end(),id.begin());return id;}
void fields(const QJsonObject& object,QSet<QString> required,QSet<QString> optional={}){
  for(const auto& key:required)if(!object.contains(key))throw std::invalid_argument("missing field");
  for(auto it=object.begin();it!=object.end();++it)if(!required.contains(it.key())&&!optional.contains(it.key()))throw std::invalid_argument("unknown field");
}
UsageMode mode(const QJsonValue& value){if(value=="input")return UsageMode::Input;if(value=="extended")return UsageMode::Extended;throw std::invalid_argument("mode");}
bool validPeer(const std::string& value){if(value.size()!=64)return false;for(auto c:value)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;return true;}
QJsonObject keyJson(const UsageKey& key){return {{"operationId",hex(key.operation)},{"operationGeneration",QString::number(key.generation)}};}
const char* permissionName(Permission value){switch(value){case Permission::Granted:return "granted";case Permission::Denied:return "denied";case Permission::Required:return "required";case Permission::Unknown:return "unknown";}return "unknown";}
}
PeerUsageService::PeerUsageService(QObject* parent):QObject(parent){
  m_timer.setInterval(100);connect(&m_timer,&QTimer::timeout,this,[this]{
    for(auto& [identity,entry]:m_peers){entry.coordinator->tick(Coordinator::Clock::now());publish(identity);}
  });m_timer.start();
}
PeerUsageService* PeerUsageService::applicationService(){
  auto app=QCoreApplication::instance();
  if(!app||app->thread()!=QThread::currentThread())throw std::logic_error("Usage service needs application owner thread");
  static QPointer<PeerUsageService> service;
  if(!service){
    service=new PeerUsageService(app);auto display=DisplayService::applicationService();
    new UsageDisplayBridge(service,display,service);
    auto input=new InputRouteAdapter(service,display,service);new CoreInputChannel(input,input);
  }
  return service;
}
void PeerUsageService::ownerThread()const{if(QThread::currentThread()!=thread())throw std::logic_error("Usage service called off owner thread");}
void PeerUsageService::authenticatedPeer(Peer peer,UsageCapabilities caps){
  ownerThread();if(!validPeer(peer.id)||peer.name.size()>256)throw std::invalid_argument("authenticated peer");
  auto identity=QString::fromStdString(peer.id);auto it=m_peers.find(identity);
  if(it==m_peers.end()){
    Entry entry{peer,caps,std::make_unique<UsageCoordinator>(peer.id,caps),0};m_peers.emplace(identity,std::move(entry));
  }else{it->second.peer=std::move(peer);it->second.capabilities=caps;it->second.coordinator->setCapabilities(caps);it->second.published=0;}
  publish(identity);
}
void PeerUsageService::disconnected(const QString& peerId){ownerThread();auto it=m_peers.find(peerId);if(it==m_peers.end())return;it->second.capabilities={};it->second.coordinator->setCapabilities({});it->second.coordinator->connectionLost();publish(peerId);}
QJsonObject PeerUsageService::capabilities(const Entry& entry)const{
  const auto& c=entry.capabilities;return {{"input",availabilityName(c.input)},{"keyboard",availabilityName(c.keyboard)},
    {"focusSync",availabilityName(c.focusSync)},{"extended",availabilityName(c.extended)},
    {"targetIsolation",availabilityName(c.targetIsolation)},{"inputRelease",availabilityName(c.inputRelease)},
    {"displayRouting",availabilityName(c.displayRouting)},{"networkAvailable",c.networkAvailable}};
}
QJsonObject PeerUsageService::snapshot(const QString& peerId)const{
  ownerThread();auto it=m_peers.find(peerId);if(it==m_peers.end())return {};const auto& e=it->second;const auto& s=e.coordinator->snapshot();
  auto result=keyJson(s.key);result.insert("peerId",peerId);result.insert("peerName",QString::fromStdString(e.peer.name));
  result.insert("revision",QString::number(s.revision));result.insert("selectedMode",usageModeName(s.selectedMode));
  result.insert("effectiveMode",usageModeName(s.effectiveMode));result.insert("targetMode",usageModeName(s.targetMode));
  result.insert("usageState",usageStateName(s.state));result.insert("switchStage",switchStageName(s.stage));
  result.insert("inputCapabilities",capabilities(e));result.insert("inputPermissionsReady",s.inputPermission==Permission::Granted);
  result.insert("inputFocusOwner",s.inputFocusOwner.empty()?QJsonValue(QJsonValue::Null):QJsonValue(QString::fromStdString(s.inputFocusOwner)));
  result.insert("pressedInputsReleased",s.pressedInputsReleased);result.insert("oldModeStopped",s.oldModeStopped);
  result.insert("rollbackResult",rollbackName(s.rollback));result.insert("reasonCode",reasonName(s.reason));
  result.insert("permissions",QJsonObject{{"input",permissionName(s.inputPermission)}});
  QJsonArray actions;
  if(s.state==UsageState::Inactive)actions.append("start");
  else if(s.state==UsageState::Switching){actions.append("cancel");actions.append("stop");}
  else if(s.state==UsageState::CleanupRequired)actions.append("details");
  else{actions.append("switch");actions.append("stop");}
  result.insert("availableActions",actions);return result;
}
void PeerUsageService::publish(const QString& peerId){
  auto it=m_peers.find(peerId);if(it==m_peers.end())return;auto& e=it->second;auto effects=e.coordinator->takeEffects();
  auto revision=e.coordinator->snapshot().revision;const auto stage=e.coordinator->snapshot().stage;
  if(e.published!=revision){e.published=revision;Q_EMIT snapshotChanged(snapshot(peerId));}
  for(auto effect:effects){
    QTimer::singleShot(0,this,[this,peerId,effect,stage]{
      auto it=m_peers.find(peerId);if(it==m_peers.end())return;const auto& s=it->second.coordinator->snapshot();
      // A snapshot listener can synchronously stop/cancel this operation. Never
      // execute queued start work from the stage that listener invalidated.
      if(s.key!=effect.key || s.stage!=stage)return;
      auto command=keyJson(effect.key);command.insert("peerId",peerId);command.insert("operation",usageEffectName(effect.kind));
      Q_EMIT backendCommand(command);
    });
  }
}
bool PeerUsageService::displayBusy(const QString& peerId)const{
  for(const auto& [identity,entry]:m_peers){if(identity==peerId)continue;const auto& s=entry.coordinator->snapshot();
    if(s.effectiveMode==UsageMode::Extended||s.targetMode==UsageMode::Extended||s.state==UsageState::CleanupRequired)return true;
  }return false;
}
void PeerUsageService::execute(const QJsonObject& command){QTimer::singleShot(0,this,[this,command]{run(command);});}
void PeerUsageService::run(const QJsonObject& command){
  ownerThread();QString correlation,peerId;Outcome outcome{false,Reason::InvalidRequest};QJsonObject data;
  try{
    if(QJsonDocument(command).toJson(QJsonDocument::Compact).size()>65536)throw std::invalid_argument("size");
    fields(command,{"id","operation","args"});correlation=command.value("id").toString();
    if(correlation.isEmpty()||correlation.size()>64||!command.value("operation").isString()||!command.value("args").isObject())throw std::invalid_argument("envelope");
    auto op=command.value("operation").toString();auto args=command.value("args").toObject();
    peerId=args.value("peerId").toString(m_selectedPeer);auto it=m_peers.find(peerId);
    if(it==m_peers.end()){outcome={false,Reason::NetworkUnavailable};}
    else{
      auto& c=*it->second.coordinator;
      if(op=="queryPeerUsageCapabilities"){fields(args,{"peerId"});data=capabilities(it->second);outcome={true,Reason::None};}
      else if(op=="getPeerUsageSnapshot"||op=="subscribePeerUsageEvents"||op=="getInputControlStatus"){
        fields(args,{"peerId"});data=snapshot(peerId);outcome={true,Reason::None};
      }else if(op=="selectPeer"){fields(args,{"peerId"});m_selectedPeer=peerId;outcome={true,Reason::None};}
      else if(op=="selectUsageMode"){fields(args,{"mode"},{"peerId"});outcome=c.select(mode(args.value("mode")));}
      else if(op=="startPeerUsage"||op=="switchPeerUsageMode"){
        const bool start=op=="startPeerUsage";
        fields(args,start?QSet<QString>{"peerId","mode","operationId"}:QSet<QString>{"peerId","targetMode","operationId"},start?QSet<QString>{"options"}:QSet<QString>{});
        // P0 has no mode-splitting options. In particular, reject all v1.1
        // shareKeyboard/keyboardEnabled fields instead of silently ignoring them.
        if(args.contains("options")&&(!args.value("options").isObject()||!args.value("options").toObject().isEmpty()))throw std::invalid_argument("options");
        auto target=mode(args.value(start?"mode":"targetMode"));auto operation=parseId(args.value("operationId"));
        if(start && c.snapshot().effectiveMode!=UsageMode::None)outcome={false,Reason::ModeConflict};
        else if(!start && c.snapshot().effectiveMode==UsageMode::None)outcome={false,Reason::ModeConflict};
        else if(target==UsageMode::Extended&&displayBusy(peerId))outcome={false,Reason::PeerBusy};
        else outcome=c.begin(target,operation);
      }else if(op=="cancelPeerUsageOperation"){
        fields(args,{"peerId","operationId","operationGeneration"});auto key=c.snapshot().key;
        if(hex(key.operation)!=args.value("operationId").toString()||QString::number(key.generation)!=args.value("operationGeneration").toString())throw std::invalid_argument("stale operation");
        outcome=c.cancel(key);
      }else if(op=="stopPeerUsage"){
        fields(args,{"peerId"},{"reason"});if(args.contains("reason")&&args.value("reason")!="UserStopped")throw std::invalid_argument("stop reason");outcome=c.stop(randomId());
      }
    }
  }catch(const std::invalid_argument&){outcome={false,Reason::InvalidRequest};}
  if(m_peers.contains(peerId))publish(peerId);
  Q_EMIT commandFinished(QJsonObject{{"id",correlation},{"accepted",outcome.accepted},{"reasonCode",reasonName(outcome.reason)},{"data",data},{"snapshot",snapshot(peerId)}});
}
bool PeerUsageService::inputStatus(const QString& peerId,const UsageKey& key,Permission permission,const QString& owner){
  ownerThread();auto it=m_peers.find(peerId);if(it==m_peers.end()||it->second.coordinator->snapshot().key!=key)return false;
  if(!owner.isEmpty()&&owner!="local"&&owner!=peerId)return false;
  it->second.coordinator->inputStatus(permission,owner.toStdString());publish(peerId);return true;
}
bool PeerUsageService::complete(const QString& peerId,const UsageKey& key,Completion kind,Reason reason,bool verified){
  ownerThread();auto it=m_peers.find(peerId);if(it==m_peers.end())return false;auto& c=*it->second.coordinator;bool accepted=false;
  switch(kind){
  case Completion::Preflight:accepted=c.preflight(key,reason);break;
  case Completion::Permission:accepted=c.permitted(key,reason);break;
  case Completion::InputsReleased:accepted=c.inputsReleased(key,verified);break;
  case Completion::InputCleaned:accepted=c.inputCleaned(key,verified);break;
  case Completion::InputStopped:accepted=c.inputStopped(key,verified);break;
  case Completion::DisplayPresented:accepted=c.displayStarted(key);break;
  case Completion::DisplayEnded:accepted=c.displayEnded(key,reason,verified);break;
  case Completion::DisplayFailed:accepted=c.displayFailed(key,reason,verified);break;
  case Completion::DisplayStopped:accepted=c.displayStopped(key,verified);break;
  case Completion::InputStarted:accepted=c.inputStarted(key,verified);break;
  case Completion::RollbackAuthorization:accepted=c.rollbackAuthorized(key,verified);break;
  }
  publish(peerId);return accepted;
}
}
