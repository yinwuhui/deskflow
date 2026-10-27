// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "InfrastructureTypes.h"
#include <deque>

namespace deskflow::display::infra {
enum class Lifecycle
{
  Idle,
  Reserved,
  Negotiating,
  AwaitingBarriers,
  Preparing,
  AwaitingFirstFrame,
  Streaming,
  Recovering,
  Stopping,
  VerifyingCleanup,
  CleanupRequired
};
enum class EffectKind
{
  PersistIntent,
  PrepareBackend,
  StartMedia,
  RequestRefresh,
  StopMedia,
  CancelPreparation,
  TerminateWorker,
  DestroyResources,
  VerifyResources
};
enum class PresentationEvidence
{
  Decoded,
  Submitted,
  NativePaintConfirmed,
  CompositorPresented
};
struct SessionToken
{
  std::string session;
  uint64_t attempt = 0;
  uint64_t generation = 0;
  uint32_t epoch = 1;
  std::string owner;
  std::string worker;
  bool operator==(const SessionToken &) const = default;
};
struct SessionScope
{
  std::string localIdentity;
  std::string peerIdentity;
  std::string controlBinding;
  std::string session;
  std::string resourceOwner;
  Direction localDirection = Direction::Send;
  uint64_t attempt = 1;
};
struct Barriers
{
  bool authorized = false;
  bool permissionsReady = false;
  bool inputsReleased = false;
  bool routeIsolated = false;
  bool peerCommitted = false;
  TimePoint authorizationExpires{};
};
struct SessionEffect
{
  EffectKind kind;
  SessionToken token;
};
struct SessionSnapshot
{
  Lifecycle state = Lifecycle::Idle;
  SessionToken token;
  uint64_t revision = 0;
  std::string planHash;
  std::string primaryError;
  std::string cleanupError;
  bool intentDurable = false;
  bool firstFramePresented = false;
  bool cleanupVerified = true;
};
// One owner executor, no I/O, sleeps, platform operations or callbacks.
// Cleanup uses owner+worker identity, independently of cancellation generation.
class InfrastructureSession
{
public:
  const SessionSnapshot &snapshot() const
  {
    return m_snapshot;
  }
  bool reserve(SessionScope, TimePoint now);
  bool beginNegotiation(const SessionToken &, TimePoint now);
  Rejection acceptPlan(
      const SessionToken &, const SessionPlan &, const std::string &hash, const CapabilitySnapshot &sender,
      const CapabilitySnapshot &receiver, const Requirements &, const SelectionPolicy &, TimePoint now
  );
  bool commit(const SessionToken &, Barriers, TimePoint now);
  bool intentPersisted(const SessionToken &, bool success, TimePoint now);
  bool attachWorker(const SessionToken &, const std::string &workerInstance, TimePoint now);
  bool preparationFailed(const SessionToken &, TimePoint now);
  bool prepared(const SessionToken &, TimePoint now);
  bool presented(const SessionToken &, PresentationEvidence, bool exposed, TimePoint now);
  bool beginRecovery(const SessionToken &, TimePoint now);
  bool recovered(const SessionToken &, TimePoint now);
  bool retryRefresh(const SessionToken &, TimePoint now);
  bool stop(const std::string &reason, TimePoint now);
  bool workerQuiesced(const std::string &owner, const std::string &worker, TimePoint now);
  bool resourcesDestroyed(const std::string &owner, TimePoint now);
  bool cleanupEvidence(const std::string &owner, bool verified, TimePoint now);
  bool retryCleanup(TimePoint now);
  void tick(TimePoint now);
  std::vector<SessionEffect> takeEffects();

private:
  bool matches(const SessionToken &) const;
  bool expired(TimePoint now) const;
  void changed();
  void effect(EffectKind);
  void nextGeneration();
  void beginCleanup(TimePoint now);
  SessionSnapshot m_snapshot;
  SessionScope m_scope;
  SessionPolicy m_policy;
  Barriers m_barriers;
  TimePoint m_deadline{};
  TimePoint m_startupDeadline{};
  TimePoint m_reservationDeadline{};
  TimePoint m_cleanupAbsoluteDeadline{};
  SessionToken m_intentToken, m_prepareToken;
  bool m_intentPending = false;
  bool m_preparePending = false;
  bool m_verificationRequested = false;
  bool m_workerQuiesced = true;
  bool m_destroyRequested = false;
  uint32_t m_recoveryAttempts = 0;
  std::vector<SessionEffect> m_effects;
  std::deque<std::pair<std::string, std::string>> m_recentOwners;
};
} // namespace deskflow::display::infra
