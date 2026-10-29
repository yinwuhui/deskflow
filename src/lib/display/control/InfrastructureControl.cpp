// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "InfrastructureControl.h"
#include "ControlBodyRegistry.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSet>
#include <QStringDecoder>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace deskflow::display::infra {
namespace {
constexpr qsizetype messageLimit = 65536;
QString randomHex()
{
  std::array<quint32, 8> values;
  QRandomGenerator::system()->fillRange(values.data(), values.size());
  return QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(values.data()), sizeof(values)).toHex());
}
bool fail(ControlError &e, const char *code, QString detail = {})
{
  e = {QString::fromLatin1(code), std::move(detail)};
  return false;
}
bool keys(
    const QJsonObject &o, std::initializer_list<const char *> required,
    std::initializer_list<const char *> optional = {}
)
{
  QSet<QString> allowed;
  for (const auto *k : required) {
    if (!o.contains(QLatin1String(k)))
      return false;
    allowed.insert(QLatin1String(k));
  }
  for (const auto *k : optional)
    allowed.insert(QLatin1String(k));
  for (auto i = o.begin(); i != o.end(); ++i)
    if (!allowed.contains(i.key()))
      return false;
  return true;
}
bool unicode(const QString &s)
{
  for (qsizetype i = 0; i < s.size(); ++i) {
    if (s[i].isHighSurrogate()) {
      if (++i >= s.size() || !s[i].isLowSurrogate())
        return false;
    } else if (s[i].isLowSurrogate())
      return false;
  }
  return true;
}
// QJsonObject deliberately keeps only the last duplicate key. Validate the
// original tokens first so signatures/capabilities cannot be interpreted twice.
class Boundaries
{
public:
  explicit Boundaries(const QByteArray &data) : m_data(data)
  {
  }
  bool valid()
  {
    if (!value(0))
      return false;
    space();
    return m_at == m_data.size();
  }

private:
  void space()
  {
    while (m_at < m_data.size() && QByteArray(" \r\n\t").contains(m_data[m_at]))
      ++m_at;
  }
  bool eat(char c)
  {
    space();
    if (m_at >= m_data.size() || m_data[m_at] != c)
      return false;
    ++m_at;
    return true;
  }
  std::optional<QString> string()
  {
    space();
    if (m_at >= m_data.size() || m_data[m_at] != '"')
      return {};
    const auto start = m_at++;
    while (m_at < m_data.size()) {
      const auto c = m_data[m_at++];
      if (c == '\\') {
        if (m_at >= m_data.size())
          return {};
        ++m_at;
      } else if (c == '"') {
        if (m_at - start > 6 * 32768 + 2)
          return {};
        QJsonParseError error;
        const auto d = QJsonDocument::fromJson("[" + m_data.mid(start, m_at - start) + "]", &error);
        if (error.error != QJsonParseError::NoError || !d.isArray() || !d.array().first().isString())
          return {};
        const auto s = d.array().first().toString();
        if (!unicode(s) || s.toUtf8().size() > 32768)
          return {};
        return s;
      }
      if (m_at - start > 6 * 32768 + 2)
        return {};
    }
    return {};
  }
  bool value(unsigned depth)
  {
    space();
    if (depth > 16 || m_at >= m_data.size())
      return false;
    const auto c = m_data[m_at];
    if (c == '"')
      return string().has_value();
    if (c == '{') {
      ++m_at;
      space();
      if (eat('}'))
        return true;
      QSet<QString> seen;
      while (true) {
        const auto key = string();
        if (!key || seen.contains(*key) || seen.size() >= 64 || !eat(':'))
          return false;
        seen.insert(*key);
        if (!value(depth + 1))
          return false;
        if (eat('}'))
          return true;
        if (!eat(','))
          return false;
      }
    }
    if (c == '[') {
      ++m_at;
      if (eat(']'))
        return true;
      unsigned count = 0;
      while (true) {
        if (++count > 256 || !value(depth + 1))
          return false;
        if (eat(']'))
          return true;
        if (!eat(','))
          return false;
      }
    }
    const auto start = m_at;
    while (m_at < m_data.size() && !QByteArray(",]} \r\n\t").contains(m_data[m_at]))
      ++m_at;
    return m_at > start; // The full Qt parser validates primitive grammar below.
  }
  const QByteArray &m_data;
  qsizetype m_at = 0;
};
QString graphics(GraphicsMode g)
{
  switch (g) {
  case GraphicsMode::H264ConstrainedBaseline:
    return "h264-constrained-baseline";
  case GraphicsMode::RdpAvc420:
    return "rdp-avc420";
  case GraphicsMode::RdpAvc444:
    return "rdp-avc444";
  }
  return {};
}
QString security(SecurityProfile s)
{
  switch (s) {
  case SecurityProfile::RdpAuthenticatedTls13:
    return "rdp.authenticated-tls13.v1";
  case SecurityProfile::WebRtcAuthenticatedDtlsSrtp:
    return "webrtc.authenticated-dtls-srtp.v1";
  }
  return {};
}
QString wire(const Candidate &c)
{
  if (c.sender.protocol == WireProtocol::WebRtc)
    return "webrtc.video.h264.v1";
  return c.format.graphics == GraphicsMode::RdpAvc444 ? "rdp.gfx.avc444.v1" : "rdp.gfx.avc420.v1";
}
QString color(ColorProfile c)
{
  return c == ColorProfile::SdrBt709Limited8 ? "sdr-bt709-limited-8bit" : "sdr-bt709-full-8bit";
}
bool positive(const QJsonValue &value, uint64_t maximum, uint64_t &out)
{
  if (!value.isDouble())
    return false;
  const auto d = value.toDouble();
  if (!std::isfinite(d) || d < 1 || d > static_cast<double>(maximum) || std::floor(d) != d)
    return false;
  out = static_cast<uint64_t>(d);
  return true;
}
const QSet<QString> messages{"CapabilitiesSnapshot", "PlanOffer",     "PlanAnswer",    "PlanPropose",
                             "PlanAccept",           "PlanCommit",    "PlanCommitted", "MediaSignal",
                             "FrameCorrelation",     "EndpointReady", "MediaReady",    "FirstFrameEvidence",
                             "HealthSummary",        "Stop",          "Stopped",       "Error"};
