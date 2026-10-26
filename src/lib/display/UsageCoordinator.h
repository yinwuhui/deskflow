// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "Coordinator.h"
namespace deskflow::display {
enum class UsageMode { None, Input, Extended };
enum class UsageState { Inactive, InputActive, DisplayActive, Switching, CleanupRequired };
enum class SwitchStage { None, Preflight, AwaitingPermission, ReleasingInputs, StoppingInput,
                         StartingDisplay, StoppingDisplay, StartingInput, CleaningDisplay,
                         CheckingRollback, RestoringInput };
enum class RollbackResult { NotNeeded, Pending, Succeeded, Unavailable, Failed, Cancelled };
struct UsageKey {
  SessionId operation{};
  uint64_t generation = 0;
  bool operator==(const UsageKey&) const = default;
};
struct UsageCapabilities {
  Availability input = Availability::Unknown;
  Availability focusSync = Availability::Unknown;
  Availability keyboard = Availability::Unknown;
  Availability extended = Availability::Unknown;
  Availability targetIsolation = Availability::Unknown;
  Availability inputRelease = Availability::Unknown;
  Availability displayRouting = Availability::Unknown;
  bool networkAvailable = false;
  Reason displayPolicyReason = Reason::None;
  bool operator==(const UsageCapabilities&) const = default;
};
struct UsageSnapshot {
  std::string peerId;
  UsageKey key;
  uint64_t revision = 0;
  UsageMode selectedMode = UsageMode::Input;
  UsageMode effectiveMode = UsageMode::None;
  UsageMode targetMode = UsageMode::None;
  UsageState state = UsageState::Inactive;
  SwitchStage stage = SwitchStage::None;
  Permission inputPermission = Permission::Unknown;
  std::string inputFocusOwner; // Empty until the input adapter reports the unified focus.
  bool pressedInputsReleased = true;
  bool oldModeStopped = false;
  RollbackResult rollback = RollbackResult::NotNeeded;
  Reason reason = Reason::None;
};
enum class UsageEffectKind { PreflightInput, PreflightDisplay, RequestInputPermission, RequestDisplayPermission,
  ReleaseInputs, StopInput, StartDisplay, StopDisplay, StartInput, CancelDisplayRequest,
  VerifyRollbackAuthorization, RestoreInput, RevokeInput };
struct UsageEffect {
  UsageEffectKind kind;
  UsageKey key;
};
// Per authenticated peer. The application service owns one instance per peer;
// the display subservice additionally enforces the global one-display limit.
class UsageCoordinator {
public:
  UsageCoordinator(std::string peerId, UsageCapabilities capabilities);
  const UsageSnapshot& snapshot() const { return m_snapshot; }
  void setCapabilities(UsageCapabilities capabilities) { if(m_capabilities!=capabilities){m_capabilities=capabilities;changed();} }
  Outcome select(UsageMode mode);
  Outcome begin(UsageMode mode, SessionId operation);
  Outcome cancel(const UsageKey& key);
  Outcome stop(SessionId operation);
  Outcome retryCleanup(SessionId operation);
  Outcome restoreDisplayCleanup(SessionId owner);
  Outcome synchronizeInput(Permission permission, std::string focusOwner, bool active,SessionId owner={});
  bool preflight(const UsageKey& key, Reason result);
  bool permitted(const UsageKey& key, Reason result);
  bool inputsReleased(const UsageKey& key, bool verified);
  bool inputStopped(const UsageKey& key, bool verified);
  bool inputCleaned(const UsageKey& key, bool verified);
  bool displayStarted(const UsageKey& key);
  bool displayEnded(const UsageKey& key, Reason reason, bool cleanupVerified);
  bool displayFailed(const UsageKey& key, Reason reason, bool cleanupVerified);
  bool displayStopped(const UsageKey& key, bool cleanupVerified);
  bool inputStarted(const UsageKey& key, bool verified);
  bool rollbackAuthorized(const UsageKey& key, bool valid);
  void connectionLost(bool inputReleaseVerified=false);
  void restrictDisplay(Reason reason,SessionId stopOperation);
  void invalidateRollback(SessionId stopOperation);
  void inputStatus(Permission permission, std::string focusOwner);
  void tick(Coordinator::TimePoint now);
  std::vector<UsageEffect> takeEffects();
private:
  bool matches(const UsageKey& key) const;
  void changed();
  void effect(UsageEffectKind kind);
  Outcome check(UsageMode mode) const;
  void afterPermission();
  void complete(UsageMode mode, Reason reason = Reason::None);
  void failedAfterStop(Reason reason, bool cleanupVerified);
  void rollbackOrEnd();
  UsageSnapshot m_snapshot;
  UsageCapabilities m_capabilities;
  std::vector<UsageEffect> m_effects;
  UsageMode m_original = UsageMode::None;
  bool m_allowRollback = false;
  bool m_stopAll = false;
  bool m_cancelRequested = false;
  std::deque<SessionId> m_recentOperations;
  UsageKey m_timedKey;
  SwitchStage m_timedStage = SwitchStage::None;
  Coordinator::TimePoint m_deadline{};
};
const char* usageModeName(UsageMode mode);
const char* usageStateName(UsageState state);
const char* switchStageName(SwitchStage stage);
const char* rollbackName(RollbackResult rollback);
const char* usageEffectName(UsageEffectKind effect);
} // namespace deskflow::display
