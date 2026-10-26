// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "Coordinator.h"
#include <algorithm>
#include <cctype>
#include <limits>

namespace deskflow::display {
namespace {
bool identity(const std::string& id) {
  return id.size() == 64 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
bool name(const std::string& value) {
  return !value.empty() && value.size() <= 256 &&
    std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
}
Coordinator::Coordinator(Capabilities local) : m_local(local) { changed(); }

void Coordinator::changed() {
  if (m_snapshot.revision == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("Display snapshot revision exhausted");
  ++m_snapshot.revision;
  m_snapshot.state = m_state.state();
  m_snapshot.key.generation = m_state.generation();
  m_snapshot.actions.clear();
  switch (m_snapshot.state) {
  case State::Idle:
    if (m_local.sessionAvailable && (m_local.send == Availability::Supported || m_local.receive == Availability::Supported))
      m_snapshot.actions.push_back(Action::Start);
    break;
  case State::Connecting: m_snapshot.actions = {Action::Cancel}; break;
  case State::Authorizing:
    m_snapshot.actions = {Action::Cancel};
    if (m_snapshot.role == Role::Receiver && !m_authorized)
      m_snapshot.actions = {Action::Allow, Action::Reject};
    else if (m_authorized && m_snapshot.permission == Permission::Required)
      m_snapshot.actions.push_back(Action::OpenSystemSettings);
    break;
  case State::Preparing: m_snapshot.actions = {Action::Cancel}; break;
  case State::Streaming:
    m_snapshot.actions = {Action::Stop};
    if (m_snapshot.role == Role::Receiver) m_snapshot.actions.push_back(Action::ShowWindow);
    break;
  case State::CleanupRequired:
    m_snapshot.actions = {Action::RetryCleanup, Action::OpenSystemSettings}; break;
  default: break;
  }
}
bool Coordinator::matches(const Key& key) const { return key == m_snapshot.key && key.session != SessionId{}; }
void Coordinator::effect(EffectKind kind, Reason reason) {
  m_effects.push_back({kind, m_snapshot.key, reason, m_ownerGeneration});
}
std::vector<Effect> Coordinator::takeEffects() {
  auto result = std::move(m_effects); m_effects.clear(); return result;
}
void Coordinator::setCapabilities(Capabilities local, TimePoint now) {
  m_local = local;
  if (m_state.state() != State::Idle && m_state.state() != State::Stopping &&
      m_state.state() != State::CleanupRequired && !local.sessionAvailable)
    terminate(Reason::SessionUnavailable, now);
  else changed();
}
void Coordinator::setReceiveRequestsEnabled(bool enabled) {
  if (m_snapshot.receiveRequestsEnabled == enabled) return;
  m_snapshot.receiveRequestsEnabled = enabled;
  // Existing authorization/session is independent of the preference for new requests.
  changed();
}
Reason Coordinator::preflight(const Peer& peer, Role role) const {
  if (!identity(peer.id) || !name(peer.name)) return Reason::InvalidRequest;
  if (m_state.state() != State::Idle) return Reason::PeerBusy;
  if (m_local.protocol != 1 || peer.capabilities.protocol != 1) return Reason::UnsupportedPeerVersion;
  if (!m_local.sessionAvailable || !peer.capabilities.sessionAvailable) return Reason::SessionUnavailable;
  auto local = role == Role::Host ? m_local.send : m_local.receive;
  auto remote = role == Role::Host ? peer.capabilities.receive : peer.capabilities.send;
  if (local == Availability::Unknown || local == Availability::Checking ||
      remote == Availability::Unknown || remote == Availability::Checking) return Reason::CapabilityCheckRequired;
  if (local != Availability::Supported || remote != Availability::Supported) return Reason::UnsupportedPlatform;
  if (role == Role::Receiver && !m_snapshot.receiveRequestsEnabled) return Reason::PeerRejected;
  return Reason::None;
}
Outcome Coordinator::begin(Peer peer, SessionId session, SessionId request, Role role, TimePoint now) {
  if (auto reason = preflight(peer, role); reason != Reason::None) return {false, reason};
  if (session == SessionId{} || request == SessionId{} ||
      std::find(m_recentRequests.begin(), m_recentRequests.end(), request) != m_recentRequests.end())
    return {false, Reason::InvalidRequest};
  m_recentRequests.push_back(request);
  if (m_recentRequests.size() > 128) m_recentRequests.pop_front();
  const bool receiveEnabled = m_snapshot.receiveRequestsEnabled;
  const auto revision = m_snapshot.revision;
  m_snapshot = {};
  m_snapshot.receiveRequestsEnabled = receiveEnabled;
  m_snapshot.revision = revision;
  m_snapshot.key = {session, request, m_state.begin(), 1};
  m_snapshot.role = role;
  m_snapshot.peer = std::move(peer);
  m_snapshot.permission = role == Role::Receiver ? Permission::Granted : Permission::Unknown;
  m_authorized = m_preparing = m_prepared = m_peerPresented = m_deferPreparation = false;
  m_ownerGeneration = 0;
  m_deadline = now + std::chrono::seconds(30);
  if (role == Role::Receiver) m_state.advance(m_state.generation(), Event::Connected);
  else effect(EffectKind::SendRequest);
  changed();
  return {true, Reason::None};
}
Outcome Coordinator::request(Peer peer, SessionId session, SessionId request, Routing routing, TimePoint now, bool deferPreparation) {
  if (!deferPreparation && routing != Routing::Clear) return {false, Reason::InputConflict};
  auto outcome=begin(std::move(peer), session, request, Role::Host, now);
  if(outcome.accepted)m_deferPreparation=deferPreparation;
  return outcome;
}
Outcome Coordinator::incoming(Peer peer, SessionId session, SessionId request, TimePoint now) {
  return begin(std::move(peer), session, request, Role::Receiver, now);
}
void Coordinator::finishPending(Reason reason) {
  m_state.stop();
  m_state.advance(m_state.generation(), Event::CleanupSucceeded);
  m_snapshot.reason = m_snapshot.terminalReason = reason;
  m_snapshot.cleanup = Cleanup::Verified;
  changed();
}
Outcome Coordinator::respond(const Key& key, bool allow, TimePoint now) {
  if (!matches(key) || m_state.state() != State::Authorizing || m_snapshot.role != Role::Receiver || m_authorized)
    return {false, Reason::InvalidRequest};
  if (now >= m_deadline) { tick(now); return {false, Reason::RequestExpired}; }
  if (!allow) {
    effect(EffectKind::SendRejected, Reason::PeerRejected);
    finishPending(Reason::PeerRejected);
  } else {
    m_authorized = true;
    effect(EffectKind::SendAccepted);
    // Sender may still be in a system permission dialog. Do not start the
    // receiver first-frame timer until the authenticated sender is ready.
    changed();
  }
  return {true, Reason::None};
}
bool Coordinator::peerResponse(const Key& key, bool allow, Reason reason, TimePoint now) {
  if (!matches(key) || m_state.state() != State::Connecting || m_snapshot.role != Role::Host) return false;
  if (now >= m_deadline) { tick(now); return false; }
  if (!allow) { finishPending(reason == Reason::None ? Reason::PeerRejected : reason); return true; }
  m_state.advance(m_state.generation(), Event::Connected);
  m_authorized = true;
  authorize(now);
  return true;
}
void Coordinator::authorize(TimePoint now) {
  if (m_snapshot.permission == Permission::Granted) prepare(now);
  else {
    m_snapshot.permission = Permission::Required;
    m_snapshot.reason = Reason::PermissionRequired;
    changed();
  }
}
bool Coordinator::permission(const Key& key, Permission permissionValue, TimePoint now) {
  if (!matches(key) || m_snapshot.role != Role::Host || !m_authorized ||
      m_state.state() != State::Authorizing) return false;
  if (permissionValue != Permission::Granted && permissionValue != Permission::Denied) return false;
  m_snapshot.permission = permissionValue;
  if (permissionValue == Permission::Denied) terminate(Reason::PermissionDenied, now);
  else prepare(now);
  return true;
}
bool Coordinator::peerReady(const Key& key, TimePoint now) {
  if (!matches(key) || m_snapshot.role != Role::Receiver || !m_authorized ||
      m_state.state() != State::Authorizing) return false;
  prepare(now); return true;
}
Outcome Coordinator::resumePreparation(const Key& key, Routing routing, TimePoint now) {
  if(!matches(key)||!m_deferPreparation||m_snapshot.role!=Role::Host||!m_authorized||
      m_snapshot.permission!=Permission::Granted||m_state.state()!=State::Authorizing)return {false,Reason::InvalidRequest};
  if(routing!=Routing::Clear)return {false,Reason::InputConflict};
  m_snapshot.inputRoutingSuspended=true;m_deferPreparation=false;prepare(now);return {true,Reason::None};
}
void Coordinator::prepare(TimePoint now) {
  // Authorization is obtained while the old input route remains active. Only
  // the upper usage transaction can release this barrier after route shutdown.
  if(m_deferPreparation){m_snapshot.reason=Reason::None;changed();return;}

  m_state.advance(m_state.generation(), Event::Authorized);
  m_state.advance(m_state.generation(), Event::Agreed);
  m_snapshot.reason = Reason::None;
  m_snapshot.cleanup = Cleanup::Pending;
  m_preparing = true;
  m_ownerGeneration = m_state.generation();
  m_deadline = now + std::chrono::seconds(10);
  if (m_snapshot.role == Role::Host) effect(EffectKind::SendPrepare);
  effect(m_snapshot.role == Role::Host ? EffectKind::PrepareHost : EffectKind::PrepareReceiver);
  changed();
}
bool Coordinator::prepared(const Key& key, std::string ownedDisplayId, TimePoint now) {
  if (!matches(key) || m_state.state() != State::Preparing || m_prepared) return false;
  if (now >= m_deadline) { tick(now); return false; }
  if (m_snapshot.role == Role::Host && (ownedDisplayId.empty() || ownedDisplayId.size() > 256))
    return fail(key, Reason::DisplayCreationFailed, now);
  m_snapshot.virtualDisplayId = std::move(ownedDisplayId);
  m_prepared = true;
  if (m_peerPresented) m_state.advance(m_state.generation(), Event::FirstFrame);
  changed();
  return true;
}
bool Coordinator::presented(const Key& key, TimePoint now) {
  if (!matches(key) || m_state.state() != State::Preparing) return false;
  if (now >= m_deadline) { tick(now); return false; }
  m_peerPresented = m_snapshot.firstFramePresented = true;
  if (m_prepared) m_state.advance(m_state.generation(), Event::FirstFrame);
  changed();
  return true;
}
void Coordinator::terminate(Reason reason, TimePoint now) {
  if (m_state.state() == State::Idle || m_state.state() == State::Stopping || m_state.state() == State::CleanupRequired)
    return;
  effect(m_authorized ? EffectKind::StopPeer : EffectKind::CancelRequest, reason);
  if (!m_preparing) { finishPending(reason); return; }
  m_state.stop();
  m_snapshot.reason = m_snapshot.terminalReason = reason;
  m_snapshot.cleanup = Cleanup::Pending;
  changed();
  m_deadline = now + ((reason == Reason::UserStopped || reason == Reason::ApplicationExit || reason == Reason::Cancelled)
    ? std::chrono::seconds(5) : std::chrono::seconds(15));
  effect(EffectKind::CleanupResources, reason);
}
Outcome Coordinator::stop(const Key& key, Reason reason, TimePoint now) {
  if (!matches(key)) return {false, Reason::InvalidRequest};
  if (m_state.state() == State::Idle || m_state.state() == State::Stopping) return {true, Reason::None};
  if (m_state.state() == State::CleanupRequired) return {false, Reason::CleanupFailed};
  if (reason != Reason::UserStopped && reason != Reason::Cancelled && reason != Reason::ApplicationExit)
    return {false, Reason::InvalidRequest};
  terminate(reason, now);
  return {true, Reason::None};
}
bool Coordinator::fail(const Key& key, Reason reason, TimePoint now) {
  if (!matches(key) || m_state.state() == State::Idle || m_state.state() == State::Stopping ||
      m_state.state() == State::CleanupRequired || reason == Reason::None) return false;
  terminate(reason, now);
  return true;
}
Outcome Coordinator::retryCleanup(const Key& key, TimePoint now) {
  if (!matches(key) || m_state.state() != State::CleanupRequired) return {false, Reason::InvalidRequest};
  m_state.stop();
  m_snapshot.cleanup = Cleanup::Pending;
  m_deadline = now + std::chrono::seconds(5);
  changed(); effect(EffectKind::CleanupResources, m_snapshot.terminalReason);
  return {true, Reason::None};
}
bool Coordinator::cleaned(const Key& key, bool verified) {
  if (!matches(key) || (m_state.state() != State::Stopping && m_state.state() != State::CleanupRequired)) return false;
  m_state.advance(m_state.generation(), verified ? Event::CleanupSucceeded : Event::CleanupFailed);
  m_snapshot.cleanup = verified ? Cleanup::Verified : Cleanup::Required;
  m_snapshot.reason = verified ? m_snapshot.terminalReason : Reason::CleanupFailed;
  if (verified) {
    m_snapshot.virtualDisplayId.clear(); m_snapshot.confirmedLayout.reset();
    m_snapshot.firstFramePresented = false;
  }
  changed(); return true;
}
bool Coordinator::adapterStateChanged(const Key& key){
  if(!matches(key)||(m_state.state()!=State::Preparing&&m_state.state()!=State::Streaming))return false;
  changed();return true;
}
bool Coordinator::windowState(const Key& key,std::string screen,bool fullscreen){
  if(!matches(key)||m_state.state()!=State::Streaming||m_snapshot.role!=Role::Receiver||screen.empty()||screen.size()>256)return false;
  m_snapshot.receiverScreenId=std::move(screen);m_snapshot.fullscreen=fullscreen;changed();return true;
}
Outcome Coordinator::showWindow(const Key& key) {
  if (!matches(key) || m_state.state() != State::Streaming || m_snapshot.role != Role::Receiver)
    return {false, Reason::InvalidRequest};
  effect(EffectKind::ShowWindow); return {true, Reason::None};
}
bool Coordinator::restorePending(Peer peer, const Key& owner, Role role, std::string displayId) {
  if (m_state.state() != State::Idle || !identity(peer.id) || owner.session == SessionId{} ||
      owner.request == SessionId{} || !owner.generation || !owner.epoch ||
      (role != Role::Host && role != Role::Receiver) || displayId.size() > 256) return false;
  m_snapshot.peer = std::move(peer); m_snapshot.role = role;
  m_snapshot.key = owner; m_state.begin(); m_state.stop();
  m_state.advance(m_state.generation(), Event::CleanupFailed);
  m_ownerGeneration = owner.generation; m_preparing = true;
  m_snapshot.virtualDisplayId = std::move(displayId);
  m_snapshot.cleanup = Cleanup::Required; m_snapshot.reason = Reason::CleanupFailed;
  m_snapshot.terminalReason = Reason::ApplicationExit;
  changed(); return true;
}
void Coordinator::tick(TimePoint now) {
  if (now < m_deadline) return;
  switch (m_state.state()) {
  case State::Connecting:
  case State::Authorizing:
    if (!m_authorized) {
      effect(m_snapshot.role == Role::Receiver ? EffectKind::SendRejected : EffectKind::CancelRequest, Reason::RequestExpired);
      finishPending(Reason::RequestExpired);
    }
    break;
  case State::Preparing: terminate(Reason::PresentationFailed, now); break;
  case State::Stopping: cleaned(m_snapshot.key, false); break;
  default: break;
  }
}

const char* reasonName(Reason reason) {
#define ENTRY(x) case Reason::x: return #x
  switch(reason) {
    ENTRY(None); ENTRY(CapabilityCheckRequired); ENTRY(UnsupportedPlatform); ENTRY(UnsupportedPeerVersion); ENTRY(PeerBusy); ENTRY(PeerRejected);
    ENTRY(RequestExpired); ENTRY(Cancelled); ENTRY(PermissionRequired); ENTRY(PermissionDenied); ENTRY(SessionUnavailable);
    ENTRY(NetworkUnavailable); ENTRY(IdentityChanged); ENTRY(InputConflict); ENTRY(DisplayCreationFailed);
    ENTRY(LayoutRejected); ENTRY(DecodeFailed); ENTRY(PresentationFailed); ENTRY(Congestion); ENTRY(ConnectionLost);
    ENTRY(UserStopped); ENTRY(ApplicationExit); ENTRY(CleanupFailed); ENTRY(InvalidRequest);
    ENTRY(InputCapabilityIncomplete); ENTRY(InputIsolationUnsupported); ENTRY(InputReleaseFailed);
    ENTRY(InputPermissionDenied); ENTRY(ModeConflict); ENTRY(SwitchFailed); ENTRY(RollbackFailed);
  }
#undef ENTRY
  return "InvalidRequest";
}
const char* stateName(State state) {
#define ENTRY(x) case State::x: return #x
  switch(state) {
    ENTRY(Idle); ENTRY(Connecting); ENTRY(Authorizing); ENTRY(Negotiating); ENTRY(Preparing); ENTRY(Streaming);
    ENTRY(Reconfiguring); ENTRY(Recovering); ENTRY(Stopping); ENTRY(CleanupRequired);
  }
#undef ENTRY
  return "Idle";
}
const char* roleName(Role role) {
  switch(role) { case Role::None:return "none"; case Role::Host:return "host"; case Role::Receiver:return "receiver"; }
  return "none";
}
const char* availabilityName(Availability value) {
  switch(value) { case Availability::Unknown:return "unknown"; case Availability::Checking:return "checking";
    case Availability::Supported:return "supported"; case Availability::Unsupported:return "unsupported"; }
  return "unknown";
}
const char* actionName(Action action) {
#define ENTRY(x) case Action::x: return #x
  switch(action) { ENTRY(Start); ENTRY(Allow); ENTRY(Reject); ENTRY(Cancel); ENTRY(Stop); ENTRY(ShowWindow);
    ENTRY(RetryCleanup); ENTRY(OpenSystemSettings); }
#undef ENTRY
  return "Cancel";
}
} // namespace deskflow::display
