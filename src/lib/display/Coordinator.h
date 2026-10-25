// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "Session.h"
#include <deque>
#include <optional>

namespace deskflow::display {

enum class Role { None, Host, Receiver };
enum class Availability { Unknown, Checking, Supported, Unsupported };
enum class Reason {
  None, CapabilityCheckRequired, UnsupportedPlatform, UnsupportedPeerVersion, PeerBusy, PeerRejected,
  RequestExpired, Cancelled, PermissionRequired, PermissionDenied, SessionUnavailable,
  NetworkUnavailable, IdentityChanged, InputConflict, DisplayCreationFailed,
  LayoutRejected, DecodeFailed, PresentationFailed, Congestion, ConnectionLost,
  UserStopped, ApplicationExit, CleanupFailed, InvalidRequest, InputCapabilityIncomplete,
  InputIsolationUnsupported, InputReleaseFailed, InputPermissionDenied, ModeConflict, SwitchFailed, RollbackFailed
};
enum class Routing { Unknown, Clear, ConflictingPeer, UnverifiedOtherRoutes };
enum class Permission { Unknown, Required, Granted, Denied };
enum class Cleanup { None, Pending, Verified, Required };
enum class Action { Start, Allow, Reject, Cancel, Stop, ShowWindow, RetryCleanup, OpenSystemSettings };

struct Capabilities {
  Availability send = Availability::Unknown;
  Availability receive = Availability::Unknown;
  bool layout = false;
  bool fullscreen = false;
  bool sessionAvailable = false;
  unsigned protocol = 1;
  // P0 deliberately has no remote input, automatic recovery or dynamic modes.
};
struct Peer {
  std::string id; // SHA-256 identity from the authenticated transport, never the device name/IP.
  std::string name;
  Capabilities capabilities;
};
struct Key {
  SessionId session{};
  SessionId request{};
  uint64_t generation = 0;
  uint32_t epoch = 1;
  bool operator==(const Key&) const = default;
};
struct Layout {
  int x = 0;
  int y = 0;
  double scale = 1;
  bool operator==(const Layout&) const = default;
};
struct Snapshot {
  Key key;
  uint64_t revision = 0;
  Role role = Role::None;
  Peer peer;
  State state = State::Idle;
  Reason reason = Reason::None;
  Reason terminalReason = Reason::None;
  Permission permission = Permission::Unknown;
  Cleanup cleanup = Cleanup::None;
  std::string virtualDisplayId;
  std::optional<Layout> confirmedLayout;
  std::string receiverScreenId;
  bool fullscreen = false;
  bool firstFramePresented = false;
  bool receiveRequestsEnabled = true;
  bool inputEnabled = false;
  bool inputRoutingSuspended = false;
  std::vector<Action> actions;
};

enum class EffectKind { SendRequest, SendAccepted, SendRejected, SendPrepare, CancelRequest,
                        PrepareHost, PrepareReceiver, StopPeer, CleanupResources, ShowWindow };
struct Effect {
  EffectKind kind;
  Key key;
  Reason reason = Reason::None;
  uint64_t ownerGeneration = 0;
};
struct Outcome {
  bool accepted = false;
  Reason reason = Reason::None;
};

// Single coordinator-owner thread. No callbacks, sockets, timers or blocking work
// run here. Adapters execute effects asynchronously and return generation-tagged
// results. Resource ownership survives callback invalidation until verified cleanup.
class Coordinator {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  explicit Coordinator(Capabilities local);
  const Snapshot& snapshot() const { return m_snapshot; }
  const Capabilities& capabilities() const { return m_local; }
  std::vector<Effect> takeEffects();
  void setCapabilities(Capabilities local, TimePoint now);
  void setReceiveRequestsEnabled(bool enabled);
  Outcome request(Peer peer, SessionId session, SessionId request, Routing routing, TimePoint now, bool deferPreparation = false);
  Outcome resumePreparation(const Key& key, Routing routing, TimePoint now);
  Outcome incoming(Peer peer, SessionId session, SessionId request, TimePoint now);
  Outcome respond(const Key& key, bool allow, TimePoint now);
  bool peerResponse(const Key& key, bool allow, Reason reason, TimePoint now);
  bool permission(const Key& key, Permission permission, TimePoint now);
  bool peerReady(const Key& key, TimePoint now);
  bool prepared(const Key& key, std::string ownedDisplayId, TimePoint now);
  // Called only by a trusted renderer/authenticated peer presentation event.
  bool presented(const Key& key, TimePoint now);
  bool fail(const Key& key, Reason reason, TimePoint now);
  Outcome stop(const Key& key, Reason reason, TimePoint now);
  Outcome retryCleanup(const Key& key, TimePoint now);
  bool cleaned(const Key& key, bool verified);
  Outcome showWindow(const Key& key);
  bool restorePending(Peer peer, const Key& owner, Role role, std::string displayId);
  void tick(TimePoint now);
private:
  bool matches(const Key& key) const;
  Reason preflight(const Peer& peer, Role role) const;
  Outcome begin(Peer peer, SessionId session, SessionId request, Role role, TimePoint now);
  void authorize(TimePoint now);
  void prepare(TimePoint now);
  void terminate(Reason reason, TimePoint now);
  void finishPending(Reason reason);
  void changed();
  void effect(EffectKind kind, Reason reason = Reason::None);
  Capabilities m_local;
  SessionState m_state;
  Snapshot m_snapshot;
  std::vector<Effect> m_effects;
  std::deque<SessionId> m_recentRequests;
  TimePoint m_deadline{};
  bool m_authorized = false;
  bool m_deferPreparation = false;
  bool m_preparing = false;
  bool m_prepared = false;
  bool m_peerPresented = false;
  uint64_t m_ownerGeneration = 0;
};
const char* reasonName(Reason reason);
const char* stateName(State state);
const char* roleName(Role role);
const char* availabilityName(Availability availability);
const char* actionName(Action action);
} // namespace deskflow::display
