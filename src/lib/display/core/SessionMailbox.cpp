// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "SessionMailbox.h"
#include <algorithm>
#include <type_traits>
#include <utility>
namespace deskflow::display::infra {
namespace {
bool tokenValid(const SessionToken &t)
{
  return validId(t.session, 16) && validId(t.owner, 16) && t.attempt && t.generation && t.epoch &&
         (t.worker.empty() || validId(t.worker, 16));
}
int critical(const MailboxPayload &p)
{
  if (std::holds_alternative<MailboxRevoked>(p))
    return 0;
  if (std::holds_alternative<MailboxDisconnected>(p))
    return 1;
  if (std::holds_alternative<MailboxWorkerExited>(p))
    return 2;
  if (std::holds_alternative<MailboxStop>(p))
    return 3;
  return -1;
}
bool valid(const MailboxEvent &e)
{
  if (!tokenValid(e.token))
    return false;
  return std::visit(
      [&](const auto &p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, MailboxStop>)
          return int(p.reason) >= 0 && int(p.reason) <= int(MailboxStopReason::Deadline);
        else if constexpr (std::is_same_v<T, MailboxWorkerAttached> || std::is_same_v<T, MailboxWorkerExited>)
          return validId(p.worker, 16);
        else if constexpr (std::is_same_v<T, MailboxPresented>)
          return !e.token.worker.empty() && p.frame && int(p.evidence) >= 0 &&
                 int(p.evidence) <= int(PresentationEvidence::CompositorPresented);
        else if constexpr (std::is_same_v<T, MailboxStatistics>)
          return !e.token.worker.empty() && p.sequence;
        else if constexpr (std::is_same_v<T, MailboxWorkerQuiesced>)
          return !e.token.worker.empty();
        return true;
      },
      e.payload
  );
}
} // namespace
SessionMailbox::SessionMailbox(InfrastructureSession &session, uint32_t capacity)
    : m_session(session),
      m_owner(std::this_thread::get_id()),
      m_scope(session.snapshot().token),
      m_capacity(capacity <= OrdinaryCapacity ? capacity : 0),
      m_valid(tokenValid(m_scope) && m_capacity && session.snapshot().state != Lifecycle::Idle)
{
}
bool SessionMailbox::scope(const SessionToken &t) const
{
  return t.session == m_scope.session && t.owner == m_scope.owner && t.attempt == m_scope.attempt;
}
MailboxPost SessionMailbox::post(const MailboxEvent &event)
{
  if (!m_valid || !valid(event))
    return MailboxPost::Invalid;
  if (!scope(event.token))
    return MailboxPost::WrongScope;
  std::lock_guard lock(m_mutex);
  const auto priority = critical(event.payload);
  if (priority >= 0) {
    auto &slot = m_critical[size_t(priority)];
    if (slot) {
      // Different exits must not silently replace an instance still needing
      // processing. Trusted supervision still owns both instance records.
      if (priority == 2 &&
          std::get<MailboxWorkerExited>(slot->payload).worker != std::get<MailboxWorkerExited>(event.payload).worker) {
        m_overflow = true;
        return MailboxPost::Full;
      }
      return MailboxPost::Coalesced;
    }
    slot = event;
    return MailboxPost::Queued;
  }
  if (const auto *stats = std::get_if<MailboxStatistics>(&event.payload)) {
    if (m_statistics) {
      // Different generations may arrive concurrently; never let a stale
      // snapshot replace a newer token. The core checks the exact token at drain.
      const auto &old = m_statistics->token;
      const auto &next = event.token;
      if (next.generation < old.generation || (next.generation == old.generation && next.epoch < old.epoch))
        return MailboxPost::Invalid;
      if (next == old && stats->sequence <= std::get<MailboxStatistics>(m_statistics->payload).sequence)
        return MailboxPost::Coalesced;
      m_statistics = event;
      return MailboxPost::Coalesced;
    }
    m_statistics = event;
    return MailboxPost::Queued;
  }
  if (m_ordinary.size() >= m_capacity) {
    m_overflow = true;
    return MailboxPost::Full;
  }
  m_ordinary.push_back(event);
  return MailboxPost::Queued;
}
MailboxUsage SessionMailbox::usage() const
{
  std::lock_guard lock(m_mutex);
  return {
      uint32_t(m_ordinary.size()),
      uint32_t(std::count_if(m_critical.begin(), m_critical.end(), [](const auto &v) { return v.has_value(); })),
      m_statistics.has_value(), m_overflow
  };
}
bool SessionMailbox::apply(const MailboxEvent &event, TimePoint now)
{
  const auto &t = event.token;
  const auto &current = m_session.snapshot().token;
  if (!scope(current))
    return false;
  return std::visit(
      [&](const auto &p) -> bool {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, MailboxStop>) {
          switch (p.reason) {
          case MailboxStopReason::Cancelled:
            return m_session.stop("Cancelled", now);
          case MailboxStopReason::Shutdown:
            return m_session.stop("Shutdown", now);
          case MailboxStopReason::Deadline:
            return m_session.stop("Deadline", now);
          }
        } else if constexpr (std::is_same_v<T, MailboxRevoked>)
          return m_session.stop("AuthorizationRevoked", now);
        else if constexpr (std::is_same_v<T, MailboxDisconnected>)
          return m_session.stop("ConnectionLost", now);
        else if constexpr (std::is_same_v<T, MailboxWorkerExited>) {
          // Exit may overtake the ordinary attach completion. Register the exact
          // in-flight launch using its original token before confirming quiescence.
          if (current.worker.empty() && !m_session.attachWorker(t, p.worker, now))
            return false;
          if (m_session.snapshot().token.worker != p.worker)
            return false;
          m_session.stop("WorkerCrashed", now);
          return m_session.workerQuiesced(t.owner, p.worker, now);
        } else if constexpr (std::is_same_v<T, MailboxIntentCompleted>)
          return m_session.intentPersisted(t, p.success, now);
        else if constexpr (std::is_same_v<T, MailboxWorkerAttached>)
          return m_session.attachWorker(t, p.worker, now);
        else if constexpr (std::is_same_v<T, MailboxPrepared>)
          return p.success ? m_session.prepared(t, now) : m_session.preparationFailed(t, now);
        else if constexpr (std::is_same_v<T, MailboxPresented>) {
          if (t != current)
            return false;
          if (m_lastPresentation && m_lastPresentation->token == t &&
              p.frame <= std::get<MailboxPresented>(m_lastPresentation->payload).frame)
            return false;
          if (!m_session.presented(t, p.evidence, p.exposed, now))
            return false;
          m_lastPresentation = event;
          return true;
        } else if constexpr (std::is_same_v<T, MailboxRecovered>)
          return m_session.recovered(t, now);
        else if constexpr (std::is_same_v<T, MailboxWorkerQuiesced>)
          return m_session.workerQuiesced(t.owner, t.worker, now);
        else if constexpr (std::is_same_v<T, MailboxResourcesDestroyed>)
          return m_session.resourcesDestroyed(t.owner, now);
        else if constexpr (std::is_same_v<T, MailboxCleanupVerified>)
          return m_session.cleanupEvidence(t.owner, p.verified, now);
        return false;
      },
      event.payload
  );
}
MailboxDrain SessionMailbox::drain(TimePoint now, uint32_t limit)
{
  MailboxDrain result;
  if (std::this_thread::get_id() != m_owner || !m_valid || (m_lastDrain && now < *m_lastDrain))
    return result;
  m_lastDrain = now;
  result.ownerAccepted = true;
  std::array<std::optional<MailboxEvent>, 4> urgent;
  std::deque<MailboxEvent> ordinary;
  std::optional<MailboxEvent> statistics;
  bool overflow = false;
  {
    std::lock_guard lock(m_mutex);
    urgent.swap(m_critical);
    overflow = std::exchange(m_overflow, false);
    for (uint32_t i = 0; i < std::min(limit, OrdinaryCapacity) && !m_ordinary.empty(); ++i) {
      ordinary.push_back(std::move(m_ordinary.front()));
      m_ordinary.pop_front();
    }
    statistics.swap(m_statistics);
  }
  const auto dispatch = [&](const MailboxEvent &e) {
    if (apply(e, now))
      ++result.applied;
    else
      ++result.ignored;
  };
  for (const auto &e : urgent)
    if (e)
      dispatch(*e);
  if (overflow && scope(m_session.snapshot().token))
    result.overflowStop = m_session.stop("OwnerMailboxOverflow", now);
  for (const auto &e : ordinary)
    dispatch(e);
  // Timeouts remain local and run even if no producer has posted an event.
  if (scope(m_session.snapshot().token))
    m_session.tick(now);
  if (statistics) {
    const auto &current = m_session.snapshot();
    const auto &next = std::get<MailboxStatistics>(statistics->payload);
    const bool fresh = !m_lastStatistics || m_lastStatistics->token != statistics->token ||
                       next.sequence > std::get<MailboxStatistics>(m_lastStatistics->payload).sequence;
    if (scope(current.token) && current.token == statistics->token && fresh &&
        (current.state == Lifecycle::AwaitingFirstFrame || current.state == Lifecycle::Streaming ||
         current.state == Lifecycle::Recovering)) {
      m_lastStatistics = *statistics;
      result.statistics = std::move(statistics);
    } else
      ++result.ignored;
  }
  return result;
}
} // namespace deskflow::display::infra
