// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "WorkerProtocol.h"
#include "display/control/InfrastructureControl.h"
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QtEndian>
#include <limits>

namespace deskflow::display::infra {
namespace {
QByteArray json(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
QByteArray mac(const QByteArray& key, const QByteArray& value) {
  return QMessageAuthenticationCode::hash(value, key, QCryptographicHash::Sha256).toHex();
}
bool constantEqual(const QByteArray& a,const QByteArray& b) {
  if(a.size()!=b.size())return false; unsigned difference=0;
  for(qsizetype i=0;i<a.size();++i)difference|=static_cast<unsigned char>(a[i])^static_cast<unsigned char>(b[i]);
  return difference==0;
}
QByteArray nonce() { QByteArray b(32,Qt::Uninitialized);for(qsizetype i=0;i<32;i+=4){auto n=QRandomGenerator::system()->generate();qToBigEndian(n,b.data()+i);}return b.toHex(); }
bool id(const QString& s,size_t n){return validId(s.toStdString(),n/2);}
bool name(const QString& s){return validName(s.toStdString());}
QString side(WorkerSide s){return s==WorkerSide::Agent?QStringLiteral("agent"):QStringLiteral("worker");}
QByteArray framed(const QByteArray& b){QByteArray out(4,Qt::Uninitialized);qToBigEndian(quint32(b.size()),out.data());return out+b;}
QString counter(uint64_t n){return QString::number(n);}
std::optional<uint64_t> number(const QJsonValue& v){if(!v.isString())return {};return parseCounter(v.toString().toStdString());}
void wipe(QByteArray& b){if(!b.isEmpty()){b.detach();volatile char* p=b.data();for(qsizetype i=0;i<b.size();++i)p[i]=0;}b.clear();}
QJsonObject body(const WorkerMessage& message) {
  return std::visit([](const auto& m)->QJsonObject{
    using T=std::decay_t<decltype(m)>;
    if constexpr(std::is_same_v<T,WorkerStart>)return {{"type","Start"}};
    if constexpr(std::is_same_v<T,WorkerStop>)return {{"type","Stop"},{"reason",int(m.reason)}};
    if constexpr(std::is_same_v<T,WorkerHeartbeat>)return {{"type","Heartbeat"}};
    if constexpr(std::is_same_v<T,WorkerRefresh>)return {{"type","Refresh"}};
    if constexpr(std::is_same_v<T,WorkerReady>)return {{"type","Ready"}};
    if constexpr(std::is_same_v<T,WorkerPresented>)return {{"type","Presented"},{"frame",counter(m.frame)},
      {"evidence",int(m.evidence)},{"exposed",m.exposed}};
    if constexpr(std::is_same_v<T,WorkerQuiesced>)return {{"type","Quiesced"}};
    if constexpr(std::is_same_v<T,WorkerFailure>)return {{"type","Failure"},{"code",m.code}};
  },message);
}
std::optional<WorkerMessage> message(const QJsonObject& o) {
  if(!o.value("type").isString())return {};
  const auto type=o.value("type").toString();
  WorkerMessage value;
  if(type=="Start")value=WorkerStart{};
  else if(type=="Stop"){
    const auto r=o.value("reason");if(!r.isDouble()||r.toInt(-1)<0||r.toInt(-1)>int(StopReason::Shutdown)||r.toDouble()!=r.toInt())return {};
    value=WorkerStop{StopReason(r.toInt())};
  }else if(type=="Heartbeat")value=WorkerHeartbeat{};
  else if(type=="Refresh")value=WorkerRefresh{};
  else if(type=="Ready")value=WorkerReady{};
  else if(type=="Quiesced")value=WorkerQuiesced{};
  else if(type=="Presented"){
    auto f=number(o.value("frame"));const auto e=o.value("evidence");
    if(!f||!e.isDouble()||e.toInt(-1)<0||e.toInt(-1)>int(PresentationEvidence::CompositorPresented)||e.toDouble()!=e.toInt()||!o.value("exposed").isBool())return {};
    value=WorkerPresented{*f,PresentationEvidence(e.toInt()),o.value("exposed").toBool()};
  }else if(type=="Failure"){
    if(!o.value("code").isString()||!name(o.value("code").toString()))return {};
    value=WorkerFailure{o.value("code").toString()};
  }else return {};
  if(body(value)!=o)return {}; // Exact typed schema, no shell paths or arbitrary payloads.
  return value;
}
}
WorkerProtocol::WorkerProtocol(WorkerSide s,WorkerBinding binding,QByteArray secret,TimePoint now):
  m_side(s),m_binding(std::move(binding)),m_secret(std::move(secret)),m_nonce(nonce()),
  m_handshakeDeadline(now+std::chrono::seconds(3)),m_leaseDeadline(now) {
  const auto& t=m_binding.token;
  if(m_secret.size()!=32||!validId(t.session,16)||!validId(t.owner,16)||!validId(t.worker,16)||
     !t.attempt||!t.generation||!t.epoch||!name(m_binding.localUserSession)||
     !name(m_binding.buildCompatibility)||!name(m_binding.backend)||!name(m_binding.profile)||!id(m_binding.planHash,64))fail("InvalidBootstrap");
}
WorkerProtocol::~WorkerProtocol(){wipe(m_secret);wipe(m_key);}
QByteArray WorkerProtocol::context() const {
  const auto& t=m_binding.token;
  return json({{"protocol","display-worker/1"},{"session",QString::fromStdString(t.session)},
    {"attempt",counter(t.attempt)},{"generation",counter(t.generation)},{"epoch",counter(t.epoch)},
    {"owner",QString::fromStdString(t.owner)},{"worker",QString::fromStdString(t.worker)},
    {"userSession",m_binding.localUserSession},{"build",m_binding.buildCompatibility},
    {"backend",m_binding.backend},{"profile",m_binding.profile},{"planHash",m_binding.planHash}});
}
QByteArray WorkerProtocol::hello() {
  if(failed()||m_helloSent)return {};
  m_helloSent=true;
  QJsonObject o{{"type","Hello"},{"side",side(m_side)},{"nonce",QString::fromLatin1(m_nonce)},
    {"context",QString::fromLatin1(QCryptographicHash::hash(context(),QCryptographicHash::Sha256).toHex())}};
  o.insert("proof",QString::fromLatin1(mac(m_secret,QByteArray("display-worker-hello/1\0",23)+json(o))));
  return framed(json(o));
}
void WorkerProtocol::fail(const QString& code){if(m_error.isEmpty())m_error=code;wipe(m_secret);wipe(m_key);m_buffer.clear();m_pending.clear();}
bool WorkerProtocol::feed(const QByteArray& bytes,bool authenticated,TimePoint now) {
  tick(now);if(failed())return false;
  if(!authenticated){fail("LocalTransportIdentity");return false;}
  // Bound the caller's chunk too, before making any copy. Socket readers must
  // cap read() at this budget and stop reading once the queue is full.
  if(bytes.size()>MaxMessageBytes+4||m_buffer.size()+bytes.size()>2*(MaxMessageBytes+4)){
    fail("IpcReadBudget");return false;
  }
  m_buffer+=bytes;
  while(m_buffer.size()>=4){
    const auto n=qFromBigEndian<quint32>(m_buffer.constData());
    if(!n||n>MaxMessageBytes){fail("IpcFrameSize");return false;}
    if(m_buffer.size()<qsizetype(n)+4)break;
    const auto b=m_buffer.mid(4,n);m_buffer.remove(0,n+4);
    if(!consume(b,now))return false;
  }
  return true;
}
bool WorkerProtocol::consume(const QByteArray& bytes,TimePoint now) {
  ControlError e;auto parsed=parseBoundedObject(bytes,MaxMessageBytes,e);
  if(!parsed){fail("IpcJson");return false;}
  auto o=*parsed;
  if(!m_established){
    if(!m_helloSent||o.size()!=5||o.value("type")!="Hello"||
       o.value("side")!=side(m_side==WorkerSide::Agent?WorkerSide::Worker:WorkerSide::Agent)||
       !id(o.value("nonce").toString(),64)||!id(o.value("proof").toString(),64)||
       o.value("context")!=QString::fromLatin1(QCryptographicHash::hash(context(),QCryptographicHash::Sha256).toHex())){
      fail("IpcHelloBinding");return false;
    }
    const auto proof=o.take("proof").toString().toLatin1();
    if(!constantEqual(proof,mac(m_secret,QByteArray("display-worker-hello/1\0",23)+json(o)))){fail("IpcHelloProof");return false;}
    m_remoteNonce=o.value("nonce").toString().toLatin1();
    const auto a=m_side==WorkerSide::Agent?m_nonce:m_remoteNonce;
    const auto w=m_side==WorkerSide::Worker?m_nonce:m_remoteNonce;
    m_key=QMessageAuthenticationCode::hash(QByteArray("display-worker-key/1\0",21)+context()+a+w,m_secret,QCryptographicHash::Sha256);
    wipe(m_secret);m_established=true;m_leaseDeadline=now+std::chrono::milliseconds(1500);return true;
  }
  if(o.size()!=4||!o.value("body").isObject()||o.value("side")!=side(m_side==WorkerSide::Agent?WorkerSide::Worker:WorkerSide::Agent)||!id(o.value("mac").toString(),64)){
    fail("IpcEnvelope");return false;
  }
  const auto sequence=number(o.value("sequence"));
  if(!sequence||m_rx==std::numeric_limits<uint64_t>::max()||*sequence!=m_rx+1){fail("IpcSequence");return false;}
  const auto signature=o.take("mac").toString().toLatin1();
  if(!constantEqual(signature,mac(m_key,json(o)))){fail("IpcMessageProof");return false;}
  const auto typed=message(o.value("body").toObject());
  if(!typed||!acceptMessage(*typed,false)){fail("IpcMessageState");return false;}
  if(m_pending.size()>=size_t(MaxPendingMessages)){fail("IpcQueueBudget");return false;}
  m_rx=*sequence;
  // Frames/Ready already in the pipe when Stop was sent are not new work.
  // Authenticate and consume their sequence but never dispatch them after revoke.
  const bool lateObservation=m_stopping&&(std::holds_alternative<WorkerPresented>(*typed)||std::holds_alternative<WorkerReady>(*typed));
  if(!lateObservation)m_pending.push_back(*typed);
  m_leaseDeadline=now+std::chrono::milliseconds(1500);return true;
}
bool WorkerProtocol::acceptMessage(const WorkerMessage& m,bool outbound) {
  const bool agent=(m_side==WorkerSide::Agent)==outbound;
  if(m_quiesced)return false;
  if(std::holds_alternative<WorkerHeartbeat>(m))return true;
  if(const auto* stop=std::get_if<WorkerStop>(&m)){
    if(!agent||int(stop->reason)<0||int(stop->reason)>int(StopReason::Shutdown))return false;
    m_stopping=true;return true;
  }
  if(const auto* failure=std::get_if<WorkerFailure>(&m)){
    if(agent||!name(failure->code))return false;m_stopping=true;return true;
  }
  if(std::holds_alternative<WorkerQuiesced>(m)){
    if(agent||!m_stopping)return false;m_quiesced=true;return true;
  }
  const bool lateObservation=!outbound&&m_side==WorkerSide::Agent&&
    (std::holds_alternative<WorkerPresented>(m)||std::holds_alternative<WorkerReady>(m));
  if(m_stopping&&!lateObservation)return false;
  if(std::holds_alternative<WorkerReady>(m)){if(agent||m_ready)return false;m_ready=true;return true;}
  if(std::holds_alternative<WorkerStart>(m)){if(!agent||!m_ready||m_started)return false;m_started=true;return true;}
  if(std::holds_alternative<WorkerRefresh>(m))return agent&&m_started;
  if(const auto* f=std::get_if<WorkerPresented>(&m)){
    if(agent||!m_started||!f->frame||f->frame<=m_lastFrame||int(f->evidence)<0||int(f->evidence)>int(PresentationEvidence::CompositorPresented))return false;
    m_lastFrame=f->frame;return true;
  }
  return false;
}
QByteArray WorkerProtocol::encode(const WorkerMessage& m,TimePoint now) {
  tick(now);if(!established()||m_tx==std::numeric_limits<uint64_t>::max())return {};
  if(!acceptMessage(m,true)){fail("IpcOutboundState");return {};}
  QJsonObject o{{"side",side(m_side)},{"sequence",counter(++m_tx)},{"body",body(m)}};
  o.insert("mac",QString::fromLatin1(mac(m_key,json(o))));
  const auto bytes=json(o);if(bytes.size()>MaxMessageBytes){fail("IpcWriteBudget");return {};}return framed(bytes);
}
std::vector<WorkerMessage> WorkerProtocol::takeMessages(){auto result=std::move(m_pending);m_pending.clear();return result;}
void WorkerProtocol::tick(TimePoint now){if(failed())return;if(!m_established&&now>=m_handshakeDeadline)fail("IpcHandshakeDeadline");else if(m_established&&!m_quiesced&&now>=m_leaseDeadline)fail("IpcLeaseExpired");}
void WorkerProtocol::disconnected(){fail("IpcDisconnected");}
} // namespace deskflow::display::infra