bool needsPlan(const QString &type)
{
  return type != "CapabilitiesSnapshot" && type != "PlanOffer" && type != "PlanAnswer" && type != "Stop" &&
         type != "Stopped" && type != "Error";
}
bool envelopeFields(const Envelope &e)
{
  if (!messages.contains(e.type) || !validId(e.messageId.toStdString(), 16) || e.body.size() > 32)
    return false;
  if (e.type != "CapabilitiesSnapshot" && e.type != "Error" && (!validId(e.sessionId.toStdString(), 16) || !e.attempt))
    return false;
  if (needsPlan(e.type) && !validId(e.planHash.toStdString(), 32))
    return false;
  if (!e.sessionId.isEmpty() && !validId(e.sessionId.toStdString(), 16))
    return false;
  if (!e.planHash.isEmpty() && !validId(e.planHash.toStdString(), 32))
    return false;
  return true;
}
} // namespace
std::optional<QJsonObject> parseControlObject(const QByteArray &bytes, ControlError &error)
{
  return parseBoundedObject(bytes, messageLimit, error);
}
std::optional<QJsonObject> parseBoundedObject(const QByteArray &bytes, qsizetype budget, ControlError &error)
{
  if (budget <= 0 || budget > 4 * 1024 * 1024 || bytes.isEmpty() || bytes.size() > budget) {
    fail(error, "MessageBudgetExceeded");
    return {};
  }
  QStringDecoder utf8(QStringDecoder::Utf8);
  const QString decoded = utf8(bytes);
  if (utf8.hasError() || !unicode(decoded) || !Boundaries(bytes).valid()) {
    fail(error, "InvalidControlEncoding");
    return {};
  }
  QJsonParseError parse;
  const auto doc = QJsonDocument::fromJson(bytes, &parse);
  if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
    fail(error, "InvalidControlEncoding");
    return {};
  }
  error = {};
  return doc.object();
}
QByteArray canonicalPlan(const SessionPlan &p)
{
  const auto &f = p.candidate.format;
  QJsonObject video{
      {"width", static_cast<int>(f.width)},
      {"height", static_cast<int>(f.height)},
      {"fpsNumerator", static_cast<int>(f.fpsNumerator)},
      {"fpsDenominator", static_cast<int>(f.fpsDenominator)},
      {"graphicsMode", graphics(f.graphics)},
      {"colorProfileId", color(f.color)},
      {"cursorMode", f.cursor == CursorMode::Embedded ? "embedded" : "metadata"}
  };
  QJsonObject policy{
      {"startupDeadlineMs", static_cast<int>(p.policy.startup.count())},
      {"normalStopTargetMs", static_cast<int>(p.policy.normalStop.count())},
      {"abnormalCleanupTargetMs", static_cast<int>(p.policy.abnormalCleanup.count())},
      {"maxAttempts", static_cast<int>(p.policy.maximumAttempts)},
      {"allowRemoteInput", false},
      {"allowAudio", false},
      {"allowRedirection", false}
  };
  QJsonObject o{
      {"schemaVersion", 1},
      {"controlBindingId", QString::fromStdString(p.controlBinding)},
      {"sessionId", QString::fromStdString(p.session)},
      {"attemptId", QString::number(p.attempt)},
      {"senderPeerId", QString::fromStdString(p.senderIdentity)},
      {"receiverPeerId", QString::fromStdString(p.receiverIdentity)},
      {"senderCapabilityRevision", QString::number(p.senderRevision)},
      {"receiverCapabilityRevision", QString::number(p.receiverRevision)},
      {"senderPipelineId", QString::fromStdString(p.candidate.sender.id)},
      {"receiverPipelineId", QString::fromStdString(p.candidate.receiver.id)},
      {"mediaBackend", QLatin1String(protocolName(p.candidate.sender.protocol))},
      {"wireProfileId", wire(p.candidate)},
      {"securityProfileId", security(p.candidate.security)},
      {"authorizationScopeDigest", QString::fromStdString(p.authorizationScopeDigest)},
      {"video", video},
      {"policy", policy}
  };
  // This schema's keys/identifiers are ASCII and its numeric values are bounded
  // integers; Qt's sorted compact object encoding matches the RFC8785 subset.
  return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
QString planHash(const SessionPlan &p)
{
  return QString::fromLatin1(QCryptographicHash::hash(canonicalPlan(p), QCryptographicHash::Sha256).toHex());
}
std::optional<SessionPlan> decodePlan(
    const QByteArray &bytes, const QString &expectedHash, const CapabilitySnapshot &sender,
    const CapabilitySnapshot &receiver, const Requirements &requirements, const SelectionPolicy &selection,
    ControlError &error
)
{
  auto parsed = parseControlObject(bytes, error);
  if (!parsed)
    return {};
  const auto &o = *parsed;
  if (!keys(
          o, {"schemaVersion", "controlBindingId", "sessionId", "attemptId", "senderPeerId", "receiverPeerId",
              "senderCapabilityRevision", "receiverCapabilityRevision", "senderPipelineId", "receiverPipelineId",
              "mediaBackend", "wireProfileId", "securityProfileId", "authorizationScopeDigest", "video", "policy"}
      ) ||
      o.value("schemaVersion") != QJsonValue(1) || !o.value("video").isObject() || !o.value("policy").isObject()) {
    fail(error, "InvalidPlan");
    return {};
  }
  for (auto it = o.begin(); it != o.end(); ++it)
    if (it.key() != "schemaVersion" && it.key() != "video" && it.key() != "policy" && !it.value().isString()) {
      fail(error, "InvalidPlan");
      return {};
    }
  const auto attempt = parseCounter(o.value("attemptId").toString().toStdString());
  const auto sr = parseCounter(o.value("senderCapabilityRevision").toString().toStdString());
  const auto rr = parseCounter(o.value("receiverCapabilityRevision").toString().toStdString());
  if (!attempt || !sr || !rr) {
    fail(error, "InvalidCounter");
    return {};
  }
  const auto video = o.value("video").toObject(), policy = o.value("policy").toObject();
  if (!keys(
          video, {"width", "height", "fpsNumerator", "fpsDenominator", "graphicsMode", "colorProfileId", "cursorMode"}
      ) ||
      !keys(
          policy, {"startupDeadlineMs", "normalStopTargetMs", "abnormalCleanupTargetMs", "maxAttempts",
                   "allowRemoteInput", "allowAudio", "allowRedirection"}
      )) {
    fail(error, "InvalidPlan");
    return {};
  }
  for (const auto *k : {"allowRemoteInput", "allowAudio", "allowRedirection"})
    if (policy.value(QLatin1String(k)) != QJsonValue(false)) {
      fail(error, "UnsupportedFeature");
      return {};
    }
  uint64_t startup = 0, stop = 0, cleanup = 0, attempts = 0;
  if (!positive(policy.value("startupDeadlineMs"), 10000, startup) ||
      !positive(policy.value("normalStopTargetMs"), 5000, stop) ||
      !positive(policy.value("abnormalCleanupTargetMs"), 15000, cleanup) ||
      !positive(policy.value("maxAttempts"), 2, attempts)) {
    fail(error, "InvalidPlan");
    return {};
  }
  const auto choices = plan(sender, receiver, requirements, selection);
  if (choices.reason != Rejection::None) {
    fail(error, rejectionName(choices.reason));
    return {};
  }
  for (const auto &c : choices.candidates) {
    if (o.value("senderPipelineId") != QString::fromStdString(c.sender.id) ||
        o.value("receiverPipelineId") != QString::fromStdString(c.receiver.id) ||
        o.value("mediaBackend") != QLatin1String(protocolName(c.sender.protocol)) ||
        o.value("wireProfileId") != wire(c) || o.value("securityProfileId") != security(c.security))
      continue;
    SessionPlan p{
        o.value("controlBindingId").toString().toStdString(),
        o.value("sessionId").toString().toStdString(),
        *attempt,
        o.value("senderPeerId").toString().toStdString(),
        o.value("receiverPeerId").toString().toStdString(),
        *sr,
        *rr,
        c,
        o.value("authorizationScopeDigest").toString().toStdString(),
        {std::chrono::milliseconds(startup), std::chrono::milliseconds(stop), std::chrono::milliseconds(cleanup),
         static_cast<uint32_t>(attempts)}
    };
    const auto canonical = canonicalPlan(p);
    const auto canonicalObject = QJsonDocument::fromJson(canonical).object();
    if (canonicalObject != o || validatePlan(p, sender, receiver, requirements, selection) != Rejection::None ||
        !validId(expectedHash.toStdString(), 32) || planHash(p) != expectedHash) {
      fail(error, "PlanMismatch");
      return {};
    }
    error = {};
    return p;
  }
  fail(error, "NoCompatiblePipeline");
  return {};
}
ControlBinding::ControlBinding(QString local, QString peer, QString connection)
    : m_local(std::move(local)),
      m_peer(std::move(peer)),
      m_connection(std::move(connection)),
      m_nonce(randomHex())
{
  if (!validId(m_local.toStdString(), 32) || !validId(m_peer.toStdString(), 32) || m_local == m_peer ||
      m_connection.isEmpty() || m_connection.size() > 256)
    throw std::invalid_argument("invalid authenticated control identity");
}
QByteArray ControlBinding::hello() const
{
  return QJsonDocument(
             QJsonObject{{"protocol", "display-infra-bind/1"}, {"senderPeerId", m_local}, {"nonce", m_nonce}}
  ).toJson(QJsonDocument::Compact);
}
bool ControlBinding::acceptHello(
    const QByteArray &bytes, const QString &peer, const QString &connection, ControlError &error
)
{
  if (peer != m_peer || connection != m_connection)
    return fail(error, "ControlBindingMismatch");
  if (bytes.size() > 1024)
    return fail(error, "MessageBudgetExceeded");
  const auto o = parseControlObject(bytes, error);
  if (!o)
    return false;
  if (!keys(*o, {"protocol", "senderPeerId", "nonce"}) || o->value("protocol") != "display-infra-bind/1" ||
      o->value("senderPeerId") != m_peer || !o->value("nonce").isString() ||
      !validId(o->value("nonce").toString().toStdString(), 32))
    return fail(error, "InvalidBindingHello");
  const auto nonce = o->value("nonce").toString();
  if (!m_remoteNonce.isEmpty() && nonce != m_remoteNonce)
    return fail(error, "ControlBindingMismatch");
  m_remoteNonce = nonce;
  QByteArray transcript("BMouse display-infra-bind/1\0", 28);
  const bool localFirst = m_local < m_peer;
  for (const auto &field :
       {localFirst ? m_local : m_peer, localFirst ? m_nonce : m_remoteNonce, localFirst ? m_peer : m_local,
        localFirst ? m_remoteNonce : m_nonce})
    transcript += QByteArray::fromHex(field.toLatin1());
  m_binding = QString::fromLatin1(QCryptographicHash::hash(transcript, QCryptographicHash::Sha256).toHex());
  error = {};
  return true;
}
QByteArray ControlBinding::encode(const Envelope &e, ControlError &error)
{
  if (!ready()) {
    fail(error, "ControlBindingMismatch");
    return {};
  }
  if (!envelopeFields(e) || m_tx == std::numeric_limits<uint64_t>::max()) {
    fail(error, "InvalidEnvelope");
    return {};
  }
  if (!validateControlBody(e, m_local, error))
    return {};
  QJsonObject o{
      {"protocol", "display-infra/1"},
      {"type", e.type},
      {"messageId", e.messageId},
      {"controlBindingId", m_binding},
      {"senderPeerId", m_local},
      {"sequence", QString::number(m_tx + 1)},
      {"body", e.body}
  };
  if (!e.sessionId.isEmpty())
    o.insert("sessionId", e.sessionId);
  if (e.attempt)
    o.insert("attemptId", QString::number(e.attempt));
  if (!e.planHash.isEmpty())
    o.insert("planHash", e.planHash);
  const auto bytes = QJsonDocument(o).toJson(QJsonDocument::Compact);
  if (!parseControlObject(bytes, error))
    return {};
  ++m_tx;
  error = {};
  return bytes;
}
std::optional<Envelope>
ControlBinding::receive(const QByteArray &bytes, const QString &peer, const QString &connection, ControlError &error)
{
  if (!ready() || peer != m_peer || connection != m_connection) {
    fail(error, "ControlBindingMismatch");
    return {};
  }
  const auto o = parseControlObject(bytes, error);
  if (!o)
    return {};
  if (!keys(
          *o, {"protocol", "type", "messageId", "controlBindingId", "senderPeerId", "sequence", "body"},
          {"sessionId", "attemptId", "planHash"}
      ) ||
      o->value("protocol") != "display-infra/1" || o->value("controlBindingId") != m_binding ||
      o->value("senderPeerId") != m_peer || !o->value("body").isObject()) {
    fail(error, "InvalidEnvelope");
    return {};
  }
  for (const auto *k : {"type", "messageId", "sequence"})
    if (!o->value(QLatin1String(k)).isString()) {
      fail(error, "InvalidEnvelope");
      return {};
    }
  for (const auto *k : {"sessionId", "attemptId", "planHash"})
    if (o->contains(QLatin1String(k)) && !o->value(QLatin1String(k)).isString()) {
      fail(error, "InvalidEnvelope");
      return {};
    }
  const auto sequence = parseCounter(o->value("sequence").toString().toStdString());
  const auto attempt = o->contains("attemptId") ? parseCounter(o->value("attemptId").toString().toStdString())
                                                : std::optional<uint64_t>(0);
  if (!sequence || !attempt) {
    fail(error, "InvalidCounter");
    return {};
  }
  Envelope e{
      o->value("type").toString(),
      o->value("messageId").toString(),
      o->value("sessionId").toString(),
      o->value("planHash").toString(),
      *attempt,
      *sequence,
      o->value("body").toObject()
  };
  if (!envelopeFields(e)) {
    fail(error, "InvalidEnvelope");
    return {};
  }
  if (!validateControlBody(e, m_peer, error))
    return {};
  if (*sequence <= m_rx) {
    fail(error, "ReplayRejected");
    return {};
  }
  m_rx = *sequence;
  error = {};
  return e;
}
} // namespace deskflow::display::infra
