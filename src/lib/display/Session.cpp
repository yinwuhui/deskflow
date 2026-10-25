// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "Session.h"

#include <limits>
#include <stdexcept>

namespace deskflow::display {

void SessionState::nextGeneration()
{
  if (m_generation == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("Session generation exhausted");
  ++m_generation;
}

uint64_t SessionState::begin()
{
  if (m_state != State::Idle) throw std::logic_error("Session must be idle before starting");
  nextGeneration();
  m_state = State::Connecting;
  return m_generation;
}

uint64_t SessionState::stop()
{
  if (m_state != State::Idle && m_state != State::Stopping) {
    nextGeneration();
    m_state = State::Stopping;
  }
  return m_generation;
}

bool SessionState::advance(uint64_t generation, Event event)
{
  if (generation != m_generation) return false;
  switch (m_state) {
  case State::Connecting:
    if (event == Event::Connected) { m_state = State::Authorizing; return true; }
    break;
  case State::Authorizing:
    if (event == Event::Authorized) { m_state = State::Negotiating; return true; }
    break;
  case State::Negotiating:
    if (event == Event::Agreed) { m_state = State::Preparing; return true; }
    break;
  case State::Preparing:
    if (event == Event::FirstFrame) { m_state = State::Streaming; return true; }
    break;
  case State::Streaming:
    if (event == Event::Reconfigure) { nextGeneration(); m_state = State::Reconfiguring; return true; }
    if (event == Event::Interrupted) { nextGeneration(); m_state = State::Recovering; return true; }
    break;
  case State::Reconfiguring:
    if (event == Event::FirstFrame) { m_state = State::Streaming; return true; }
    break;
  case State::Recovering:
    if (event == Event::Reconnected) { m_state = State::Preparing; return true; }
    break;
  case State::Stopping:
  case State::CleanupRequired:
    if (event == Event::CleanupSucceeded) { m_state = State::Idle; return true; }
    if (event == Event::CleanupFailed) { m_state = State::CleanupRequired; return true; }
    break;
  case State::Idle: break;
  }
  return false;
}

bool InputLease::active(TimePoint now) const
{
  return m_lease != 0 && now < m_deadline;
}

bool InputLease::matches(SessionId owner, uint64_t lease, uint32_t epoch, TimePoint now) const
{
  return active(now) && m_owner == owner && m_lease == lease && m_epoch == epoch;
}

std::vector<PressedInput> InputLease::grant(SessionId owner, uint64_t lease, uint32_t epoch,
                                          TimePoint now, std::chrono::milliseconds duration)
{
  if (owner == SessionId{} || !epoch || lease <= m_lastLease || duration.count() <= 0 || duration > std::chrono::seconds(2))
    throw std::invalid_argument("Invalid lease identity, lifetime or reused lease number");
  auto released = revoke();
  m_owner = owner;
  m_lease = lease;
  m_lastLease = lease;
  m_epoch = epoch;
  m_deadline = now + duration;
  return released;
}

bool InputLease::renew(SessionId owner, uint64_t lease, uint32_t epoch,
                       TimePoint now, std::chrono::milliseconds duration)
{
  if (!matches(owner, lease, epoch, now) || duration.count() <= 0 || duration > std::chrono::seconds(2)) return false;
  m_deadline = now + duration;
  return true;
}

bool InputLease::accept(SessionId owner, uint64_t lease, uint32_t epoch, TimePoint now,
                        PressedInput input, bool down)
{
  if (!matches(owner, lease, epoch, now)) return false;
  if (input.kind != PressedInput::Kind::Key && input.kind != PressedInput::Kind::Button) return false;
  if (input.kind == PressedInput::Kind::Button && (input.code < 1 || input.code > 5)) return false;
  if (input.kind == PressedInput::Kind::Key && (input.code == 0 || input.code > 0xE7)) return false;
  if (down) m_pressed.insert(input);
  else m_pressed.erase(input);
  return true;
}

std::vector<PressedInput> InputLease::expire(TimePoint now)
{
  if (m_lease && !active(now)) return revoke();
  return {};
}

std::vector<PressedInput> InputLease::revoke()
{
  std::vector<PressedInput> released(m_pressed.begin(), m_pressed.end());
  m_pressed.clear();
  m_owner = {};
  m_lease = 0;
  m_epoch = 0;
  return released;
}

} // namespace deskflow::display
