// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "ControlBodyRegistry.h"
#include "display/supervision/MediaWorkerContracts.h"
#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <set>
namespace deskflow::display::infra {
namespace {
bool fields(const QJsonObject &o, std::initializer_list<const char *> names)
{
  if (o.size() != qsizetype(names.size()))
    return false;
  return std::all_of(names.begin(), names.end(), [&](const auto *n) { return o.contains(QLatin1String(n)); });
}
bool choice(const QJsonValue &v, std::initializer_list<const char *> values)
{
  if (!v.isString())
    return false;
  return std::any_of(values.begin(), values.end(), [&](const auto *s) { return v.toString() == QLatin1String(s); });
}
bool id(const QJsonValue &v, size_t bytes)
{
  return v.isString() && validId(v.toString().toStdString(), bytes);
}
bool name(const QJsonValue &v, size_t maximum = 64, bool empty = false)
{
  return v.isString() && ((empty && v.toString().isEmpty()) || validName(v.toString().toStdString(), maximum));
}
bool counter(const QJsonValue &v, bool zero = false)
{
  return v.isString() && ((zero && v.toString() == "0") || parseCounter(v.toString().toStdString()).has_value());
}
bool number(const QJsonValue &v, uint64_t maximum, bool zero = false)
{
  const double d = v.toDouble(-1);
  return v.isDouble() && std::isfinite(d) && d >= (zero ? 0 : 1) && d <= double(maximum) && std::floor(d) == d;
}
bool flag(const QJsonObject &o, const char *n)
{
  return o.value(QLatin1String(n)).isBool();
}
bool format(const QJsonValue &v, WireProtocol protocol)
{
  if (!v.isObject())
    return false;
  const auto o = v.toObject();
  if (!fields(
          o, {"width", "height", "fpsNumerator", "fpsDenominator", "graphicsMode", "colorProfileId", "cursorMode"}
      ) ||
      !number(o["width"], 1920) || o["width"] != 1920 || o["height"] != 1080 || o["fpsNumerator"] != 30 ||
      o["fpsDenominator"] != 1 || !choice(o["colorProfileId"], {"sdr-bt709-limited-8bit", "sdr-bt709-full-8bit"}) ||
      !choice(o["cursorMode"], {"embedded", "metadata"}))
    return false;
  return protocol == WireProtocol::WebRtc ? choice(o["graphicsMode"], {"h264-constrained-baseline"})
                                          : choice(o["graphicsMode"], {"rdp-avc420", "rdp-avc444"});
}
bool protocol(const QJsonValue &v)
{
  return choice(v, {"webrtc", "rdp"});
}
bool security(const QJsonValue &v, WireProtocol p)
{
  return p == WireProtocol::WebRtc ? choice(v, {"webrtc.authenticated-dtls-srtp.v1"})
                                   : choice(v, {"rdp.authenticated-tls13.v1"});
}
bool revisions(const QJsonObject &o)
{
  return counter(o["senderCapabilityRevision"]) && counter(o["receiverCapabilityRevision"]);
}
bool accepted(const QJsonObject &o)
{
  return flag(o, "accepted") && name(o["reason"], 64, true) &&
         (o["accepted"].toBool() ? o["reason"].toString().isEmpty() : !o["reason"].toString().isEmpty());
}
bool profile(const QJsonObject &o, const QString &environment)
{
  if (!fields(
          o, {"id", "implementation", "implementationVersion", "environment", "direction", "backend", "readiness",
              "reason", "adapterVersion", "security", "formats", "memory", "hardware", "rateFeedback",
              "refreshFeedback", "presentationEvidence", "verifiedCleanup", "resourceCoupledToWorker"}
      ) ||
      !name(o["id"], 128) || !name(o["implementation"], 128) || !name(o["implementationVersion"]) ||
      o["environment"] != environment || !choice(o["direction"], {"send", "receive"}) || !protocol(o["backend"]) ||
      !choice(o["readiness"], {"unknown", "checking", "available", "conditional", "unavailable"}) ||
      !name(o["reason"], 64, true) || o["adapterVersion"] != 1 || !o["security"].isArray() || !o["formats"].isArray() ||
      !choice(o["memory"], {"cpu", "d3d11", "iosurface", "dmabuf", "backend-native"}))
    return false;
  for (const auto *n :
       {"hardware", "rateFeedback", "refreshFeedback", "presentationEvidence", "verifiedCleanup",
        "resourceCoupledToWorker"})
    if (!flag(o, n))
      return false;
  if ((choice(o["readiness"], {"conditional", "unavailable"}) && o["reason"].toString().isEmpty()) ||
      (o["readiness"] == "available" && !o["reason"].toString().isEmpty()))
    return false;
  const auto p = o["backend"] == "webrtc" ? WireProtocol::WebRtc : WireProtocol::Rdp;
  const auto securities = o["security"].toArray(), formats = o["formats"].toArray();
  // V1 declares exactly one protocol-compatible security profile.
  if (securities.size() != 1 || !security(securities[0], p) || formats.isEmpty() || formats.size() > 8)
    return false;
  for (qsizetype i = 0; i < formats.size(); ++i) {
    if (!format(formats[i], p))
      return false;
    for (qsizetype j = 0; j < i; ++j)
      if (formats[i] == formats[j])
        return false;
  }
  return true;
}
bool capabilities(const QJsonObject &o, const QString &peer)
{
  if (!fields(
          o, {"peerIdentity", "revision", "environment", "directNetworkAllowed", "sessionAvailable", "cleanupVerified",
              "pipelines"}
      ) ||
      o["peerIdentity"] != peer || !counter(o["revision"]) || !id(o["environment"], 32) ||
      !flag(o, "directNetworkAllowed") || !flag(o, "sessionAvailable") || !flag(o, "cleanupVerified") ||
      !o["pipelines"].isArray() || o["pipelines"].toArray().size() > 16)
    return false;
  QSet<QString> unique;
  for (const auto &v : o["pipelines"].toArray()) {
    if (!v.isObject() || !profile(v.toObject(), o["environment"].toString()))
      return false;
    const auto p = v.toObject();
    const auto key = p["direction"].toString() + ":" + p["id"].toString();
    if (unique.contains(key))
      return false;
    unique.insert(key);
  }
  return true;
}
bool minimum(const QJsonValue &v)
{
  if (!v.isObject())
    return false;
  const auto o = v.toObject();
  return fields(
             o, {"width", "height", "fpsNumerator", "fpsDenominator", "colorProfileId", "cursorMode", "requireHardware",
                 "allowAudio", "allowRemoteInput", "allowRedirection"}
         ) &&
         o["width"] == 1920 && o["height"] == 1080 && o["fpsNumerator"] == 30 && o["fpsDenominator"] == 1 &&
         choice(o["colorProfileId"], {"sdr-bt709-limited-8bit", "sdr-bt709-full-8bit"}) &&
         choice(o["cursorMode"], {"embedded", "metadata"}) && flag(o, "requireHardware") && o["allowAudio"] == false &&
         o["allowRemoteInput"] == false && o["allowRedirection"] == false;
}
bool mediaSignal(const QJsonObject &o)
{
  if (!fields(o, {"backend", "signalSequence", "kind", "description", "candidate", "mid", "mediaLine"}) ||
      o["backend"] != "webrtc" || !counter(o["signalSequence"]) ||
      !choice(o["kind"], {"offer", "answer", "ice", "end-of-candidates"}) || !o["description"].isString() ||
      !o["candidate"].isString() || !o["mid"].isString() || !number(o["mediaLine"], 0, true))
    return false;
  const auto kind = o["kind"].toString();
  WorkerMediaSignal signal;
  signal.kind = kind == "offer"    ? WebRtcSignalKind::Offer
                : kind == "answer" ? WebRtcSignalKind::Answer
                : kind == "ice"    ? WebRtcSignalKind::Ice
                                   : WebRtcSignalKind::EndOfCandidates;
  signal.description = o["description"].toString();
  signal.candidate = o["candidate"].toString();
  signal.mid = o["mid"].toString();
  return validMediaSignal(signal);
}
bool body(const Envelope &e, const QString &sender)
{
  const auto &o = e.body;
  const auto &type = e.type;
  if ((type == "PlanOffer" || type == "PlanAnswer" || type == "PlanPropose" || type == "PlanAccept" ||
       type == "PlanCommit" || type == "PlanCommitted" || type == "Stop" || type == "Stopped") &&
      !id(o["operationId"], 16))
    return false;
  const auto samePlan = [&] { return id(o["planHash"], 32) && o["planHash"] == e.planHash; };
  if (type == "CapabilitiesSnapshot")
    return capabilities(o, sender);
  if (type == "PlanOffer") {
    if (!fields(
            o, {"operationId", "senderPeerId", "receiverPeerId", "senderCapabilityRevision",
                "receiverCapabilityRevision", "minimum", "candidateRefs"}
        ) ||
        !id(o["senderPeerId"], 32) || !id(o["receiverPeerId"], 32) || o["senderPeerId"] == o["receiverPeerId"] ||
        (o["senderPeerId"] != sender && o["receiverPeerId"] != sender) || !revisions(o) || !minimum(o["minimum"]) ||
        !o["candidateRefs"].isArray())
      return false;
    const auto refs = o["candidateRefs"].toArray();
    if (refs.isEmpty() || refs.size() > 8)
      return false;
    QSet<QString> unique;
    for (const auto &ref : refs) {
      if (!id(ref, 32) || unique.contains(ref.toString()))
        return false;
      unique.insert(ref.toString());
    }
    return true;
  }
  if (type == "PlanAnswer")
    return fields(
               o, {"operationId", "accepted", "reason", "candidateRef", "senderCapabilityRevision",
                   "receiverCapabilityRevision"}
           ) &&
           accepted(o) && revisions(o) &&
           (o["accepted"].toBool() ? id(o["candidateRef"], 32)
                                   : (o["candidateRef"].isString() && o["candidateRef"].toString().isEmpty()));
  if (type == "PlanPropose")
    return fields(o, {"operationId", "planHash", "planRef"}) && samePlan() && id(o["planRef"], 32) &&
           o["planRef"] == o["planHash"];
  if (type == "PlanAccept")
    return fields(
               o, {"operationId", "planHash", "accepted", "reason", "senderCapabilityRevision",
                   "receiverCapabilityRevision"}
           ) &&
           samePlan() && revisions(o) && accepted(o);
  if (type == "PlanCommit")
    return fields(
               o, {"operationId", "planHash", "senderAuthorizationDigest", "receiverAuthorizationDigest",
                   "senderBarrierRef", "receiverBarrierRef"}
           ) &&
           samePlan() && id(o["senderAuthorizationDigest"], 32) && id(o["receiverAuthorizationDigest"], 32) &&
           id(o["senderBarrierRef"], 16) && id(o["receiverBarrierRef"], 16);
  if (type == "PlanCommitted")
    return fields(o, {"operationId", "planHash", "committed", "localCommitRef"}) && samePlan() &&
           o["committed"] == true && id(o["localCommitRef"], 16);
  if (type == "MediaSignal")
    return mediaSignal(o);
  if (type == "FrameCorrelation") {
    ControlError error;
    return decodeFrameCorrelationBody(o, error).has_value();
  }
  if (type == "EndpointReady")
    return fields(o, {"backend", "resourceRef", "prepared"}) && protocol(o["backend"]) && id(o["resourceRef"], 16) &&
           o["prepared"] == true;
  if (type == "MediaReady")
    return fields(o, {"backend", "planHash", "video", "securityProfileId"}) && protocol(o["backend"]) && samePlan() &&
           format(o["video"], o["backend"] == "webrtc" ? WireProtocol::WebRtc : WireProtocol::Rdp) &&
           security(o["securityProfileId"], o["backend"] == "webrtc" ? WireProtocol::WebRtc : WireProtocol::Rdp);
  if (type == "FirstFrameEvidence")
    return fields(o, {"epoch", "frame", "sourceResource", "targetResource", "targetValid", "evidence"}) &&
           number(o["epoch"], UINT32_MAX) && counter(o["frame"]) && id(o["sourceResource"], 16) &&
           id(o["targetResource"], 16) && flag(o, "targetValid") &&
           choice(o["evidence"], {"decoded", "submitted", "native-paint-confirmed", "compositor-presented"});
  if (type == "HealthSummary")
    return fields(o, {"sequence", "health", "valid", "captured", "decoded", "presented", "retainedCpuBytes"}) &&
           counter(o["sequence"]) && choice(o["health"], {"healthy", "idle", "degraded", "failed"}) &&
           flag(o, "valid") && counter(o["captured"], true) && counter(o["decoded"], true) &&
           counter(o["presented"], true) && number(o["retainedCpuBytes"], 64 * 1024 * 1024, true) &&
           (o["valid"].toBool() ||
            (o["captured"] == "0" && o["decoded"] == "0" && o["presented"] == "0" && o["retainedCpuBytes"] == 0));
  if (type == "Stop")
    return fields(o, {"operationId", "reason", "lastEpoch", "lastSequence"}) &&
           choice(o["reason"], {"Cancelled", "Revoked", "ConnectionLost", "Deadline", "Shutdown", "BackendFailed"}) &&
           number(o["lastEpoch"], UINT32_MAX, true) && counter(o["lastSequence"], true);
  if (type == "Stopped")
    return fields(o, {"operationId", "stopped", "localCleanup", "lastEpoch"}) && o["stopped"] == true &&
           choice(o["localCleanup"], {"verified", "pending", "unknown"}) && number(o["lastEpoch"], UINT32_MAX, true);
  if (type == "Error")
    return fields(o, {"code", "stage", "relatedMessageId", "resourceImpact"}) && name(o["code"]) && name(o["stage"]) &&
           id(o["relatedMessageId"], 16) &&
           choice(o["resourceImpact"], {"none", "possibly-created", "owned", "unknown"});
  return false;
}
} // namespace
QJsonObject encodeWebRtcSignalBody(const ControlMediaSignal &value, ControlError &error)
{
  if (!value.sequence || !validMediaSignal(value.signal)) {
    error = {"InvalidMediaSignal", {}};
    return {};
  }
  const char *kind = nullptr;
  switch (value.signal.kind) {
  case WebRtcSignalKind::Offer:
    kind = "offer";
    break;
  case WebRtcSignalKind::Answer:
    kind = "answer";
    break;
  case WebRtcSignalKind::Ice:
    kind = "ice";
    break;
  case WebRtcSignalKind::EndOfCandidates:
    kind = "end-of-candidates";
    break;
  }
  error = {};
  return {
      {"backend", "webrtc"},
      {"signalSequence", QString::number(value.sequence)},
      {"kind", QString::fromLatin1(kind)},
      {"description", value.signal.description},
      {"candidate", value.signal.candidate},
      {"mid", value.signal.mid},
      {"mediaLine", int(value.signal.mediaLine)}
  };
}
std::optional<ControlMediaSignal> decodeWebRtcSignalBody(const QJsonObject &o, ControlError &error)
{
  if (!mediaSignal(o)) {
    error = {"InvalidMediaSignal", {}};
    return {};
  }
  ControlMediaSignal value;
  value.sequence = *parseCounter(o["signalSequence"].toString().toStdString());
  const auto kind = o["kind"].toString();
  value.signal.kind = kind == "offer"    ? WebRtcSignalKind::Offer
                      : kind == "answer" ? WebRtcSignalKind::Answer
                      : kind == "ice"    ? WebRtcSignalKind::Ice
                                         : WebRtcSignalKind::EndOfCandidates;
  value.signal.description = o["description"].toString();
  value.signal.candidate = o["candidate"].toString();
  value.signal.mid = o["mid"].toString();
  error = {};
  return value;
}
QJsonObject encodeFrameCorrelationBody(const WebRtcFrameCorrelation &m, ControlError &error)
{
  if (!validFrameCorrelation(m)) {
    error = {"InvalidFrameCorrelation", {}};
    return {};
  }
  error = {};
  return {
      {"backend", "webrtc"},
      {"mappingProfile", "control.rtp-map.v1"},
      {"sourceResource", m.sourceResource},
      {"captureClockId", m.captureClockId},
      {"topologyGeneration", QString::number(m.topologyGeneration)},
      {"frame", QString::number(m.frame)},
      {"captureFrame", QString::number(m.captureFrame)},
      {"captureNanoseconds", QString::number(m.captureNanoseconds)},
      {"epoch", double(m.epoch)},
      {"ssrc", double(m.ssrc)},
      {"rtpTimestamp", double(m.rtpTimestamp)},
      {"repeated", m.repeated}
  };
}
std::optional<WebRtcFrameCorrelation> decodeFrameCorrelationBody(const QJsonObject &o, ControlError &error)
{
  if (!fields(
          o, {"backend", "mappingProfile", "sourceResource", "captureClockId", "topologyGeneration", "frame",
              "captureFrame", "captureNanoseconds", "epoch", "ssrc", "rtpTimestamp", "repeated"}
      ) ||
      o["backend"] != "webrtc" || o["mappingProfile"] != "control.rtp-map.v1" || !id(o["sourceResource"], 16) ||
      !name(o["captureClockId"], 56) || !counter(o["topologyGeneration"]) || !counter(o["frame"]) ||
      !counter(o["captureFrame"]) || !counter(o["captureNanoseconds"]) || !number(o["epoch"], UINT32_MAX) ||
      !number(o["ssrc"], UINT32_MAX, true) || !number(o["rtpTimestamp"], UINT32_MAX, true) || !flag(o, "repeated")) {
    error = {"InvalidFrameCorrelation", {}};
    return {};
  }
  WebRtcFrameCorrelation m;
  m.sourceResource = o["sourceResource"].toString();
  m.captureClockId = o["captureClockId"].toString();
  m.topologyGeneration = *parseCounter(o["topologyGeneration"].toString().toStdString());
  m.frame = *parseCounter(o["frame"].toString().toStdString());
  m.captureFrame = *parseCounter(o["captureFrame"].toString().toStdString());
  m.captureNanoseconds = *parseCounter(o["captureNanoseconds"].toString().toStdString());
  m.epoch = uint32_t(o["epoch"].toDouble());
  m.ssrc = uint32_t(o["ssrc"].toDouble());
  m.rtpTimestamp = uint32_t(o["rtpTimestamp"].toDouble());
  m.repeated = o["repeated"].toBool();
  error = {};
  return m;
}
bool validateControlBody(const Envelope &e, const QString &sender, ControlError &error)
{
  if (!validId(sender.toStdString(), 32) || !body(e, sender)) {
    error = {"InvalidControlBody", e.type};
    return false;
  }
  error = {};
  return true;
}
} // namespace deskflow::display::infra
