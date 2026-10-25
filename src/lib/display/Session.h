// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once

#include "Protocol.h"
#include <chrono>
#include <compare>
#include <set>

namespace deskflow::display {

enum class State { Idle, Connecting, Authorizing, Negotiating, Preparing, Streaming,
                   Reconfiguring, Recovering, Stopping, CleanupRequired };
enum class Event { Connected, Authorized, Agreed, FirstFrame, Reconfigure, Interrupted,
                   Reconnected, CleanupSucceeded, CleanupFailed };

class SessionState {
public:
  uint64_t begin();
  uint64_t stop();
  bool advance(uint64_t generation, Event event);
  State state() const { return m_state; }
  uint64_t generation() const { return m_generation; }
private:
  void nextGeneration();
  uint64_t m_generation = 0;
  State m_state = State::Idle;
};

struct PressedInput {
  enum class Kind { Key, Button };
  Kind kind;
  uint16_t code;
  auto operator<=>(const PressedInput&) const = default;
};

// Only tracks ownership and releases. The coordinator authorizes a grant;
// a platform adapter executes returned releases and supplies event filtering.
class InputLease {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  std::vector<PressedInput> grant(SessionId owner, uint64_t lease, uint32_t epoch,
                                TimePoint now, std::chrono::milliseconds duration);
  bool renew(SessionId owner, uint64_t lease, uint32_t epoch,
             TimePoint now, std::chrono::milliseconds duration);
  bool accept(SessionId owner, uint64_t lease, uint32_t epoch, TimePoint now,
              PressedInput input, bool down);
  std::vector<PressedInput> expire(TimePoint now);
  std::vector<PressedInput> revoke();
  bool active(TimePoint now) const;
private:
  bool matches(SessionId owner, uint64_t lease, uint32_t epoch, TimePoint now) const;
  SessionId m_owner{};
  uint64_t m_lease = 0;
  uint64_t m_lastLease = 0;
  uint32_t m_epoch = 0;
  TimePoint m_deadline{};
  std::set<PressedInput> m_pressed;
};

} // namespace deskflow::display
