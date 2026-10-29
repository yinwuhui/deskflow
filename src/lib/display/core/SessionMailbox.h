// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "InfrastructureSession.h"
#include <array>
#include <mutex>
#include <thread>
#include <variant>

namespace deskflow::display::infra {
enum class MailboxStopReason
{
  Cancelled,
  Shutdown,
  Deadline
};
struct MailboxStop
{
  MailboxStopReason reason = MailboxStopReason::Cancelled;
};
struct MailboxRevoked
{
};
struct MailboxDisconnected
{
};
// Only a trusted supervisor may post this after confirming that exact instance
// has exited. A worker's self-report is not native process-absence evidence.
struct MailboxWorkerExited
{
  std::string worker;
};
struct MailboxIntentCompleted
{
  bool success = false;
};
struct MailboxWorkerAttached
{
  std::string worker;
};
struct MailboxPrepared
{
  bool success = false;
};
struct MailboxPresented
{
  uint64_t frame = 0;
  PresentationEvidence evidence = PresentationEvidence::Decoded;
  bool exposed = false;
};
struct MailboxRecovered
{
};
struct MailboxWorkerQuiesced
{
};
struct MailboxResourcesDestroyed
{
};
struct MailboxCleanupVerified
{
  bool verified = false;
};
struct MailboxStatistics
{
  uint64_t sequence = 0, captured = 0, decoded = 0, presented = 0, retainedBytes = 0;
};
using MailboxPayload = std::variant<
    MailboxStop, MailboxRevoked, MailboxDisconnected, MailboxWorkerExited, MailboxIntentCompleted,
    MailboxWorkerAttached, MailboxPrepared, MailboxPresented, MailboxRecovered, MailboxWorkerQuiesced,
    MailboxResourcesDestroyed, MailboxCleanupVerified, MailboxStatistics>;
struct MailboxEvent
{
  SessionToken token;
  MailboxPayload payload;
};
enum class MailboxPost
{
  Queued,
  Coalesced,
  Full,
  Invalid,
  WrongScope
};
struct MailboxUsage
{
  uint32_t ordinary = 0, critical = 0;
  bool statistics = false, overflow = false;
};
struct MailboxDrain
{
  bool ownerAccepted = false;
  uint32_t applied = 0, ignored = 0;
  bool overflowStop = false;
  std::optional<MailboxEvent> statistics;
};
// One mailbox for one reserved session/owner/attempt. Construct and drain on
// the lifecycle owner thread; producers only post. No callbacks, native objects,
// raw frames, arbitrary strings or JSON are carried through this queue.
// The session must outlive the mailbox. It is never accessed by producer threads.
class SessionMailbox
{
public:
  static constexpr uint32_t OrdinaryCapacity = 64;
  explicit SessionMailbox(InfrastructureSession &, uint32_t ordinaryCapacity = OrdinaryCapacity);
  // Full means the event was NOT accepted. The producer retains ownership of
  // the operation and must retry its typed completion; do not destroy native
  // resources or infer cleanup from a failed post. Overflow also latches stop.
  MailboxPost post(const MailboxEvent &);
  MailboxUsage usage() const;
  // A bounded batch. All critical slots and the overflow latch are processed
  // ahead of up to ordinaryLimit completions. No lock is held across core calls.
  MailboxDrain drain(TimePoint now, uint32_t ordinaryLimit = 32);

private:
  bool scope(const SessionToken &) const;
  bool apply(const MailboxEvent &, TimePoint);
  InfrastructureSession &m_session;
  const std::thread::id m_owner;
  const SessionToken m_scope;
  const uint32_t m_capacity;
  const bool m_valid;
  mutable std::mutex m_mutex;
  std::deque<MailboxEvent> m_ordinary;
  std::array<std::optional<MailboxEvent>, 4> m_critical;
  std::optional<MailboxEvent> m_statistics;
  bool m_overflow = false;
  std::optional<TimePoint> m_lastDrain;
  std::optional<MailboxEvent> m_lastStatistics, m_lastPresentation;
};
} // namespace deskflow::display::infra
