// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "DisplayService.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QUuid>
#include <cmath>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace deskflow::display {
namespace {
QString hex(const SessionId& id) { return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(id.data()), 16).toHex()); }
SessionId randomId() { auto uuid = QUuid::createUuid().toRfc4122(); SessionId id{}; std::copy(uuid.begin(), uuid.end(), id.begin()); return id; }
SessionId parseId(const QJsonValue& value) {
  const auto string = value.toString();
  if (string.size() != 32 || string.toLower() != string) throw std::invalid_argument("identity");
  for (auto c : string) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) throw std::invalid_argument("identity");
  auto bytes = QByteArray::fromHex(string.toLatin1()); SessionId id{};
  std::copy(bytes.begin(), bytes.end(), id.begin()); return id;
}
uint64_t generation(const QJsonValue& value) {
  if (!value.isString()) throw std::invalid_argument("generation must be a decimal string");
  const auto text = value.toString();
  bool ok = false; auto number = text.toULongLong(&ok);
  if (!ok || QString::number(number) != text) throw std::invalid_argument("generation");
  return number;
}
Key keyFrom(const QJsonObject& object) {
  const auto epoch = object.value("displayEpoch");
  if (!epoch.isDouble() || !std::isfinite(epoch.toDouble()) || epoch.toDouble() < 1 ||
      epoch.toDouble() > std::numeric_limits<uint32_t>::max() || std::floor(epoch.toDouble()) != epoch.toDouble())
    throw std::invalid_argument("epoch");
  return {parseId(object.value("sessionId")), parseId(object.value("requestId")),
    generation(object.value("generation")), static_cast<uint32_t>(epoch.toDouble())};
}
QJsonObject keyJson(const Key& key) {
  return {{"sessionId", hex(key.session)}, {"requestId", hex(key.request)},
    {"generation", QString::number(key.generation)}, {"displayEpoch", static_cast<double>(key.epoch)}};
}
void fields(const QJsonObject& value, const QSet<QString>& required, const QSet<QString>& optional = {}) {
  for (const auto& key : required) if (!value.contains(key)) throw std::invalid_argument("missing field");
  for (auto it = value.begin(); it != value.end(); ++it)
    if (!required.contains(it.key()) && !optional.contains(it.key())) throw std::invalid_argument("unknown field");
}
const QSet<QString> keyFields{"sessionId", "requestId", "generation", "displayEpoch"};
QJsonObject capabilityJson(const Capabilities& capability) {
  return {{"send", availabilityName(capability.send)}, {"receive", availabilityName(capability.receive)},
    {"layout", capability.layout}, {"fullscreen", capability.fullscreen},{"minimize",capability.minimize}, {"sessionAvailable", capability.sessionAvailable},
    {"sessionReason",reasonName(capability.sessionReason)}, {"sendReason",reasonName(capability.sendReason)}, {"receiveReason",reasonName(capability.receiveReason)},
    {"protocolVersion", static_cast<int>(capability.protocol)}, {"receiverInput", false}, {"reconnect", false},
    {"dynamicResolution", false}, {"initialMode", QJsonObject{{"width",1920},{"height",1080},{"targetFps",30},{"codec","h264"},{"color","SDR"}}}};
}
const char* effectName(EffectKind kind) {
#define ENTRY(x) case EffectKind::x:return #x
  switch(kind) { ENTRY(SendRequest); ENTRY(SendAccepted); ENTRY(SendRejected); ENTRY(SendPrepare); ENTRY(CancelRequest);
    ENTRY(PrepareHost); ENTRY(PrepareReceiver); ENTRY(StopPeer); ENTRY(CleanupResources); ENTRY(ShowWindow); }
#undef ENTRY
  return "Invalid";
}
}
DisplayService::DisplayService(Capabilities local, const QString& preferencesFile, QObject* parent)
  : QObject(parent), m_coordinator(local) {
  if (!preferencesFile.isEmpty()) {
    m_preferences = std::make_unique<QSettings>(preferencesFile, QSettings::IniFormat);
    m_coordinator.setReceiveRequestsEnabled(m_preferences->value("display/receiveRequestsEnabled", true).toBool());
  }
  m_timer.setInterval(100);
  connect(&m_timer, &QTimer::timeout, this, [this] { m_coordinator.tick(Coordinator::Clock::now()); publish(); });
  m_timer.start();
}
DisplayService* DisplayService::applicationService() {
  auto app = QCoreApplication::instance();
  if (!app || QThread::currentThread() != app->thread()) throw std::logic_error("Display service needs application owner thread");
  static QPointer<DisplayService> instance;
  if (!instance) {
    // Capability gates remain unknown until an actual product adapter probes them.
    instance = new DisplayService({}, QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/display.ini", app);
  }
  return instance;
}
void DisplayService::ownerThread() const {
  if (QThread::currentThread() != thread()) throw std::logic_error("Display service called off its owner thread");
}
QJsonObject DisplayService::localCapabilities() const { ownerThread(); return capabilityJson(m_coordinator.capabilities()); }
QJsonObject DisplayService::snapshot() const {
  ownerThread(); const auto& source = m_coordinator.snapshot(); auto result = keyJson(source.key);
  result.insert("revision", QString::number(source.revision));
  result.insert("localDisplayRole", roleName(source.role)); result.insert("state", stateName(source.state));
  result.insert("peerId", QString::fromStdString(source.peer.id)); result.insert("peerName", QString::fromStdString(source.peer.name));
  result.insert("reasonCode", reasonName(source.reason)); result.insert("terminalReasonCode", reasonName(source.terminalReason));
  result.insert("firstFramePresented", source.firstFramePresented); result.insert("inputEnabled", false);
  result.insert("inputRoutingSuspended", source.inputRoutingSuspended);
  result.insert("receiveRequestsEnabled", source.receiveRequestsEnabled);
  result.insert("virtualDisplayId", QString::fromStdString(source.virtualDisplayId));
  result.insert("receiverScreenId", QString::fromStdString(source.receiverScreenId));
  result.insert("presentationMode", source.fullscreen ? "fullscreen" : "window");
  const char* cleanup[] = {"none","pending","verified","required"};
  const char* permission[] = {"unknown","required","granted","denied"};
  result.insert("cleanupStatus", cleanup[static_cast<unsigned>(source.cleanup)]);
  result.insert("permissions", QJsonObject{{"capture", permission[static_cast<unsigned>(source.permission)]}, {"receiverInput", "disabled"}});
  result.insert("confirmedLayout",source.state!=State::Idle&&source.key==m_layoutOwner&&!m_layoutState.isEmpty()?QJsonValue(m_layoutState):QJsonValue(QJsonValue::Null));
  result.insert("receiverWindow",source.state!=State::Idle&&!m_windowState.isEmpty()&&source.key==m_windowOwner?QJsonValue(m_windowState):QJsonValue(QJsonValue::Null));
  const auto sampleAt = m_statistics.value("sampledAtMs").toString().toLongLong();
  const auto sampleAge = QDateTime::currentMSecsSinceEpoch() - sampleAt;
  result.insert("statistics", source.state==State::Streaming && source.key==m_statisticsOwner && !m_statistics.isEmpty() && sampleAge>=0 && sampleAge<=5000
      ? QJsonValue(m_statistics) : QJsonValue(QJsonValue::Null));
  result.insert("streamConfig",source.state!=State::Idle&&source.key==m_streamOwner&&!m_streamConfig.isEmpty()?QJsonValue(m_streamConfig):QJsonValue(QJsonValue::Null));
  QJsonArray actions; for (auto action : source.actions) actions.append(actionName(action)); result.insert("availableActions", actions);
  return result;
}
void DisplayService::publish() {
  ownerThread(); auto effects = m_coordinator.takeEffects();
  // Publish before running adapters: a synchronous callback cannot make a newer
  // snapshot appear before the state that scheduled it.
  auto revision = m_coordinator.snapshot().revision;
  if (m_publishedRevision != revision) { m_publishedRevision = revision; Q_EMIT snapshotChanged(snapshot()); }
  for (const auto& effect : effects) {
    QTimer::singleShot(0, this, [this, effect] {
    const auto& current = m_coordinator.snapshot();
    if (current.key.session != effect.key.session || current.key.request != effect.key.request) return;
    const bool startsWork = effect.kind == EffectKind::PrepareHost || effect.kind == EffectKind::PrepareReceiver ||
      effect.kind == EffectKind::SendPrepare || effect.kind == EffectKind::SendRequest ||
      effect.kind == EffectKind::SendAccepted || effect.kind == EffectKind::ShowWindow;
    if (startsWork && (current.key != effect.key || current.state == State::Idle ||
        current.state == State::Stopping || current.state == State::CleanupRequired)) return;
    if(startsWork&&!m_networkPolicies.value(QString::fromStdString(current.peer.id),true)){
      m_coordinator.fail(current.key,Reason::BluetoothOnlyMode,Coordinator::Clock::now());publish();return;
    }
    auto object = keyJson(effect.key); object.insert("operation", effectName(effect.kind));
    object.insert("ownerGeneration", QString::number(effect.ownerGeneration));
    object.insert("reasonCode", reasonName(effect.reason)); Q_EMIT backendCommand(object);
    });
  }
}
void DisplayService::execute(const QJsonObject& command) {
  QTimer::singleShot(0, this, [this, command] { runCommand(command); });
}
void DisplayService::runCommand(const QJsonObject& command) {
  ownerThread(); QString correlation; Outcome outcome{false, Reason::InvalidRequest}; QJsonObject data;bool deferred=false;
  try {
    if (QJsonDocument(command).toJson(QJsonDocument::Compact).size() > 65536) throw std::invalid_argument("size");
    fields(command, {"id","operation","args"});
    correlation = command.value("id").toString();
    if (correlation.isEmpty() || correlation.size() > 64 || !command.value("operation").isString() || !command.value("args").isObject())
      throw std::invalid_argument("envelope");
    const auto op = command.value("operation").toString(); const auto args = command.value("args").toObject();
    auto now = Coordinator::Clock::now();
    if (op == "getDisplaySessionSnapshot") { fields(args, {}); data = snapshot(); outcome = {true, Reason::None}; }
    else if (op == "queryLocalDisplayCapabilities") { fields(args, {}); data = localCapabilities(); outcome = {true, Reason::None}; }
    else if (op == "queryPeerDisplayCapabilities") {
      fields(args, {"peerId"}); const auto it = m_peers.constFind(args.value("peerId").toString());
      if (it != m_peers.cend()) { data = capabilityJson(it->capabilities); outcome = {true, Reason::None}; }
      else outcome = {false, Reason::NetworkUnavailable};
    } else if (op == "requestDisplaySession") {
      fields(args, {"peerId"}, {"requestId"});
      // Starts are owned by the device usage transaction. GUI callers cannot
      // bypass input release and mode mutual exclusion through this subservice.
      outcome = {false, Reason::ModeConflict};
    } else if (op == "respondToDisplayRequest") {
      auto required = keyFields; required.insert("allow"); fields(args, required);
      if (!args.value("allow").isBool()) throw std::invalid_argument("allow");
      if(args.value("allow").toBool()&&!m_networkPolicies.value(QString::fromStdString(m_coordinator.snapshot().peer.id),true))outcome={false,Reason::BluetoothOnlyMode};
      else outcome = m_coordinator.respond(keyFrom(args), args.value("allow").toBool(), now);
    } else if (op == "cancelDisplayRequest" || op == "stopDisplaySession") {
      fields(args, keyFields); outcome = m_coordinator.stop(keyFrom(args), op == "cancelDisplayRequest" ? Reason::Cancelled : Reason::UserStopped, now);
    } else if (op == "retryDisplayCleanup") { fields(args, keyFields); outcome = m_coordinator.retryCleanup(keyFrom(args), now); }
    else if(op=="presentReceiverWindow"||op=="setReceiverPresentation"){
      fields(args,keyFields,op=="setReceiverPresentation"?QSet<QString>{"presentationMode","receiverScreenId","windowState","scaleMode"}:QSet<QString>{});
      const auto key=keyFrom(args);const auto state=m_coordinator.snapshot();
      if(op=="setReceiverPresentation"&&args.value("presentationMode")!="window"&&args.value("presentationMode")!="fullscreen")throw std::invalid_argument("presentation mode");
      if(args.contains("scaleMode")&&args.value("scaleMode")!="fit"&&args.value("scaleMode")!="actual")throw std::invalid_argument("scale mode");
      if(args.contains("windowState")&&args.value("windowState")!="normal"&&args.value("windowState")!="minimized")throw std::invalid_argument("window state");
      if(args.contains("receiverScreenId")&&(!args.value("receiverScreenId").isString()||args.value("receiverScreenId").toString().size()>256))throw std::invalid_argument("screen");
      if(key!=state.key||state.role!=Role::Receiver||state.state!=State::Streaming)outcome={false,Reason::InvalidRequest};
      else if(op=="setReceiverPresentation"&&!m_coordinator.capabilities().fullscreen)outcome={false,Reason::UnsupportedPlatform};
      else if(args.value("windowState")=="minimized"&&!m_coordinator.capabilities().minimize)outcome={false,Reason::UnsupportedPlatform};
      else if(m_windowCommands.size()>=16)outcome={false,Reason::PeerBusy};
      else if(std::any_of(m_windowCommands.cbegin(),m_windowCommands.cend(),[&](const WindowPending& pending){return pending.id==correlation;}))outcome={false,Reason::InvalidRequest};
      else{
        const auto transaction=QUuid::createUuid().toString(QUuid::Id128);m_windowCommands.insert(transaction,{key,correlation});deferred=true;
        auto request=args;request.insert("operation",op=="presentReceiverWindow"?"ShowWindow":"SetReceiverPresentation");request.insert("commandId",transaction);
        QTimer::singleShot(0,this,[this,request,key]{if(currentKey()==key)Q_EMIT backendCommand(request);});
        QTimer::singleShot(3000,this,[this,transaction,key]{if(m_windowCommands.contains(transaction))receiverWindowResult(key,transaction,false,{});});
      }
    }
    else if (op == "setReceiveRequestsEnabled") {
      fields(args, {"enabled"}); if (!args.value("enabled").isBool()) throw std::invalid_argument("enabled");
      const auto enabled = args.value("enabled").toBool(); m_coordinator.setReceiveRequestsEnabled(enabled);
      if (m_preferences) m_preferences->setValue("display/receiveRequestsEnabled", enabled);
      outcome = {true, Reason::None};
    } else if (op == "setDisplayLayout" || op == "queryDisplayLayout") {
      auto required=keyFields;if(op=="setDisplayLayout")required.insert("layout");fields(args,required);
      const auto key=keyFrom(args);const auto state=m_coordinator.snapshot();
      if(op=="setDisplayLayout"){
        if(!args.value("layout").isObject())throw std::invalid_argument("layout");
        const auto layout=args.value("layout").toObject();fields(layout,{"x","y"},{"scale"});
        for(const auto* axis:{"x","y"}){const auto v=layout.value(axis);if(!v.isDouble()||!std::isfinite(v.toDouble())||std::floor(v.toDouble())!=v.toDouble()||std::abs(v.toDouble())>32768)throw std::invalid_argument("position");}
        if(layout.contains("scale")&&(!layout.value("scale").isDouble()||!std::isfinite(layout.value("scale").toDouble())||layout.value("scale").toDouble()<=0))throw std::invalid_argument("scale");
      }
      if(key!=state.key||state.role!=Role::Host||state.state!=State::Streaming)outcome={false,Reason::InvalidRequest};
      else if(!m_coordinator.capabilities().layout)outcome={false,Reason::UnsupportedPlatform};
      else if(!m_layoutCommands.isEmpty())outcome={false,Reason::PeerBusy};
      else{
        const auto transaction=QUuid::createUuid().toString(QUuid::Id128);m_layoutCommands.insert(transaction,{key,correlation});deferred=true;
        auto request=args;request.insert("operation",op=="setDisplayLayout"?"SetDisplayLayout":"QueryDisplayLayout");request.insert("commandId",transaction);
        QTimer::singleShot(0,this,[this,request,key]{if(currentKey()==key)Q_EMIT backendCommand(request);});
        QTimer::singleShot(5000,this,[this,key,transaction]{layoutResult(key,transaction,false,{});});
      }
    }
  } catch (const std::invalid_argument&) { outcome = {false, Reason::InvalidRequest}; }
  publish();
  if(!deferred)Q_EMIT commandFinished(QJsonObject{{"id",correlation},{"accepted",outcome.accepted},{"reasonCode",reasonName(outcome.reason)},
    {"data", data},{"snapshot",snapshot()}});
}
Outcome DisplayService::requestManagedSession(const QString& peerId,SessionId request){
  ownerThread();if(!m_networkPolicies.value(peerId,true))return {false,Reason::BluetoothOnlyMode};auto it=m_peers.constFind(peerId);
  auto outcome=it==m_peers.cend()?Outcome{false,Reason::NetworkUnavailable}:
    m_coordinator.request(*it,randomId(),request,m_routing,Coordinator::Clock::now(),true);
  publish();return outcome;
}
Outcome DisplayService::retryManagedCleanup(const Key& key){ownerThread();auto result=m_coordinator.retryCleanup(key,Coordinator::Clock::now());publish();return result;}
Outcome DisplayService::resumeManagedSession(const Key& key){ownerThread();auto result=m_coordinator.resumePreparation(key,m_routing,Coordinator::Clock::now());publish();return result;}
Outcome DisplayService::stopManagedSession(const Key& key,Reason reason){ownerThread();auto result=(reason==Reason::Cancelled||reason==Reason::UserStopped||reason==Reason::ApplicationExit)?
    m_coordinator.stop(key,reason,Coordinator::Clock::now()):Outcome{m_coordinator.fail(key,reason,Coordinator::Clock::now()),reason};publish();return result;}
void DisplayService::updateCapabilities(Capabilities local) { ownerThread(); m_coordinator.setCapabilities(local, Coordinator::Clock::now()); publish(); }
void DisplayService::authenticatedPeer(Peer peer) { ownerThread(); const auto identity = QString::fromStdString(peer.id); m_peers.insert(identity, std::move(peer)); }
void DisplayService::setPeerNetworkPolicy(const QString& peerId,bool allowed){
  ownerThread();m_networkPolicies.insert(peerId,allowed);
  const auto state=m_coordinator.snapshot();
  if(!allowed&&QString::fromStdString(state.peer.id)==peerId&&state.state!=State::Idle&&state.state!=State::Stopping&&state.state!=State::CleanupRequired)
    stopManagedSession(state.key,Reason::BluetoothOnlyMode);
}
void DisplayService::forgetPeer(const QString& identity) { ownerThread(); m_peers.remove(identity); }
Outcome DisplayService::incomingAuthenticated(Peer peer, SessionId session, SessionId request) {
  ownerThread();if(!m_networkPolicies.value(QString::fromStdString(peer.id),true))return {false,Reason::BluetoothOnlyMode}; auto result = m_coordinator.incoming(std::move(peer), session, request, Coordinator::Clock::now()); publish(); return result;
}
void DisplayService::peerResponse(const Key& key, bool allowed, Reason reason) { ownerThread(); m_coordinator.peerResponse(key,allowed,reason,Coordinator::Clock::now()); publish(); }
void DisplayService::peerReady(const Key& key) { ownerThread(); m_coordinator.peerReady(key,Coordinator::Clock::now()); publish(); }
void DisplayService::permissionResult(const Key& key, Permission permission) { ownerThread(); m_coordinator.permission(key,permission,Coordinator::Clock::now()); publish(); }
void DisplayService::backendPrepared(const Key& key, const QString& displayId) { ownerThread(); m_coordinator.prepared(key,displayId.toStdString(),Coordinator::Clock::now()); publish(); }
void DisplayService::firstFramePresented(const Key& key) { ownerThread(); m_coordinator.presented(key,Coordinator::Clock::now()); publish(); }
void DisplayService::backendFailed(const Key& key, Reason reason) { ownerThread(); m_coordinator.fail(key,reason,Coordinator::Clock::now()); publish(); }
void DisplayService::cleanupFinished(const Key& key, bool verified) { ownerThread(); m_coordinator.cleaned(key,verified); publish(); }
bool DisplayService::restorePendingResource(Peer peer, const Key& owner, Role role, const QString& displayId) {
  ownerThread(); auto result=m_coordinator.restorePending(std::move(peer),owner,role,displayId.toStdString()); publish(); return result;
}
void DisplayService::receiverWindowState(const Key& key,const QJsonObject& state){
  ownerThread();const auto screen=state.value("receiverScreenId").toString();
  if(!state.value("fullscreen").isBool()||!m_coordinator.windowState(key,screen.toStdString(),state.value("fullscreen").toBool()))return;
  m_windowOwner=key;m_windowState=state;publish();
}
void DisplayService::receiverWindowResult(const Key& key,const QString& commandId,bool accepted,const QJsonObject& state){
  ownerThread();auto it=m_windowCommands.find(commandId);if(it==m_windowCommands.end()||it.value().key!=key)return;const auto correlation=it.value().id;m_windowCommands.erase(it);
  accepted=accepted&&currentKey()==key&&m_coordinator.snapshot().state==State::Streaming&&
    !state.value("receiverScreenId").toString().isEmpty()&&state.value("fullscreen").isBool()&&state.value("visible").isBool()&&state.value("minimized").isBool();
  if(accepted)receiverWindowState(key,state);
  Q_EMIT commandFinished({{"id",correlation},{"accepted",accepted},{"reasonCode",accepted?"None":"PresentationFailed"},{"data",state},{"snapshot",snapshot()}});
}
void DisplayService::streamStatistics(const Key& key,const QJsonObject& statistics)
{
  ownerThread();
  if(currentKey()!=key || !statistics.value("metricsFresh").isBool() || !statistics.value("metricsFresh").toBool())return;
  for(const auto field : {"txBytesPerSecond", "rxBytesPerSecond"}) {
    const auto value=statistics.value(field);
    if(!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble()<0)return;
  }
  const auto rtt=statistics.value("rttMs");
  if(!rtt.isNull() && (!rtt.isDouble() || !std::isfinite(rtt.toDouble()) || rtt.toDouble()<0))return;
  const auto source=statistics.value("rttSource").toString();
  if(source!="heartbeat" && source!="tcp")return;
  bool valid=false;const auto stamp=statistics.value("sampledAtMs").toString().toLongLong(&valid);
  const auto age=QDateTime::currentMSecsSinceEpoch()-stamp;
  if(!valid || age<0 || age>5000 || !m_coordinator.adapterStateChanged(key))return;
  m_statisticsOwner=key;
  m_statistics={{"metricsFresh",true},{"sampledAtMs",QString::number(stamp)},
    {"txBytesPerSecond",statistics.value("txBytesPerSecond")},{"rxBytesPerSecond",statistics.value("rxBytesPerSecond")},
    {"rttMs",rtt},{"rttSource",source}};
  publish();
}
void DisplayService::streamConfiguration(const Key& key,const QJsonObject& config){
  ownerThread();if(currentKey()!=key)return;
  if(config.value("codec")!="h264"||config.value("width")!=1920||config.value("height")!=1080||config.value("fps")!=30||config.value("input")!=false)return;
  if(!m_coordinator.adapterStateChanged(key))return;
  m_streamOwner=key;m_streamConfig=config;publish();
}
void DisplayService::layoutResult(const Key& key,const QString& commandId,bool accepted,const QJsonObject& data){
  ownerThread();QString correlation;
  if(!commandId.isEmpty()){auto it=m_layoutCommands.find(commandId);if(it==m_layoutCommands.end()||it->key!=key)return;correlation=it->id;m_layoutCommands.erase(it);}
  const auto state=m_coordinator.snapshot();
  accepted=accepted&&state.key==key&&state.role==Role::Host&&state.state==State::Streaming&&data.value("layout").isObject()&&data.value("displays").isArray();
  if(accepted){m_coordinator.adapterStateChanged(key);m_layoutOwner=key;m_layoutState=data;publish();}
  if(!commandId.isEmpty())Q_EMIT commandFinished({{"id",correlation},{"accepted",accepted},{"reasonCode",accepted?"None":"LayoutRejected"},{"data",data},{"snapshot",snapshot()}});
}
void DisplayService::setInputRouting(Routing routing) { ownerThread(); m_routing = routing; }
} // namespace deskflow::display
