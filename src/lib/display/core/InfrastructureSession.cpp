// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "InfrastructureSession.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace deskflow::display::infra {
void InfrastructureSession::changed() {
  if (m_snapshot.revision == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("infrastructure revision exhausted");
  ++m_snapshot.revision;
}
void InfrastructureSession::nextGeneration() {
  if (m_snapshot.token.generation == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("infrastructure generation exhausted");
  ++m_snapshot.token.generation;
}
void InfrastructureSession::effect(EffectKind kind) {
  // State transitions and recovery bounds cap outstanding effects. Exhaustion is
  // an internal contract violation, never a reason to silently discard Stop.
  if (m_effects.size() >= 32) throw std::logic_error("infrastructure effect budget exhausted");
  m_effects.push_back({kind, m_snapshot.token});
}
bool InfrastructureSession::matches(const SessionToken& t) const {
  return m_snapshot.state != Lifecycle::Idle && t == m_snapshot.token;
}
bool InfrastructureSession::expired(TimePoint now) const {
  return now >= m_deadline || (m_barriers.authorized && now >= m_barriers.authorizationExpires);
}
std::vector<SessionEffect> InfrastructureSession::takeEffects() {
  auto result = std::move(m_effects); m_effects.clear(); return result;
}
bool InfrastructureSession::reserve(SessionScope scope, TimePoint now) {
  if (m_snapshot.state != Lifecycle::Idle || !m_effects.empty() ||
      !validId(scope.localIdentity, 32) || !validId(scope.peerIdentity, 32) ||
      scope.localIdentity == scope.peerIdentity || !validId(scope.controlBinding, 32) ||
      !validId(scope.session, 16) || !validId(scope.resourceOwner, 16) || !scope.attempt ||
      (scope.localDirection != Direction::Send && scope.localDirection != Direction::Receive)) return false;
  for (const auto& [session, owner] : m_recentOwners)
    if (session == scope.session || owner == scope.resourceOwner) return false;
  m_recentOwners.emplace_back(scope.session, scope.resourceOwner);
  if (m_recentOwners.size() > 256) m_recentOwners.pop_front();
  // The durable owner registry additionally rejects replay across restarts.
  nextGeneration();
  const auto generation = m_snapshot.token.generation, revision = m_snapshot.revision;
  m_snapshot = {};
  m_snapshot.revision = revision;
  m_snapshot.token = {scope.session, scope.attempt, generation, 1, scope.resourceOwner, {}};
  m_snapshot.state = Lifecycle::Reserved;
  m_scope = std::move(scope); m_policy = {}; m_barriers = {};
  m_workerQuiesced = true; m_destroyRequested = false; m_recoveryAttempts = 0;
  m_intentPending = m_preparePending = m_verificationRequested = false;
  m_reservationDeadline = now + std::chrono::seconds(30);
  m_deadline = m_reservationDeadline; m_startupDeadline = {};
  changed(); return true;
}
bool InfrastructureSession::beginNegotiation(const SessionToken& t, TimePoint now) {
  if (!matches(t) || m_snapshot.state != Lifecycle::Reserved || expired(now)) return false;
  m_snapshot.state = Lifecycle::Negotiating;
  m_deadline = std::min(m_deadline, now + std::chrono::seconds(3));
  changed(); return true;
}
Rejection InfrastructureSession::acceptPlan(const SessionToken& t, const SessionPlan& p,
  const std::string& hash, const CapabilitySnapshot& sender, const CapabilitySnapshot& receiver,
  const Requirements& requirements, const SelectionPolicy& selection, TimePoint now) {
  if (!matches(t) || m_snapshot.state != Lifecycle::Negotiating || expired(now) || !validId(hash, 32))
    return Rejection::InvalidPlan;
  if (p.session != m_scope.session || p.controlBinding != m_scope.controlBinding || p.attempt != m_scope.attempt)
    return Rejection::InvalidPlan;
  const bool localSender = m_scope.localDirection == Direction::Send;
  if ((localSender ? p.senderIdentity : p.receiverIdentity) != m_scope.localIdentity ||
      (localSender ? p.receiverIdentity : p.senderIdentity) != m_scope.peerIdentity)
    return Rejection::IdentityMismatch;
  const auto result = validatePlan(p, sender, receiver, requirements, selection);
  if (result != Rejection::None) return result;
  m_snapshot.planHash = hash; m_policy = p.policy;
  m_snapshot.state = Lifecycle::AwaitingBarriers;
  // Local authorization may take longer than plan negotiation, but reservation
  // expiry is never reset by a duplicate message.
  m_deadline = m_reservationDeadline;
  changed(); return Rejection::None;
}
bool InfrastructureSession::commit(const SessionToken& t, Barriers barriers, TimePoint now) {
  if (!matches(t) || m_snapshot.state != Lifecycle::AwaitingBarriers || expired(now) ||
      !barriers.authorized || !barriers.permissionsReady || !barriers.inputsReleased ||
      !barriers.routeIsolated || !barriers.peerCommitted || barriers.authorizationExpires <= now) return false;
  m_barriers = barriers;
  m_snapshot.state = Lifecycle::Preparing;
  m_snapshot.cleanupVerified = false;
  m_startupDeadline = now + m_policy.startup; m_deadline = m_startupDeadline;
  m_intentPending = true; m_intentToken = m_snapshot.token;
  effect(EffectKind::PersistIntent);
  changed(); return true;
}
bool InfrastructureSession::intentPersisted(const SessionToken& t, bool success, TimePoint now) {
  if (!m_intentPending || t != m_intentToken) return false;
  m_intentPending = false;
  if (success) m_snapshot.intentDurable = true;
  if (m_snapshot.state == Lifecycle::Stopping || m_snapshot.state == Lifecycle::CleanupRequired) {
    beginCleanup(now); changed(); return true;
  }
  if (!matches(t) || m_snapshot.state != Lifecycle::Preparing) return false;
  if (!success || expired(now)) { stop(success ? "AttemptExpired" : "DurabilityFailed", now); return true; }
  m_preparePending = true; m_prepareToken = m_snapshot.token;
  effect(EffectKind::PrepareBackend);
  changed(); return true;
}
bool InfrastructureSession::attachWorker(const SessionToken& t, const std::string& instance, TimePoint now) {
  if (!m_preparePending || t != m_prepareToken || !validId(instance, 16) ||
      !m_snapshot.token.worker.empty() || !m_snapshot.intentDurable) return false;
  m_preparePending = false;
  m_snapshot.token.worker = instance; m_workerQuiesced = false;
  // A late spawn completion cannot activate media, but its live instance must
  // remain registered for cleanup after cancellation.
  if (m_snapshot.state == Lifecycle::Stopping || m_snapshot.state == Lifecycle::CleanupRequired) {
    effect(EffectKind::StopMedia); changed(); return true;
  }
  if (expired(now)) stop("AttemptExpired", now); else changed();
  return true;
}
bool InfrastructureSession::preparationFailed(const SessionToken& t, TimePoint now) {
  if (!m_preparePending || t != m_prepareToken) return false;
  m_preparePending = false;
  if (m_snapshot.state == Lifecycle::Stopping || m_snapshot.state == Lifecycle::CleanupRequired) {
    beginCleanup(now); changed(); return true;
  }
  stop("BackendPreparationFailed", now); return true;
}
bool InfrastructureSession::prepared(const SessionToken& t, TimePoint now) {
  if (!matches(t) || m_snapshot.state != Lifecycle::Preparing || !m_snapshot.intentDurable ||
      m_snapshot.token.worker.empty() || expired(now)) return false;
  m_snapshot.state = Lifecycle::AwaitingFirstFrame;
  effect(EffectKind::StartMedia);
  changed(); return true;
}
bool InfrastructureSession::presented(const SessionToken& t, PresentationEvidence evidence,
                                     bool exposed, TimePoint now) {
  if (!matches(t) || m_snapshot.state != Lifecycle::AwaitingFirstFrame || expired(now) || !exposed ||
      (evidence != PresentationEvidence::NativePaintConfirmed &&
       evidence != PresentationEvidence::CompositorPresented)) return false;
  m_snapshot.firstFramePresented = true;
  m_snapshot.state = Lifecycle::Streaming;
  m_deadline = TimePoint::max();
  changed(); return true;
}
bool InfrastructureSession::beginRecovery(const SessionToken& t, TimePoint now) {
  if (!matches(t) || m_snapshot.state != Lifecycle::Streaming || expired(now) || m_recoveryAttempts >= 2) return false;
  ++m_recoveryAttempts;
  m_snapshot.state = Lifecycle::Recovering;
  m_deadline = std::min(now + std::chrono::seconds(3), m_barriers.authorizationExpires);
  effect(EffectKind::RequestRefresh);
  changed(); return true;
}
bool InfrastructureSession::recovered(const SessionToken& t, TimePoint now) {
  if (!matches(t) || m_snapshot.state != Lifecycle::Recovering || expired(now)) return false;
  m_snapshot.state = Lifecycle::Streaming; m_deadline = TimePoint::max();
  m_recoveryAttempts = 0;
  changed(); return true;
}
bool InfrastructureSession::retryRefresh(const SessionToken& t, TimePoint now) {
  if (!matches(t) || m_snapshot.state != Lifecycle::Recovering || expired(now) || m_recoveryAttempts >= 2)
    return false;
  ++m_recoveryAttempts; effect(EffectKind::RequestRefresh); return true;
}
bool InfrastructureSession::stop(const std::string& reason, TimePoint now) {
  if (m_snapshot.state == Lifecycle::Idle || !validName(reason)) return false;
  if (m_snapshot.state == Lifecycle::Stopping || m_snapshot.state == Lifecycle::VerifyingCleanup ||
      m_snapshot.state == Lifecycle::CleanupRequired) return true;
  if (m_snapshot.primaryError.empty()) m_snapshot.primaryError = reason;
  nextGeneration();
  m_barriers.authorized = false;
  m_snapshot.firstFramePresented = false;
  if (m_snapshot.cleanupVerified) {
    // No PersistIntent was ever dispatched, so no platform side effect exists.
    m_snapshot.state = Lifecycle::Idle; changed(); return true;
  }
  m_snapshot.state = Lifecycle::Stopping;
  m_deadline = now + m_policy.normalStop;
  m_cleanupAbsoluteDeadline = now + m_policy.abnormalCleanup;
  // Effects not yet dispatched can be cancelled without an in-flight write or
  // process spawn. Dispatched operations must report completion before cleanup.
  std::erase_if(m_effects, [&](const SessionEffect& e) {
    if (e.kind == EffectKind::PersistIntent && e.token == m_intentToken) { m_intentPending = false; return true; }
    if (e.kind == EffectKind::PrepareBackend && e.token == m_prepareToken) { m_preparePending = false; return true; }
    return e.kind == EffectKind::StartMedia || e.kind == EffectKind::RequestRefresh;
  });
  if (m_preparePending) effect(EffectKind::CancelPreparation);
  if (m_workerQuiesced) beginCleanup(now);
  else effect(EffectKind::StopMedia);
  changed(); return true;
}
void InfrastructureSession::beginCleanup(TimePoint now) {
  if (!m_workerQuiesced || m_intentPending || m_preparePending || m_destroyRequested) return;
  m_destroyRequested = true;
  if (m_snapshot.state == Lifecycle::CleanupRequired)
    m_deadline = std::min(now + m_policy.normalStop, m_cleanupAbsoluteDeadline);
  m_snapshot.state = Lifecycle::VerifyingCleanup;
  effect(EffectKind::DestroyResources);
}
bool InfrastructureSession::workerQuiesced(const std::string& owner, const std::string& worker, TimePoint now) {
  if (owner != m_snapshot.token.owner || worker != m_snapshot.token.worker || m_workerQuiesced ||
      (m_snapshot.state != Lifecycle::Stopping && m_snapshot.state != Lifecycle::VerifyingCleanup &&
       m_snapshot.state != Lifecycle::CleanupRequired)) return false;
  m_workerQuiesced = true;
  beginCleanup(now); changed(); return true;
}
bool InfrastructureSession::resourcesDestroyed(const std::string& owner, TimePoint) {
  if (owner != m_snapshot.token.owner || m_snapshot.state != Lifecycle::VerifyingCleanup ||
      !m_workerQuiesced || !m_destroyRequested || m_verificationRequested) return false;
  m_verificationRequested = true;
  effect(EffectKind::VerifyResources);
  return true;
}
bool InfrastructureSession::cleanupEvidence(const std::string& owner, bool verified, TimePoint) {
  if (owner != m_snapshot.token.owner || m_snapshot.state != Lifecycle::VerifyingCleanup || !m_workerQuiesced ||
      !m_verificationRequested || m_intentPending || m_preparePending)
    return false;
  if (verified) {
    m_snapshot.cleanupVerified = true; m_snapshot.cleanupError.clear();
    m_snapshot.state = Lifecycle::Idle;
  } else {
    m_snapshot.cleanupError = "CleanupVerificationUnavailable";
    m_snapshot.state = Lifecycle::CleanupRequired;
  }
  changed(); return true;
}
bool InfrastructureSession::retryCleanup(TimePoint now) {
  if (m_snapshot.state != Lifecycle::CleanupRequired || !m_effects.empty()) return false;
  nextGeneration(); m_destroyRequested = false; m_verificationRequested = false;
  m_deadline = now + m_policy.normalStop; m_cleanupAbsoluteDeadline = now + m_policy.abnormalCleanup;
  m_snapshot.state = Lifecycle::Stopping;
  if (m_preparePending) effect(EffectKind::CancelPreparation);
  if (m_workerQuiesced) beginCleanup(now); else effect(EffectKind::StopMedia);
  changed(); return true;
}
void InfrastructureSession::tick(TimePoint now) {
  const auto state = m_snapshot.state;
  if (state == Lifecycle::Idle || state == Lifecycle::CleanupRequired) return;
  if (!expired(now)) return;
  if (state == Lifecycle::Stopping || state == Lifecycle::VerifyingCleanup) {
    if (!m_workerQuiesced) effect(EffectKind::TerminateWorker);
    if (m_preparePending) effect(EffectKind::CancelPreparation);
    m_snapshot.state = Lifecycle::CleanupRequired;
    m_snapshot.cleanupError = "CleanupDeadlineExceeded";
    changed(); return;
  }
  stop(m_barriers.authorized && now >= m_barriers.authorizationExpires ? "AuthorizationExpired" :
       state == Lifecycle::Recovering ? "CongestionUnrecoverable" : "AttemptExpired", now);
}
} // namespace deskflow::display::infra
