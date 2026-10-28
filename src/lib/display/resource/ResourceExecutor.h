// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "DurableResourceLedger.h"
#include "display/core/InfrastructureTypes.h"
#include <QThread>
#include <atomic>
#include <functional>
#include <memory>

namespace deskflow::display::infra {
struct ResourceAuthorization
{
  QString owner, session, planHash, userSession, bootInstance;
  TimePoint expires;
  std::shared_ptr<std::atomic<bool>> revoked;
};
struct ResourceContextSnapshot
{
  bool complete = false;
  QString digest;
};
enum class CreationCertainty
{
  NotCreated,
  Created,
  Unknown
};
struct NativeCreation
{
  CreationCertainty certainty = CreationCertainty::Unknown;
  QString reference;
  ResourceEvidence ownership;
};
enum class Presence
{
  Absent,
  Owned,
  Ambiguous,
  Unavailable
};
struct NativeInspection
{
  Presence presence = Presence::Unavailable;
  QString reference;
  ResourceEvidence evidence;
  bool topologyMatches = false;
};
// Invoked on a dedicated resource executor thread, never a UI/core callback.
// Implementations must enforce the deadline via supervised native helpers when
// an OS call cannot be interrupted. Results are trusted local platform evidence.
class PlatformResourceProvider
{
public:
  virtual ~PlatformResourceProvider() = default;
  virtual ResourceContextSnapshot snapshot(const ResourceRecord &, TimePoint deadline) = 0;
  virtual NativeCreation create(const ResourceRecord &, TimePoint deadline) = 0;
  // Invoked only after ResourceObserved is durable. Non-layout providers may
  // keep the no-op default; display providers configure only their owned output.
  virtual bool configure(const ResourceRecord &, TimePoint)
  {
    return true;
  }
  // Stop and acknowledge all pending owner-bound creates before absence can be
  // trusted. A timed-out asynchronous create must not appear after OwnerClosed.
  virtual bool quiesce(const ResourceRecord &, TimePoint deadline) = 0;
  virtual NativeInspection inspect(const ResourceRecord &, TimePoint deadline) = 0;
  virtual bool release(const ResourceRecord &, const ResourceEvidence &ownership, TimePoint deadline) = 0;
};
struct ProviderRegistration
{
  QString id, version, verificationPolicy;
  QStringList kinds, compatibleRecoveryVersions;
  std::shared_ptr<PlatformResourceProvider> implementation;
};
enum class ResourceOutcome
{
  Active,
  Rejected,
  Cancelled,
  CleanupRequired,
  Closed
};
struct ResourceResult
{
  ResourceOutcome outcome = ResourceOutcome::Rejected;
  QString primaryError, cleanupError;
  QStringList blockedResources;
};
class ResourceExecutor
{
public:
  ResourceExecutor(
      DurableResourceLedger &, QString userSession, QString bootInstance,
      std::function<bool(const QString &owner)> producersQuiesced
  );
  bool registerProvider(ProviderRegistration);
  ResourceResult create(ResourceRecord, const ResourceAuthorization &, TimePoint absoluteDeadline);
  ResourceResult cleanupOwner(const QString &owner, TimePoint absoluteDeadline);

private:
  bool permit(const ResourceRecord &, const ResourceAuthorization &, TimePoint deadline) const;
  bool ownerThread() const;
  ProviderRegistration *provider(const ResourceRecord &, bool recovery);
  bool cleanupOne(const QString &, TimePoint deadline, QString &error, bool compensation = false);
  void markBlocked(const ResourceRecord &);
  DurableResourceLedger &m_ledger;
  QString m_userSession, m_bootInstance;
  Qt::HANDLE m_thread;
  bool m_busy = false;
  std::function<bool(const QString &)> m_quiesced;
  QMap<QString, ProviderRegistration> m_providers;
};
} // namespace deskflow::display::infra
