// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace deskflow::display::infra {
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

enum class Direction { Send, Receive };
enum class WireProtocol { Rdp, WebRtc };
enum class Readiness { Unknown, Checking, Available, Conditional, Unavailable };
enum class GraphicsMode { H264ConstrainedBaseline, RdpAvc420, RdpAvc444 };
enum class SecurityProfile { RdpAuthenticatedTls13, WebRtcAuthenticatedDtlsSrtp };
enum class ColorProfile { SdrBt709Limited8, SdrBt709Full8 };
enum class CursorMode { Embedded, Metadata };
enum class MemoryDomain { Cpu, D3D11, IOSurface, DmaBuf, BackendNative };
enum class Rejection {
  None, InvalidCapabilities, WrongDirection, NetworkForbidden, Busy, CleanupUnverified,
  NotReady, StaleCapabilities, NoCompatiblePipeline, NoCommonProtocol, NoCommonFormat, SecurityMismatch,
  UnsupportedFeature, UncertifiedPipeline, IdentityMismatch, VersionMismatch,
  InvalidPlan, ResourceBudgetExceeded
};

bool validId(const std::string& text, size_t bytes);
bool validName(const std::string& text, size_t maximum = 128);
std::optional<uint64_t> parseCounter(const std::string& text);
const char* protocolName(WireProtocol value);
const char* rejectionName(Rejection value);

struct VideoFormat {
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t fpsNumerator = 30;
  uint32_t fpsDenominator = 1;
  GraphicsMode graphics = GraphicsMode::H264ConstrainedBaseline;
  ColorProfile color = ColorProfile::SdrBt709Limited8;
  CursorMode cursor = CursorMode::Embedded;
  bool operator==(const VideoFormat&) const = default;
};

struct PipelineProfile {
  std::string id;
  std::string implementation;
  std::string implementationVersion;
  std::string environment;
  Direction direction = Direction::Send;
  WireProtocol protocol = WireProtocol::WebRtc;
  Readiness readiness = Readiness::Unknown;
  uint32_t adapterVersion = 1;
  std::vector<SecurityProfile> security;
  std::vector<VideoFormat> formats;
  MemoryDomain memory = MemoryDomain::Cpu;
  bool hardware = false;
  bool rateFeedback = false;
  bool refreshFeedback = false;
  bool presentationEvidence = false;
  bool verifiedCleanup = false;
  bool resourceCoupledToWorker = true;
  bool operator==(const PipelineProfile&) const = default;
};

struct CapabilitySnapshot {
  std::string peerIdentity;
  uint64_t revision = 0;
  std::string environment;
  bool directNetworkAllowed = false;
  bool sessionAvailable = false;
  bool cleanupVerified = false;
  std::vector<PipelineProfile> pipelines;
  bool operator==(const CapabilitySnapshot&) const = default;
};

// Not supplied by the peer. Records come from the local release/diagnostic policy.
struct Certification {
  std::string senderPipeline;
  std::string receiverPipeline;
  std::string senderImplementationVersion;
  std::string receiverImplementationVersion;
  std::string senderEnvironment;
  std::string receiverEnvironment;
  VideoFormat format;
  SecurityProfile security;
  int priority = 0;
  bool operator==(const Certification&) const = default;
};

struct Requirements {
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t fpsNumerator = 30;
  uint32_t fpsDenominator = 1;
  ColorProfile color = ColorProfile::SdrBt709Limited8;
  CursorMode cursor = CursorMode::Embedded;
  bool requireHardware = false;
  bool requireRateFeedback = true;
  bool allowRemoteInput = false;
  bool allowAudio = false;
  bool allowRedirection = false;
};

struct SelectionPolicy {
  std::vector<WireProtocol> preference{WireProtocol::WebRtc, WireProtocol::Rdp};
  std::vector<Certification> certified;
  // Local development policy only; must never be decoded from a peer request.
  bool allowUncertifiedDiagnostics = false;
};

struct Candidate {
  PipelineProfile sender;
  PipelineProfile receiver;
  VideoFormat format;
  SecurityProfile security;
  bool certified = false;
  int certificationPriority = 0;
  bool requiresConditionalProbe = false;
  bool operator==(const Candidate&) const = default;
};
struct CandidateRejection {
  std::string sender;
  std::string receiver;
  Rejection reason;
};
struct Selection {
  Rejection reason = Rejection::None;
  std::vector<Candidate> candidates;
  std::vector<CandidateRejection> rejected;
};

bool validCapabilities(const CapabilitySnapshot& value);
bool validFormat(const VideoFormat& value, WireProtocol protocol);
Selection plan(const CapabilitySnapshot& sender, const CapabilitySnapshot& receiver,
               const Requirements& requirements, const SelectionPolicy& policy);

struct SessionPolicy {
  std::chrono::milliseconds startup{10000};
  std::chrono::milliseconds normalStop{5000};
  std::chrono::milliseconds abnormalCleanup{15000};
  uint32_t maximumAttempts = 2;
  bool operator==(const SessionPolicy&) const = default;
};
struct SessionPlan {
  std::string controlBinding;
  std::string session;
  uint64_t attempt = 0;
  std::string senderIdentity;
  std::string receiverIdentity;
  uint64_t senderRevision = 0;
  uint64_t receiverRevision = 0;
  Candidate candidate;
  std::string authorizationScopeDigest;
  SessionPolicy policy;
  bool operator==(const SessionPlan&) const = default;
};
Rejection validatePlan(const SessionPlan& value, const CapabilitySnapshot& sender,
                       const CapabilitySnapshot& receiver, const Requirements& requirements,
                       const SelectionPolicy& policy);
} // namespace deskflow::display::infra
