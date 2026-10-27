// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "InfrastructureTypes.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <tuple>

namespace deskflow::display::infra {
bool validId(const std::string& text, size_t bytes) {
  return text.size() == bytes * 2 && text.find_first_not_of('0') != std::string::npos &&
    std::all_of(text.begin(), text.end(), [](unsigned char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool validName(const std::string& text, size_t maximum) {
  return !text.empty() && text.size() <= maximum &&
    ((text.front() >= 'a' && text.front() <= 'z') || (text.front() >= 'A' && text.front() <= 'Z') ||
     (text.front() >= '0' && text.front() <= '9')) &&
    std::all_of(text.begin(), text.end(), [](unsigned char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}
std::optional<uint64_t> parseCounter(const std::string& text) {
  if (text.empty() || text.size() > 20 || text.front() == '0') return {};
  uint64_t result = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size() || result == 0) return {};
  return result;
}
const char* protocolName(WireProtocol value) {
  switch (value) { case WireProtocol::Rdp: return "rdp"; case WireProtocol::WebRtc: return "webrtc"; }
  return "invalid";
}
const char* rejectionName(Rejection value) {
  switch (value) {
#define CASE(x) case Rejection::x: return #x
    CASE(None); CASE(InvalidCapabilities); CASE(WrongDirection); CASE(NetworkForbidden);
    CASE(Busy); CASE(CleanupUnverified); CASE(NotReady); CASE(StaleCapabilities);
    CASE(NoCompatiblePipeline); CASE(NoCommonProtocol); CASE(NoCommonFormat); CASE(SecurityMismatch); CASE(UnsupportedFeature);
    CASE(UncertifiedPipeline); CASE(IdentityMismatch); CASE(VersionMismatch); CASE(InvalidPlan);
    CASE(ResourceBudgetExceeded);
#undef CASE
  }
  return "InvalidRejection";
}
bool validFormat(const VideoFormat& f, WireProtocol protocol) {
  if (f.width != 1920 || f.height != 1080 || f.fpsNumerator != 30 || f.fpsDenominator != 1)
    return false;
  if (f.color != ColorProfile::SdrBt709Full8 && f.color != ColorProfile::SdrBt709Limited8)
    return false;
  if (f.cursor != CursorMode::Embedded && f.cursor != CursorMode::Metadata) return false;
  return (protocol == WireProtocol::WebRtc && f.graphics == GraphicsMode::H264ConstrainedBaseline) ||
    (protocol == WireProtocol::Rdp && (f.graphics == GraphicsMode::RdpAvc420 || f.graphics == GraphicsMode::RdpAvc444));
}
namespace {
bool secureFor(SecurityProfile security, WireProtocol protocol) {
  return (protocol == WireProtocol::Rdp && security == SecurityProfile::RdpAuthenticatedTls13) ||
    (protocol == WireProtocol::WebRtc && security == SecurityProfile::WebRtcAuthenticatedDtlsSrtp);
}
bool usable(Readiness state) {
  return state == Readiness::Available || state == Readiness::Conditional;
}
bool matches(const Requirements& r, const VideoFormat& f) {
  return f.width == r.width && f.height == r.height && f.fpsNumerator == r.fpsNumerator &&
    f.fpsDenominator == r.fpsDenominator && f.color == r.color && f.cursor == r.cursor;
}
bool certified(const Certification& c, const Candidate& p) {
  return c.senderPipeline == p.sender.id && c.receiverPipeline == p.receiver.id &&
    c.senderImplementationVersion == p.sender.implementationVersion &&
    c.receiverImplementationVersion == p.receiver.implementationVersion &&
    c.senderEnvironment == p.sender.environment && c.receiverEnvironment == p.receiver.environment &&
    c.format == p.format && c.security == p.security;
}
}
bool validCapabilities(const CapabilitySnapshot& value) {
  if (!validId(value.peerIdentity, 32) || value.revision == 0 || !validId(value.environment, 32) ||
      value.pipelines.size() > 16) return false;
  std::set<std::pair<Direction, std::string>> unique;
  for (const auto& p : value.pipelines) {
    if (!validName(p.id) || !validName(p.implementation) || !validName(p.implementationVersion, 64) ||
        p.environment != value.environment || p.adapterVersion != 1 || p.formats.empty() ||
        p.formats.size() > 8 || p.security.empty() || p.security.size() > 2 ||
        (p.direction != Direction::Send && p.direction != Direction::Receive) ||
        !unique.emplace(p.direction, p.id).second) return false;
    if (p.readiness != Readiness::Unknown && p.readiness != Readiness::Checking &&
        p.readiness != Readiness::Available && p.readiness != Readiness::Conditional &&
        p.readiness != Readiness::Unavailable) return false;
    if (p.memory != MemoryDomain::Cpu && p.memory != MemoryDomain::D3D11 &&
        p.memory != MemoryDomain::IOSurface && p.memory != MemoryDomain::DmaBuf &&
        p.memory != MemoryDomain::BackendNative) return false;
    for (size_t i = 0; i < p.formats.size(); ++i) {
      if (!validFormat(p.formats[i], p.protocol)) return false;
      if (std::find(p.formats.begin(), p.formats.begin() + static_cast<std::ptrdiff_t>(i), p.formats[i]) !=
          p.formats.begin() + static_cast<std::ptrdiff_t>(i)) return false;
    }
    std::set<SecurityProfile> security;
    for (auto s : p.security) if (!secureFor(s, p.protocol) || !security.insert(s).second) return false;
  }
  return true;
}
Selection plan(const CapabilitySnapshot& sender, const CapabilitySnapshot& receiver,
               const Requirements& r, const SelectionPolicy& policy) {
  Selection out;
  auto fail = [&](Rejection reason) { out.reason = reason; return out; };
  if (!validCapabilities(sender) || !validCapabilities(receiver)) return fail(Rejection::InvalidCapabilities);
  if (sender.peerIdentity == receiver.peerIdentity) return fail(Rejection::IdentityMismatch);
  if (!sender.directNetworkAllowed || !receiver.directNetworkAllowed) return fail(Rejection::NetworkForbidden);
  if (!sender.cleanupVerified || !receiver.cleanupVerified) return fail(Rejection::CleanupUnverified);
  if (!sender.sessionAvailable || !receiver.sessionAvailable) return fail(Rejection::Busy);
  if (r.allowAudio || r.allowRemoteInput || r.allowRedirection) return fail(Rejection::UnsupportedFeature);
  if (policy.preference.empty() || policy.preference.size() > 2 || policy.certified.size() > 256)
    return fail(Rejection::InvalidPlan);
  std::set<WireProtocol> protocols;
  for (auto p : policy.preference) {
    if ((p != WireProtocol::Rdp && p != WireProtocol::WebRtc) || !protocols.insert(p).second)
      return fail(Rejection::InvalidPlan);
  }
  bool hasSender = false, hasReceiver = false;
  for (const auto& a : sender.pipelines) {
    if (a.direction != Direction::Send) continue;
    hasSender = true;
    for (const auto& b : receiver.pipelines) {
      if (b.direction != Direction::Receive) continue;
      hasReceiver = true;
      Rejection reason = Rejection::None;
      if (!usable(a.readiness) || !usable(b.readiness)) reason = Rejection::NotReady;
      else if (a.protocol != b.protocol || !protocols.contains(a.protocol)) reason = Rejection::NoCommonProtocol;
      else if (!a.verifiedCleanup || !b.verifiedCleanup || !b.presentationEvidence ||
               !a.refreshFeedback || !b.refreshFeedback ||
               (r.requireRateFeedback && !a.rateFeedback) ||
               (r.requireHardware && (!a.hardware || !b.hardware))) reason = Rejection::UnsupportedFeature;
      if (reason != Rejection::None) { out.rejected.push_back({a.id, b.id, reason}); continue; }
      bool anyFormat = false, anySecure = false, anyCandidate = false;
      for (const auto& format : a.formats) {
        if (!matches(r, format) || std::find(b.formats.begin(), b.formats.end(), format) == b.formats.end()) continue;
        anyFormat = true;
        for (auto security : a.security) {
          if (std::find(b.security.begin(), b.security.end(), security) == b.security.end()) continue;
          anySecure = true;
          Candidate candidate{a, b, format, security};
          candidate.requiresConditionalProbe = a.readiness == Readiness::Conditional || b.readiness == Readiness::Conditional;
          for (const auto& c : policy.certified) {
            if (certified(c, candidate)) {
              if (!candidate.certified || c.priority > candidate.certificationPriority)
                candidate.certificationPriority = c.priority;
              candidate.certified = true;
            }
          }
          if (!candidate.certified && !policy.allowUncertifiedDiagnostics) continue;
          anyCandidate = true;
          out.candidates.push_back(std::move(candidate));
        }
      }
      if (!anyCandidate) out.rejected.push_back({a.id, b.id, !anyFormat ? Rejection::NoCommonFormat :
        !anySecure ? Rejection::SecurityMismatch : Rejection::UncertifiedPipeline});
    }
  }
  if (!hasSender || !hasReceiver) return fail(Rejection::WrongDirection);
  auto protocolRank = [&](WireProtocol p) { return std::find(policy.preference.begin(), policy.preference.end(), p) - policy.preference.begin(); };
  std::sort(out.candidates.begin(), out.candidates.end(), [&](const Candidate& a, const Candidate& b) {
    if (a.certified != b.certified) return a.certified;
    if (a.certificationPriority != b.certificationPriority) return a.certificationPriority > b.certificationPriority;
    if (a.requiresConditionalProbe != b.requiresConditionalProbe) return !a.requiresConditionalProbe;
    const auto hardwareA = unsigned(a.sender.hardware) + unsigned(a.receiver.hardware);
    const auto hardwareB = unsigned(b.sender.hardware) + unsigned(b.receiver.hardware);
    if (hardwareA != hardwareB) return hardwareA > hardwareB;
    if (a.sender.protocol != b.sender.protocol) return protocolRank(a.sender.protocol) < protocolRank(b.sender.protocol);
    return std::tie(a.sender.id, a.receiver.id, a.format.graphics, a.security) <
           std::tie(b.sender.id, b.receiver.id, b.format.graphics, b.security);
  });
  // Keep the bounded explanatory list, but only two executable attempts.
  if (out.candidates.size() > 2) out.candidates.resize(2);
  if (out.candidates.empty()) {
    out.reason = out.rejected.empty() ? Rejection::NoCompatiblePipeline : out.rejected.front().reason;
    for (const auto& rejected : out.rejected)
      if (rejected.reason != out.reason) { out.reason = Rejection::NoCompatiblePipeline; break; }
  }
  return out;
}
Rejection validatePlan(const SessionPlan& p, const CapabilitySnapshot& sender,
                       const CapabilitySnapshot& receiver, const Requirements& requirements,
                       const SelectionPolicy& policy) {
  if (!validId(p.controlBinding, 32) || !validId(p.session, 16) || !p.attempt ||
      !validId(p.authorizationScopeDigest, 32) || p.policy.maximumAttempts < 1 ||
      p.policy.maximumAttempts > 2 || p.policy.startup.count() <= 0 || p.policy.startup.count() > 10000 ||
      p.policy.normalStop.count() <= 0 || p.policy.normalStop.count() > 5000 ||
      p.policy.abnormalCleanup.count() <= 0 || p.policy.abnormalCleanup.count() > 15000)
    return Rejection::InvalidPlan;
  if (p.senderIdentity != sender.peerIdentity || p.receiverIdentity != receiver.peerIdentity)
    return Rejection::IdentityMismatch;
  if (p.senderRevision != sender.revision || p.receiverRevision != receiver.revision)
    return Rejection::StaleCapabilities;
  const auto selected = plan(sender, receiver, requirements, policy);
  if (selected.reason != Rejection::None) return selected.reason;
  if (std::find(selected.candidates.begin(), selected.candidates.end(), p.candidate) == selected.candidates.end())
    return Rejection::InvalidPlan;
  if (p.candidate.requiresConditionalProbe) return Rejection::NotReady;
  return Rejection::None;
}
} // namespace deskflow::display::infra
