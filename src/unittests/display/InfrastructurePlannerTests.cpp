// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/core/InfrastructureTypes.h"
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace deskflow::display::infra;
static void require(bool value, const char *message)
{
  if (!value)
    throw std::runtime_error(message);
}
static PipelineProfile pipeline(std::string id, Direction direction, WireProtocol protocol, std::string env)
{
  PipelineProfile p;
  p.id = std::move(id);
  p.direction = direction;
  p.protocol = protocol;
  p.implementation = protocol == WireProtocol::Rdp ? "freerdp" : "webrtc";
  p.implementationVersion = "test-1";
  p.environment = std::move(env);
  p.readiness = Readiness::Available;
  p.security = {
      protocol == WireProtocol::Rdp ? SecurityProfile::RdpAuthenticatedTls13
                                    : SecurityProfile::WebRtcAuthenticatedDtlsSrtp
  };
  VideoFormat f;
  if (protocol == WireProtocol::Rdp)
    f.graphics = GraphicsMode::RdpAvc420;
  p.formats = {f};
  p.rateFeedback = p.refreshFeedback = p.presentationEvidence = p.verifiedCleanup = true;
  return p;
}
static CapabilitySnapshot device(char peer, const std::string &name)
{
  CapabilitySnapshot c;
  c.peerIdentity = std::string(64, peer);
  c.environment = std::string(64, peer == 'a' ? 'c' : 'd');
  c.revision = 1;
  c.directNetworkAllowed = c.sessionAvailable = c.cleanupVerified = true;
  c.pipelines = {
      pipeline(name + ".rdp.send", Direction::Send, WireProtocol::Rdp, c.environment),
      pipeline(name + ".rdp.receive", Direction::Receive, WireProtocol::Rdp, c.environment),
      pipeline(name + ".webrtc.send", Direction::Send, WireProtocol::WebRtc, c.environment),
      pipeline(name + ".webrtc.receive", Direction::Receive, WireProtocol::WebRtc, c.environment)
  };
  return c;
}
static Certification certificate(const PipelineProfile &a, const PipelineProfile &b, int priority = 0)
{
  return {a.id,          b.id,          a.implementationVersion, b.implementationVersion,
          a.environment, b.environment, a.formats.front(),       a.security.front(),
          priority};
}
static SelectionPolicy diagnostics()
{
  SelectionPolicy p;
  p.allowUncertifiedDiagnostics = true;
  return p;
}
static void directionMatrix()
{
  for (const auto &senderName : {"macos", "linux", "windows"}) {
    for (const auto &receiverName : {"macos", "linux", "windows"}) {
      const auto a = device('a', senderName), b = device('b', receiverName);
      auto selected = plan(a, b, {}, diagnostics());
      require(selected.reason == Rejection::None && selected.candidates.size() == 2, "direction lacks common pipeline");
      for (const auto &c : selected.candidates)
        require(
            c.sender.direction == Direction::Send && c.receiver.direction == Direction::Receive,
            "client capability mistaken for server"
        );
    }
  }
}
static void asymmetricProtocols()
{
  auto mac = device('a', "macos"), linux = device('b', "linux");
  mac.pipelines.erase(mac.pipelines.begin());         // No Mac RDP sender.
  linux.pipelines.erase(linux.pipelines.begin() + 2); // No Linux WebRTC sender.
  auto forward = plan(mac, linux, {}, diagnostics()), reverse = plan(linux, mac, {}, diagnostics());
  require(
      forward.candidates.size() == 1 && forward.candidates.front().sender.protocol == WireProtocol::WebRtc,
      "Mac direction did not use its common backend"
  );
  require(
      reverse.candidates.size() == 1 && reverse.candidates.front().sender.protocol == WireProtocol::Rdp,
      "Linux direction incorrectly requires same protocol"
  );
}
static void safetyGates()
{
  auto a = device('a', "macos"), b = device('b', "linux");
  b.directNetworkAllowed = false;
  require(plan(a, b, {}, diagnostics()).reason == Rejection::NetworkForbidden, "Bluetooth-only bypass");
  b.directNetworkAllowed = true;
  a.cleanupVerified = false;
  require(plan(a, b, {}, diagnostics()).reason == Rejection::CleanupUnverified, "unclean owner reused");
  a.cleanupVerified = true;
  b.sessionAvailable = false;
  require(plan(a, b, {}, diagnostics()).reason == Rejection::Busy, "busy target reserved");
  b.sessionAvailable = true;
  b.peerIdentity = a.peerIdentity;
  require(plan(a, b, {}, diagnostics()).reason == Rejection::IdentityMismatch, "self identity accepted");
  b.peerIdentity = std::string(64, 'b');
  Requirements r;
  r.allowRemoteInput = true;
  require(plan(a, b, r, diagnostics()).reason == Rejection::UnsupportedFeature, "remote input silently enabled");
}
static void noProtocolConversion()
{
  auto a = device('a', "linux"), b = device('b', "macos");
  a.pipelines = {a.pipelines[0]};
  b.pipelines = {b.pipelines[3]};
  const auto selected = plan(a, b, {}, diagnostics());
  require(selected.candidates.empty() && selected.reason == Rejection::NoCommonProtocol, "invented protocol gateway");
  a.pipelines = {pipeline("rdp.client.only", Direction::Receive, WireProtocol::Rdp, a.environment)};
  require(plan(a, b, {}, diagnostics()).reason == Rejection::WrongDirection, "receive-only device claimed sending");
}
static void certificationAndInvalidation()
{
  auto a = device('a', "windows"), b = device('b', "macos");
  SelectionPolicy p;
  require(plan(a, b, {}, p).candidates.empty(), "uncertified release candidate advertised");
  p.certified.push_back(certificate(a.pipelines[0], b.pipelines[1], 4));
  auto selected = plan(a, b, {}, p);
  require(selected.candidates.size() == 1 && selected.candidates.front().certified, "local certification lost");
  a.pipelines[0].implementationVersion = "new-build";
  require(plan(a, b, {}, p).candidates.empty(), "old build certificate survived update");
  a.pipelines[0].implementationVersion = "test-1";
  a.environment = std::string(64, 'e');
  for (auto &item : a.pipelines)
    item.environment = a.environment;
  require(plan(a, b, {}, p).candidates.empty(), "GPU/environment certificate survived change");
}
static void invalidDeclarations()
{
  auto a = device('a', "macos"), b = device('b', "linux");
  a.pipelines.push_back(a.pipelines.front());
  require(plan(a, b, {}, diagnostics()).reason == Rejection::InvalidCapabilities, "duplicate pipeline ID");
  a = device('a', "macos");
  a.pipelines[0].formats.front().graphics = GraphicsMode::H264ConstrainedBaseline;
  require(!validCapabilities(a), "RDP graphics accepted ordinary H264 mode");
  a = device('a', "macos");
  a.pipelines[0].id = "../../worker";
  require(!validCapabilities(a), "network pipeline path accepted");
  a = device('a', "macos");
  a.pipelines[0].security = {SecurityProfile::WebRtcAuthenticatedDtlsSrtp};
  require(!validCapabilities(a), "incompatible protocol security accepted");
  a = device('a', "macos");
  a.pipelines[0].formats.front().fpsDenominator = 0;
  require(!validCapabilities(a), "zero clock denominator accepted");
}
static void feedbackAndConditional()
{
  auto a = device('a', "macos"), b = device('b', "linux");
  a.pipelines = {a.pipelines[2]};
  b.pipelines = {b.pipelines[3]};
  a.pipelines[0].rateFeedback = false;
  require(plan(a, b, {}, diagnostics()).candidates.empty(), "fixed encoded input claims adaptive feedback");
  a.pipelines[0].rateFeedback = true;
  b.pipelines[0].presentationEvidence = false;
  require(plan(a, b, {}, diagnostics()).candidates.empty(), "decode-only receiver claims presentation");
  b.pipelines[0].presentationEvidence = true;
  a.pipelines[0].readiness = Readiness::Conditional;
  const auto selected = plan(a, b, {}, diagnostics());
  require(
      selected.candidates.size() == 1 && selected.candidates.front().requiresConditionalProbe,
      "unprobed permission hidden"
  );
  SessionPlan p{
      std::string(64, 'e'),        std::string(32, '1'), 1, a.peerIdentity, b.peerIdentity, a.revision, b.revision,
      selected.candidates.front(), std::string(64, 'f'), {}
  };
  require(validatePlan(p, a, b, {}, diagnostics()) == Rejection::NotReady, "conditional plan committed as ready");
}
static void planBinding()
{
  auto a = device('a', "macos"), b = device('b', "linux");
  const auto candidate = plan(a, b, {}, diagnostics()).candidates.front();
  SessionPlan p{std::string(64, 'e'), std::string(32, '1'), 1,         a.peerIdentity,       b.peerIdentity,
                a.revision,           b.revision,           candidate, std::string(64, 'f'), {}};
  require(validatePlan(p, a, b, {}, diagnostics()) == Rejection::None, "valid negotiated plan rejected");
  ++b.revision;
  require(validatePlan(p, a, b, {}, diagnostics()) == Rejection::StaleCapabilities, "stale capability commit");
  --b.revision;
  p.receiverIdentity = a.peerIdentity;
  require(validatePlan(p, a, b, {}, diagnostics()) == Rejection::IdentityMismatch, "peer substituted after selection");
  p.receiverIdentity = b.peerIdentity;
  p.policy.maximumAttempts = 3;
  require(validatePlan(p, a, b, {}, diagnostics()) == Rejection::InvalidPlan, "unbounded retry plan accepted");
}
static void counterBounds()
{
  require(parseCounter("18446744073709551615") == std::numeric_limits<uint64_t>::max(), "max counter lost precision");
  for (const auto &bad : {"", "0", "01", "+1", "-1", "1 ", "1.0", "18446744073709551616"})
    require(!parseCounter(bad), "invalid counter accepted");
}
int main(int argc, char **argv)
{
  const std::vector<std::pair<std::string, std::function<void()>>> cases{
      {"DirectionMatrix", directionMatrix},
      {"AsymmetricProtocols", asymmetricProtocols},
      {"SafetyGates", safetyGates},
      {"NoProtocolConversion", noProtocolConversion},
      {"CertificationInvalidation", certificationAndInvalidation},
      {"InvalidDeclarations", invalidDeclarations},
      {"FeedbackAndConditional", feedbackAndConditional},
      {"PlanBinding", planBinding},
      {"CounterBounds", counterBounds}
  };
  try {
    if (argc != 2)
      throw std::runtime_error("one test case required");
    for (const auto &[name, run] : cases)
      if (name == argv[1]) {
        run();
        return 0;
      }
    throw std::runtime_error("unknown test case");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
