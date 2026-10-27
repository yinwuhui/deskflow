// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/control/InfrastructureControl.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <iostream>
#include <stdexcept>
using namespace deskflow::display::infra;
static void require(bool v, const char *why)
{
  if (!v)
    throw std::runtime_error(why);
}
static void strictJson()
{
  ControlError e;
  for (const auto &bad :
       {QByteArray("{\"a\":1,\"a\":2}"), QByteArray("{\"a\":1,\"\\u0061\":2}"),
        QByteArray("{\"outer\":{\"x\":1,\"x\":2}}"), QByteArray("{\"x\":\"\\ud800\"}"),
        QByteArray("{\"x\":\"\xed\xa0\x80\"}"), QByteArray("{\"x\":NaN}"), QByteArray("{}{}")})
    require(!parseControlObject(bad, e), "ambiguous or invalid JSON accepted");
  QByteArray deep = "0";
  for (int i = 0; i < 18; ++i)
    deep = "[" + deep + "]";
  require(!parseControlObject("{\"x\":" + deep + "}", e), "unbounded JSON depth");
  require(!parseControlObject(QByteArray(65537, ' '), e), "message budget exceeded");
  require(
      parseControlObject("{\"x\":\"\\ud83d\\ude00\",\"items\":[1,true,null]}", e).has_value(), "valid Unicode rejected"
  );
}
static void bindingAndReplay()
{
  const QString a(64, 'a'), b(64, 'b');
  ControlError e;
  ControlBinding left(a, b, "left-local-connection"), right(b, a, "right-local-connection");
  require(left.acceptHello(right.hello(), b, "left-local-connection", e), "left binding failed");
  require(right.acceptHello(left.hello(), a, "right-local-connection", e), "right binding failed");
  require(left.id() == right.id(), "local connection references leaked into common digest");
  Envelope message;
  message.type = "CapabilitiesSnapshot";
  message.messageId = QString(32, '1');
  message.body = {{"revision", "1"}};
  const auto encoded = left.encode(message, e);
  require(!encoded.isEmpty(), "encode failed");
  require(right.receive(encoded, a, "right-local-connection", e).has_value(), "authenticated envelope rejected");
  require(!right.receive(encoded, a, "right-local-connection", e), "sequence replay accepted");
  const auto next = left.encode(message, e);
  require(
      right.receive(next, a, "right-local-connection", e).has_value(), "idempotent command with fresh sequence rejected"
  );
  require(
      !right.receive(left.encode(message, e), a, "different-local-connection", e), "cross-connection replay accepted"
  );
  ControlBinding other(a, b, "other-instance");
  require(other.acceptHello(right.hello(), b, "other-instance", e), "fresh peer handshake failed");
  require(!right.acceptHello(other.hello(), a, "right-local-connection", e), "nonce replaced existing channel binding");
  require(!other.receive(encoded, b, "other-instance", e), "old epoch envelope accepted on new nonce binding");
}
static void fieldAndSizeGates()
{
  const QString a(64, 'a'), b(64, 'b');
  ControlError e;
  ControlBinding left(a, b, "a"), right(b, a, "b");
  left.acceptHello(right.hello(), b, "a", e);
  right.acceptHello(left.hello(), a, "b", e);
  Envelope m;
  m.type = "MediaSignal";
  m.messageId = QString(32, '1');
  m.sessionId = QString(32, '2');
  m.attempt = 1;
  require(left.encode(m, e).isEmpty(), "signal without plan binding accepted");
  m.planHash = QString(64, 'c');
  const auto valid = left.encode(m, e);
  require(!valid.isEmpty(), "bound signal rejected");
  auto o = QJsonDocument::fromJson(valid).object();
  o.insert("program", "/bin/sh");
  require(
      !right.receive(QJsonDocument(o).toJson(QJsonDocument::Compact), a, "b", e), "network executable field accepted"
  );
  require(right.receive(valid, a, "b", e).has_value(), "invalid message consumed valid sequence");
  m.body = {{"payload", QString(65536, 'x')}};
  require(left.encode(m, e).isEmpty(), "oversized signal accepted");
  require(!right.acceptHello("{\"protocol\":\"display-infra-bind/1\"}", a, "b", e), "incomplete binding accepted");
}
static SessionPlan example(CapabilitySnapshot &a, CapabilitySnapshot &b, SelectionPolicy &policy)
{
  a.peerIdentity = std::string(64, 'b');
  b.peerIdentity = std::string(64, 'd');
  a.revision = 4;
  b.revision = 7;
  a.environment = std::string(64, 'c');
  b.environment = std::string(64, 'f');
  a.directNetworkAllowed = b.directNetworkAllowed = a.sessionAvailable = b.sessionAvailable = a.cleanupVerified =
      b.cleanupVerified = true;
  PipelineProfile p;
  p.id = "macos.capture.webrtc.h264.sdr.v1";
  p.implementation = "test";
  p.implementationVersion = "1";
  p.environment = a.environment;
  p.readiness = Readiness::Available;
  p.security = {SecurityProfile::WebRtcAuthenticatedDtlsSrtp};
  p.formats = {VideoFormat{}};
  p.rateFeedback = p.refreshFeedback = p.presentationEvidence = p.verifiedCleanup = true;
  a.pipelines = {p};
  p.id = "linux.receive.webrtc.h264.sdr.v1";
  p.direction = Direction::Receive;
  p.environment = b.environment;
  b.pipelines = {p};
  policy.allowUncertifiedDiagnostics = true;
  return {
      std::string(64, 'a'),
      std::string(32, '2'),
      1,
      a.peerIdentity,
      b.peerIdentity,
      4,
      7,
      plan(a, b, {}, policy).candidates.front(),
      std::string(64, 'e'),
      {}
  };
}
static void planRoundTrip()
{
  CapabilitySnapshot a, b;
  SelectionPolicy policy;
  const auto p = example(a, b, policy);
  ControlError error;
  const auto bytes = canonicalPlan(p);
  const auto hash = planHash(p);
  const auto parsed = decodePlan(bytes, hash, a, b, {}, policy, error);
  require(parsed && *parsed == p, "plan roundtrip lost binding");
  const auto indented = QJsonDocument::fromJson(bytes).toJson();
  require(decodePlan(indented, hash, a, b, {}, policy, error).has_value(), "whitespace changed canonical plan hash");
  require(!decodePlan(bytes, QString(64, 'f'), a, b, {}, policy, error), "incorrect plan hash accepted");
  auto o = QJsonDocument::fromJson(bytes).object();
  auto video = o.value("video").toObject();
  video.insert("width", 3840);
  o.insert("video", video);
  require(!decodePlan(QJsonDocument(o).toJson(), hash, a, b, {}, policy, error), "plan dimensions substituted");
  o = QJsonDocument::fromJson(bytes).object();
  auto permissions = o.value("policy").toObject();
  permissions.insert("allowRemoteInput", true);
  o.insert("policy", permissions);
  require(!decodePlan(QJsonDocument(o).toJson(), hash, a, b, {}, policy, error), "input granted by media plan");
  ++b.revision;
  require(!decodePlan(bytes, hash, a, b, {}, policy, error), "stale plan revision accepted");
}
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  try {
    if (argc != 2)
      throw std::runtime_error("one test required");
    const std::string name = argv[1];
    if (name == "StrictJson")
      strictJson();
    else if (name == "BindingReplay")
      bindingAndReplay();
    else if (name == "EnvelopeGates")
      fieldAndSizeGates();
    else if (name == "PlanCanonicalization")
      planRoundTrip();
    else
      throw std::runtime_error("unknown test");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
