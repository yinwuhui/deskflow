// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "ResourceExecutor.h"
#include <QScopedValueRollback>
#include <QSet>
#include <algorithm>
namespace deskflow::display::infra {
namespace {
template <class F, class T> T safeProvider(F &&f, T fallback)
{
  try {
    return f();
  } catch (...) {
    return fallback;
  }
}
bool ownership(const ResourceEvidence &e)
{
  return e.status == "verified" && !e.reference.isEmpty() && e.reference.size() <= 1024 &&
         !e.reference.contains(QChar(0)) &&
         (e.kind == "nativeObjectOwnership" || e.kind == "driverOwnerBinding" || e.kind == "verifiedHelperRegistry");
}
bool absence(const ResourceEvidence &e)
{
  return e.kind == "systemAbsenceCheck" && e.status == "verified" && !e.reference.isEmpty() &&
         e.reference.size() <= 1024 && !e.reference.contains(QChar(0));
}
bool reference(const QString &r)
{
  return !r.isEmpty() && r.size() <= 512 && !r.contains(QChar(0));
}
ResourceEvidence unknown()
{
  return {"unproven", "resource-executor", "unknown"};
}
} // namespace
ResourceExecutor::ResourceExecutor(
    DurableResourceLedger &ledger, QString user, QString boot, std::function<bool(const QString &)> quiesced
)
    : m_ledger(ledger),
      m_userSession(std::move(user)),
      m_bootInstance(std::move(boot)),
      m_thread(QThread::currentThreadId()),
      m_quiesced(std::move(quiesced))
{
}
bool ResourceExecutor::ownerThread() const
{
  return m_thread == QThread::currentThreadId();
}
bool ResourceExecutor::registerProvider(ProviderRegistration p)
{
  if (!ownerThread() || m_busy || m_providers.size() >= 16 || m_providers.contains(p.id) ||
      !validName(p.id.toStdString()) || !validName(p.version.toStdString()) ||
      !validName(p.verificationPolicy.toStdString()) || p.verificationPolicy == "legacy-unproven" ||
      !p.implementation || p.kinds.isEmpty() || p.kinds.size() > 8 || p.compatibleRecoveryVersions.size() > 8)
    return false;
  const QSet<QString> allowed{"virtualOutput", "capture", "encoder", "decoder", "presenter", "sharedMemory", "worker"};
  QSet<QString> kinds, versions;
  for (const auto &k : p.kinds) {
    if (!allowed.contains(k) || kinds.contains(k))
      return false;
    kinds.insert(k);
  }
  for (const auto &v : p.compatibleRecoveryVersions) {
    if (!validName(v.toStdString()) || versions.contains(v))
      return false;
    versions.insert(v);
  }
  m_providers.insert(p.id, std::move(p));
  return true;
}
ProviderRegistration *ResourceExecutor::provider(const ResourceRecord &r, bool recovery)
{
  auto i = m_providers.find(r.provider);
  if (i == m_providers.end() || !i->kinds.contains(r.kind))
    return nullptr;
  if (i->version != r.providerVersion && (!recovery || !i->compatibleRecoveryVersions.contains(r.providerVersion)))
    return nullptr;
  return &i.value();
}
bool ResourceExecutor::permit(const ResourceRecord &r, const ResourceAuthorization &a, TimePoint deadline) const
{
  return a.revoked && !a.revoked->load(std::memory_order_acquire) && Clock::now() < a.expires &&
         Clock::now() < deadline && r.owner == a.owner && r.session == a.session && r.planHash == a.planHash &&
         r.userSession == a.userSession && r.bootInstance == a.bootInstance && r.userSession == m_userSession &&
         r.bootInstance == m_bootInstance;
}
void ResourceExecutor::markBlocked(const ResourceRecord &r)
{
  if (r.phase != ResourcePhase::Closed && r.phase != ResourcePhase::CleanupRequired &&
      r.phase != ResourcePhase::ReleaseVerified)
    m_ledger.transition(
        r.id, ResourcePhase::CleanupRequired, r.platformReference, {"unproven", "cleanup-not-confirmed", "failed"}
    );
}
ResourceResult ResourceExecutor::create(ResourceRecord r, const ResourceAuthorization &a, TimePoint deadline)
{
  deadline = std::min(deadline, Clock::now() + std::chrono::seconds(10));
  ResourceResult result;
  if (!ownerThread()) {
    result.primaryError = "ResourceWrongThread";
    return result;
  }
  if (m_busy) {
    result.primaryError = "ResourceExecutorBusy";
    return result;
  }
  QScopedValueRollback<bool> busy(m_busy, true);
  if (m_ledger.status() != DurableResourceLedger::Status::Ready) {
    result.primaryError = "ResourceLedgerUnavailable";
    return result;
  }
  if (!permit(r, a, deadline)) {
    result.primaryError = "ResourcePermitRejected";
    return result;
  }
  auto *p = provider(r, false);
  if (!p || r.verificationPolicy != p->verificationPolicy) {
    result.primaryError = "ResourceProviderUnavailable";
    return result;
  }
  const auto existing = m_ledger.records().constFind(r.id);
  if (existing != m_ledger.records().cend()) {
    const bool same = existing->owner == r.owner && existing->creationIntent == r.creationIntent &&
                      existing->session == r.session && existing->planHash == r.planHash &&
                      existing->userSession == r.userSession && existing->bootInstance == r.bootInstance &&
                      existing->provider == r.provider && existing->providerVersion == r.providerVersion &&
                      existing->kind == r.kind && existing->dependencies == r.dependencies &&
                      existing->verificationPolicy == r.verificationPolicy;
    if (same && existing->phase == ResourcePhase::Active) {
      result.outcome = ResourceOutcome::Active;
      return result;
    }
    result.primaryError = "ResourceIntentAlreadyExists";
    return result;
  }
  for (const auto &dep : r.dependencies) {
    const auto i = m_ledger.records().constFind(dep);
    if (i == m_ledger.records().cend() || i->owner != r.owner || i->phase != ResourcePhase::Active) {
      result.primaryError = "ResourceDependencyNotActive";
      return result;
    }
  }
  const auto before = safeProvider([&] { return p->implementation->snapshot(r, deadline); }, ResourceContextSnapshot{});
  if (!before.complete || !validId(before.digest.toStdString(), 32)) {
    result.primaryError = "ResourceContextUnproven";
    return result;
  }
  r.preCreateContextDigest = before.digest;
  r.evidence = unknown();
  if (!permit(r, a, deadline)) {
    result.outcome = ResourceOutcome::Cancelled;
    result.primaryError = "ResourcePermitRevoked";
    return result;
  }
  if (!m_ledger.createIntent(r)) {
    result.primaryError = "ResourceIntentNotDurable";
    return result;
  }
  if (permit(r, a, deadline)) {
    const auto created = safeProvider([&] { return p->implementation->create(r, deadline); }, NativeCreation{});
    if (created.certainty == CreationCertainty::Created && reference(created.reference) &&
        ownership(created.ownership)) {
      if (!m_ledger.transition(r.id, ResourcePhase::Observed, created.reference, created.ownership))
        result.primaryError = "ResourceObservationNotDurable";
      else if (permit(r, a, deadline)) {
        const auto current = m_ledger.records().value(r.id);
        const auto observed =
            safeProvider([&] { return p->implementation->inspect(current, deadline); }, NativeInspection{});
        if (observed.presence == Presence::Owned && observed.reference == created.reference &&
            ownership(observed.evidence) && observed.topologyMatches) {
          if (!permit(r, a, deadline))
            result.primaryError = "ResourcePermitRevoked";
          else if (m_ledger.transition(r.id, ResourcePhase::Active, created.reference, observed.evidence)) {
            // A cancellation racing the durable write cannot expose the object.
            if (permit(r, a, deadline)) {
              result.outcome = ResourceOutcome::Active;
              return result;
            }
            result.primaryError = "ResourcePermitRevoked";
          } else
            result.primaryError = "ResourceActivationNotDurable";
        } else
          result.primaryError = "ResourceTopologyUnproven";
      } else
        result.primaryError = "ResourcePermitRevoked";
    } else
      result.primaryError =
          created.certainty == CreationCertainty::NotCreated ? "ResourceCreateFailed" : "ResourceCreateUncertain";
  } else
    result.primaryError = "ResourcePermitRevoked";
  // This individual resource has not been returned to the media assembler.
  // Compensate it even when journal I/O failed; durable Intent retains the owner.
  const bool closed = cleanupOne(r.id, Clock::now() + std::chrono::seconds(15), result.cleanupError, true);
  result.outcome = closed ? (permit(r, a, deadline) ? ResourceOutcome::Rejected : ResourceOutcome::Cancelled)
                          : ResourceOutcome::CleanupRequired;
  if (!closed)
    result.blockedResources = {r.id};
  return result;
}
bool ResourceExecutor::cleanupOne(const QString &id, TimePoint deadline, QString &error, bool compensation)
{
  auto r = m_ledger.records().value(id);
  auto fail = [&](const QString &code) {
    error = code;
    markBlocked(m_ledger.records().value(id));
    return false;
  };
  if (r.phase == ResourcePhase::Closed)
    return true;
  if (Clock::now() >= deadline)
    return fail("ResourceCleanupDeadline");
  // A durable verified release is already terminal evidence; closing never
  // recreates or deletes a resource which may since have reused a platform ID.
  if (r.phase == ResourcePhase::ReleaseVerified)
    return m_ledger.transition(id, ResourcePhase::Closed, r.platformReference, r.evidence) ||
           fail("ResourceClosureNotDurable");
  auto *p = provider(r, true);
  if (!p)
    return fail("ResourceRecoveryProviderUnavailable");
  if (!safeProvider([&] { return p->implementation->quiesce(r, deadline); }, false))
    return fail("ResourceCreationStillPending");
  const auto inspected = safeProvider([&] { return p->implementation->inspect(r, deadline); }, NativeInspection{});
  if (inspected.presence != Presence::Absent && inspected.presence != Presence::Owned)
    return fail("ResourceOwnershipAmbiguous");
  if (inspected.presence == Presence::Absent && !absence(inspected.evidence))
    return fail("ResourceAbsenceUnproven");
  if (inspected.presence == Presence::Owned) {
    if (r.userSession != m_userSession || r.bootInstance != m_bootInstance || !ownership(inspected.evidence) ||
        !reference(inspected.reference) ||
        (!r.platformReference.isEmpty() && r.platformReference != inspected.reference))
      return fail("ResourceOwnershipConflict");
    if (r.platformReference.isEmpty()) {
      r.platformReference = inspected.reference;
      // Intent-only recovery may discover an exact owner-bound object. Do not
      // infer ownership from a display name, model or topology difference.
      if (r.phase == ResourcePhase::Intent) {
        if (!m_ledger.transition(id, ResourcePhase::Observed, r.platformReference, inspected.evidence) && !compensation)
          return fail("ResourceRecoveryObservationNotDurable");
      }
    }
  }
  const auto persisted = m_ledger.records().value(id);
  bool releasing = persisted.phase == ResourcePhase::Releasing && persisted.platformReference == r.platformReference;
  if (!releasing) {
    if (persisted.phase == ResourcePhase::Releasing && persisted.platformReference.isEmpty() &&
        !r.platformReference.isEmpty())
      m_ledger.transition(id, ResourcePhase::CleanupRequired, r.platformReference, inspected.evidence);
    releasing = m_ledger.transition(id, ResourcePhase::Releasing, r.platformReference, unknown());
  }
  if (!releasing && !compensation)
    return fail("ResourceStopIntentNotDurable");
  if (inspected.presence == Presence::Owned) {
    if (Clock::now() >= deadline)
      return fail("ResourceCleanupDeadline");
    safeProvider(
        [&] { return p->implementation->release(r, inspected.evidence, deadline); }, false
    ); // A return value cannot prove absence.
  }
  if (Clock::now() >= deadline)
    return fail("ResourceCleanupDeadline");
  const auto after = safeProvider([&] { return p->implementation->inspect(r, deadline); }, NativeInspection{});
  if (after.presence != Presence::Absent || !absence(after.evidence))
    return fail("ResourceStillPresent");
  if (!releasing || !m_ledger.transition(id, ResourcePhase::ReleaseVerified, r.platformReference, after.evidence))
    return fail("ResourceReleaseNotDurable");
  if (!m_ledger.transition(id, ResourcePhase::Closed, r.platformReference, after.evidence))
    return fail("ResourceClosureNotDurable");
  return true;
}
ResourceResult ResourceExecutor::cleanupOwner(const QString &owner, TimePoint deadline)
{
  deadline = std::min(deadline, Clock::now() + std::chrono::seconds(15));
  ResourceResult result;
  result.outcome = ResourceOutcome::CleanupRequired;
  if (!ownerThread()) {
    result.primaryError = "ResourceWrongThread";
    return result;
  }
  if (m_busy) {
    result.primaryError = "ResourceExecutorBusy";
    return result;
  }
  QScopedValueRollback<bool> busy(m_busy, true);
  if (!validId(owner.toStdString(), 16) || !m_quiesced || !m_quiesced(owner)) {
    result.primaryError = "ResourceProducersNotQuiesced";
    return result;
  }
  if (m_ledger.status() != DurableResourceLedger::Status::Ready) {
    result.primaryError = "ResourceLedgerUnavailable";
    return result;
  }
  QSet<QString> attempted;
  for (int pass = 0; pass < 64; ++pass) {
    bool progress = false;
    const auto records = m_ledger.records();
    for (const auto &r : records) {
      if (r.owner != owner || r.phase == ResourcePhase::Closed || attempted.contains(r.id))
        continue;
      bool dependent = false;
      for (const auto &child : m_ledger.records())
        if (child.owner == owner && child.phase != ResourcePhase::Closed && child.dependencies.contains(r.id)) {
          dependent = true;
          break;
        }
      if (dependent)
        continue;
      attempted.insert(r.id);
      QString error;
      if (cleanupOne(r.id, deadline, error))
        progress = true;
      else if (result.cleanupError.isEmpty())
        result.cleanupError = error;
    }
    if (!progress)
      break;
  }
  for (const auto &r : m_ledger.records())
    if (r.owner == owner && r.phase != ResourcePhase::Closed)
      result.blockedResources.push_back(r.id);
  if (result.blockedResources.isEmpty())
    result.outcome = ResourceOutcome::Closed;
  return result;
}
} // namespace deskflow::display::infra
