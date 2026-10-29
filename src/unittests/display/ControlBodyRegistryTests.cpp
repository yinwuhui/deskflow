// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/control/ControlBodyRegistry.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <functional>
#include <iostream>
#include <stdexcept>
using namespace deskflow::display::infra;
static void require(bool v, const char *why)
{
  if (!v)
    throw std::runtime_error(why);
}
static const QString peer(64, 'a'), remote(64, 'b'), digest(64, 'c'), ref(32, 'd');
static const QStringList types{"CapabilitiesSnapshot", "PlanOffer",     "PlanAnswer",    "PlanPropose",
                               "PlanAccept",           "PlanCommit",    "PlanCommitted", "MediaSignal",
                               "FrameCorrelation",     "EndpointReady", "MediaReady",    "FirstFrameEvidence",
                               "HealthSummary",        "Stop",          "Stopped",       "Error"};
static QJsonObject video()
{
  return {
      {"width", 1920},
      {"height", 1080},
      {"fpsNumerator", 30},
      {"fpsDenominator", 1},
      {"graphicsMode", "h264-constrained-baseline"},
      {"colorProfileId", "sdr-bt709-limited-8bit"},
      {"cursorMode", "embedded"}
  };
}
static QJsonObject pipeline()
{
  return {
      {"id", "test.sender"},
      {"implementation", "test"},
      {"implementationVersion", "1"},
      {"environment", digest},
      {"direction", "send"},
      {"backend", "webrtc"},
      {"readiness", "available"},
      {"reason", ""},
      {"adapterVersion", 1},
      {"security", QJsonArray{"webrtc.authenticated-dtls-srtp.v1"}},
      {"formats", QJsonArray{video()}},
      {"memory", "cpu"},
      {"hardware", false},
      {"rateFeedback", true},
      {"refreshFeedback", true},
      {"presentationEvidence", true},
      {"verifiedCleanup", true},
      {"resourceCoupledToWorker", true}
  };
}
static Envelope envelope(const QString &type)
{
  Envelope e{type, ref, ref, digest, 1, 0, {}};
  auto &o = e.body;
  if (type == "CapabilitiesSnapshot")
    o = {
        {"peerIdentity", peer},
        {"revision", "1"},
        {"environment", digest},
        {"directNetworkAllowed", true},
        {"sessionAvailable", true},
        {"cleanupVerified", true},
        {"pipelines", QJsonArray{pipeline()}}
    };
  else if (type == "PlanOffer") {
    auto min = video();
    min.remove("graphicsMode");
    min.insert("requireHardware", false);
    min.insert("allowAudio", false);
    min.insert("allowRemoteInput", false);
    min.insert("allowRedirection", false);
    o = {
        {"senderPeerId", peer},
        {"receiverPeerId", remote},
        {"senderCapabilityRevision", "1"},
        {"receiverCapabilityRevision", "2"},
        {"minimum", min},
        {"candidateRefs", QJsonArray{digest}}
    };
  } else if (type == "PlanAnswer")
    o = {
        {"accepted", true},
        {"reason", ""},
        {"candidateRef", digest},
        {"senderCapabilityRevision", "1"},
        {"receiverCapabilityRevision", "2"}
    };
  else if (type == "PlanPropose")
    o = {{"planHash", digest}, {"planRef", digest}};
  else if (type == "PlanAccept")
    o = {
        {"planHash", digest},
        {"accepted", true},
        {"reason", ""},
        {"senderCapabilityRevision", "1"},
        {"receiverCapabilityRevision", "2"}
    };
  else if (type == "PlanCommit")
    o = {
        {"planHash", digest},
        {"senderAuthorizationDigest", digest},
        {"receiverAuthorizationDigest", digest},
        {"senderBarrierRef", ref},
        {"receiverBarrierRef", ref}
    };
  else if (type == "PlanCommitted")
    o = {{"planHash", digest}, {"committed", true}, {"localCommitRef", ref}};
  else if (type == "MediaSignal")
    o = {{"backend", "webrtc"}, {"signalSequence", "1"}, {"kind", "end-of-candidates"},
         {"description", ""},   {"candidate", ""},       {"mid", ""},
         {"mediaLine", 0}};
  else if (type == "FrameCorrelation") {
    WebRtcFrameCorrelation mapping;
    mapping.sourceResource = ref;
    mapping.captureClockId = "test.monotonic";
    mapping.topologyGeneration = mapping.frame = mapping.captureFrame = mapping.captureNanoseconds = 1;
    mapping.epoch = 1;
    ControlError error;
    o = encodeFrameCorrelationBody(mapping, error);
  } else if (type == "EndpointReady")
    o = {{"backend", "webrtc"}, {"resourceRef", ref}, {"prepared", true}};
  else if (type == "MediaReady")
    o = {
        {"backend", "webrtc"},
        {"planHash", digest},
        {"video", video()},
        {"securityProfileId", "webrtc.authenticated-dtls-srtp.v1"}
    };
  else if (type == "FirstFrameEvidence")
    o = {
        {"epoch", 1},
        {"frame", "1"},
        {"sourceResource", ref},
        {"targetResource", ref},
        {"targetValid", true},
        {"evidence", "native-paint-confirmed"}
    };
  else if (type == "HealthSummary")
    o = {{"sequence", "1"}, {"health", "healthy"}, {"valid", true},        {"captured", "100"},
         {"decoded", "99"}, {"presented", "98"},   {"retainedCpuBytes", 0}};
  else if (type == "Stop")
    o = {{"reason", "Cancelled"}, {"lastEpoch", 0}, {"lastSequence", "0"}};
  else if (type == "Stopped")
    o = {{"stopped", true}, {"localCleanup", "verified"}, {"lastEpoch", 1}};
  else if (type == "Error")
    o = {{"code", "DecodeFailed"}, {"stage", "media.decode"}, {"relatedMessageId", ref}, {"resourceImpact", "owned"}};
  if (QStringList{
          "PlanOffer", "PlanAnswer", "PlanPropose", "PlanAccept", "PlanCommit", "PlanCommitted", "Stop", "Stopped"
      }
          .contains(type))
    o["operationId"] = ref;
  return e;
}
static bool valid(const Envelope &e)
{
  ControlError error;
  return validateControlBody(e, peer, error);
}
struct Connection
{
  ControlBinding left{peer, remote, "left"}, right{remote, peer, "right"};
  ControlError error;
  Connection()
  {
    require(left.acceptHello(right.hello(), remote, "left", error), "left hello");
    require(right.acceptHello(left.hello(), peer, "right", error), "right hello");
  }
};
static void allBodies()
{
  Connection connection;
  for (const auto &type : types) {
    const auto e = envelope(type);
    require(valid(e), "valid registered body rejected");
    const auto bytes = connection.left.encode(e, connection.error);
    require(!bytes.isEmpty(), "registry send integration missing");
    const auto decoded = connection.right.receive(bytes, peer, "right", connection.error);
    require(decoded && decoded->body == e.body, "registry authenticated roundtrip");
  }
  auto rejected = envelope("PlanAnswer");
  rejected.body["accepted"] = false;
  rejected.body["reason"] = "NoCommonProtocol";
  rejected.body["candidateRef"] = "";
  require(valid(rejected), "explicit rejection rejected");
  rejected.body["candidateRef"] = digest;
  require(!valid(rejected), "rejection smuggled alternate candidate");
  auto unknown = envelope("Unknown");
  require(!valid(unknown), "unknown registry type accepted");
}
static void fieldGates()
{
  for (const auto &type : types) {
    const auto good = envelope(type);
    for (const auto &key : good.body.keys()) {
      auto bad = good;
      bad.body.remove(key);
      require(!valid(bad), "required body field omitted");
      bad = good;
      bad.body[key] = QJsonValue(QJsonValue::Null);
      require(!valid(bad), "null body field accepted");
    }
    auto bad = good;
    bad.body["program"] = "/bin/sh";
    require(!valid(bad), "unknown body field accepted");
    if (good.body.contains("planHash")) {
      bad = good;
      bad.body["planHash"] = QString(64, 'e');
      require(!valid(bad), "inner plan replaced outer plan");
    }
  }
  for (const auto &bad : QJsonArray{0, 1, 1.5, "01", "18446744073709551616"}) {
    auto e = envelope("HealthSummary");
    e.body["sequence"] = bad;
    require(!valid(e), "noncanonical/overflow counter accepted");
  }
  auto health = envelope("HealthSummary");
  health.body["valid"] = false;
  require(!valid(health), "invalid metrics carried invented measurements");
  for (const auto *key : {"captured", "decoded", "presented"})
    health.body[key] = "0";
  require(valid(health), "explicit unknown metrics rejected");
}
static void capabilityGates()
{
  auto e = envelope("CapabilitiesSnapshot");
  e.body["peerIdentity"] = remote;
  require(!valid(e), "peer body replaced authenticated sender");
  for (int i = 0; i < 8; ++i) {
    e = envelope("CapabilitiesSnapshot");
    auto p = pipeline();
    switch (i) {
    case 0:
      p["environment"] = QString(64, 'e');
      break;
    case 1:
      p["hardware"] = 1;
      break;
    case 2:
      p["formats"] = QJsonArray{video(), video()};
      break;
    case 3:
      p["security"] = QJsonArray{"rdp.authenticated-tls13.v1"};
      break;
    case 4:
      p["readiness"] = "conditional";
      break;
    case 5:
      p["reason"] = "unexpected.available.reason";
      break;
    case 6:
      p["backend"] = "rdp";
      break;
    case 7:
      p["executable"] = "/bin/sh";
      break;
    }
    e.body["pipelines"] = QJsonArray{p};
    require(!valid(e), "unsafe capability accepted");
  }
  e = envelope("CapabilitiesSnapshot");
  e.body["pipelines"] = QJsonArray{pipeline(), pipeline()};
  require(!valid(e), "duplicate pipeline accepted");
  QJsonArray oversized;
  for (int i = 0; i < 17; ++i) {
    auto p = pipeline();
    p["id"] = QString("test.profile%1").arg(i);
    oversized.append(p);
  }
  e.body["pipelines"] = oversized;
  require(!valid(e), "unbounded pipelines accepted");
  e = envelope("PlanOffer");
  auto refs = e.body["candidateRefs"].toArray();
  refs.append(digest);
  e.body["candidateRefs"] = refs;
  require(!valid(e), "duplicate candidate refs accepted");
  e = envelope("PlanOffer");
  auto min = e.body["minimum"].toObject();
  min["allowRemoteInput"] = true;
  e.body["minimum"] = min;
  require(!valid(e), "media offer enabled input");
}
static void signalGates()
{
  for (const auto &kind : {"offer", "answer"}) {
    auto e = envelope("MediaSignal");
    e.body["kind"] = kind;
    e.body["description"] = "v=0\r\n";
    require(valid(e), "structural SDP rejected before backend parser");
    e.body["candidate"] = "candidate:1";
    require(!valid(e), "mixed signal payloads accepted");
    e.body["candidate"] = "";
    e.body["description"] = QString(32769, 'x');
    require(!valid(e), "oversized SDP accepted");
  }
  auto e = envelope("MediaSignal");
  e.body["kind"] = "ice";
  e.body["candidate"] = "candidate:1 1 UDP 1 127.0.0.1 5000 typ host";
  e.body["mid"] = "video0";
  require(valid(e), "structural candidate rejected; backend owns address policy");
  e.body["mediaLine"] = 1;
  require(!valid(e), "extra media track admitted");
  e.body["mediaLine"] = 0;
  e.body["candidate"] = "candidate:\nunsafe";
  require(!valid(e), "control character in ICE accepted");
  e = envelope("MediaSignal");
  e.body["backend"] = "rdp";
  require(!valid(e), "unimplemented RDP signaling admitted as WebRTC");
}
static void sequenceIsolation()
{
  Connection c;
  auto e = envelope("Stop");
  const auto bytes = c.left.encode(e, c.error);
  require(!bytes.isEmpty(), "valid stop encode");
  auto forged = QJsonDocument::fromJson(bytes).object();
  auto body = forged["body"].toObject();
  body["reason"] = "ExecuteCommand";
  forged["body"] = body;
  require(
      !c.right.receive(QJsonDocument(forged).toJson(QJsonDocument::Compact), peer, "right", c.error) &&
          c.error.code == "InvalidControlBody",
      "invalid body bypassed receive registry"
  );
  require(c.right.receive(bytes, peer, "right", c.error).has_value(), "invalid body consumed valid receive sequence");
  e.body["reason"] = "ExecuteCommand";
  require(c.left.encode(e, c.error).isEmpty(), "invalid body bypassed send registry");
  e = envelope("Stop");
  const auto next = c.left.encode(e, c.error);
  require(QJsonDocument::fromJson(next).object()["sequence"] == "2", "failed encode advanced sequence");
  require(c.right.receive(next, peer, "right", c.error).has_value(), "valid sequence after invalid body");
}
static WebRtcFrameCorrelation mappedFrame()
{
  WebRtcFrameCorrelation m;
  m.sourceResource = ref;
  m.captureClockId = "source.monotonic";
  m.topologyGeneration = m.frame = m.captureFrame = m.captureNanoseconds = UINT64_MAX;
  m.epoch = UINT32_MAX;
  m.ssrc = 0;
  m.rtpTimestamp = UINT32_MAX;
  m.repeated = true;
  return m;
}
static void frameCorrelationWire()
{
  ControlError error;
  const auto original = mappedFrame();
  auto e = envelope("FrameCorrelation");
  e.body = encodeFrameCorrelationBody(original, error);
  require(e.body["captureNanoseconds"] == "18446744073709551615", "capture time lost uint64 precision");
  Connection c;
  const auto bytes = c.left.encode(e, error);
  const auto received = c.right.receive(bytes, peer, "right", error);
  require(received.has_value(), "bound frame mapping envelope rejected");
  const auto decoded = decodeFrameCorrelationBody(received->body, error);
  require(decoded && *decoded == original, "frame correlation wire lost source/clock/wrap/repeat evidence");
  require(!c.right.receive(bytes, peer, "right", error), "frame correlation replay bypassed transport sequence");
  e.planHash.clear();
  require(c.left.encode(e, error).isEmpty(), "unbound mapping admitted");
  auto zeroWrap = original;
  zeroWrap.rtpTimestamp = 0;
  zeroWrap.ssrc = UINT32_MAX;
  const auto roundtrip = decodeFrameCorrelationBody(encodeFrameCorrelationBody(zeroWrap, error), error);
  require(roundtrip && *roundtrip == zeroWrap, "standard RTP zero/wrap values rejected");
}
static void malformedCorrelation()
{
  ControlError error;
  const auto good = encodeFrameCorrelationBody(mappedFrame(), error);
  for (const auto &key : good.keys()) {
    auto bad = good;
    bad.remove(key);
    require(!decodeFrameCorrelationBody(bad, error), "mapping required field omitted");
    bad = good;
    bad[key] = QJsonValue(QJsonValue::Null);
    require(!decodeFrameCorrelationBody(bad, error), "null mapping field admitted");
  }
  for (int i = 0; i < 12; ++i) {
    auto bad = good;
    switch (i) {
    case 0:
      bad["epoch"] = 0;
      break;
    case 1:
      bad["epoch"] = 4294967296.0;
      break;
    case 2:
      bad["ssrc"] = -1;
      break;
    case 3:
      bad["rtpTimestamp"] = 0.5;
      break;
    case 4:
      bad["frame"] = 1;
      break;
    case 5:
      bad["captureNanoseconds"] = "01";
      break;
    case 6:
      bad["repeated"] = "true";
      break;
    case 7:
      bad["mappingProfile"] = "control.rtp-map.v2";
      break;
    case 8:
      bad["backend"] = "rdp";
      break;
    case 9:
      bad["captureClockId"] = QString(57, 'x');
      break;
    case 10:
      bad["sourceResource"] = "cgdisplay:77";
      break;
    case 11:
      bad["localPointer"] = "0x1234";
      break;
    }
    require(!decodeFrameCorrelationBody(bad, error), "malformed/unbounded/native-locator mapping admitted");
  }
  auto invalid = mappedFrame();
  invalid.captureNanoseconds = 0;
  require(encodeFrameCorrelationBody(invalid, error).isEmpty(), "invented unknown capture time admitted");
}
static void typedSignalWire()
{
  for (const auto kind :
       {WebRtcSignalKind::Offer, WebRtcSignalKind::Answer, WebRtcSignalKind::Ice, WebRtcSignalKind::EndOfCandidates}) {
    ControlMediaSignal m;
    m.sequence = UINT64_MAX;
    m.signal.kind = kind;
    if (kind == WebRtcSignalKind::Offer || kind == WebRtcSignalKind::Answer)
      m.signal.description = "v=0\r\n";
    if (kind == WebRtcSignalKind::Ice) {
      m.signal.candidate = "candidate:1 1 UDP 1 127.0.0.1 5000 typ host";
      m.signal.mid = "video0";
    }
    ControlError error;
    const auto decoded = decodeWebRtcSignalBody(encodeWebRtcSignalBody(m, error), error);
    require(
        decoded && decoded->sequence == m.sequence && decoded->signal == m.signal,
        "typed signal codec lost standard payload"
    );
    m.sequence = 0;
    require(encodeWebRtcSignalBody(m, error).isEmpty(), "zero signal sequence admitted");
  }
}
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  const std::vector<std::pair<std::string, std::function<void()>>> cases{
      {"BodyRegistryAllTypes", allBodies},
      {"BodyRegistryFields", fieldGates},
      {"BodyRegistryCapabilities", capabilityGates},
      {"BodyRegistrySignals", signalGates},
      {"BodyRegistrySequence", sequenceIsolation},
      {"FrameCorrelationWire", frameCorrelationWire},
      {"FrameCorrelationMalformed", malformedCorrelation},
      {"MediaSignalTypedWire", typedSignalWire}
  };
  try {
    require(argc == 2, "one test required");
    for (const auto &[name, run] : cases)
      if (name == argv[1]) {
        run();
        return 0;
      }
    throw std::runtime_error("unknown case");
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
