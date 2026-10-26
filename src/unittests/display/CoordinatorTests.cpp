// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/Coordinator.h"
#include <iostream>
#include <stdexcept>
#include <string>
using namespace deskflow::display;
using namespace std::chrono_literals;
void check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
Capabilities capabilities() { return {Availability::Supported, Availability::Supported, false, false, true, 1}; }
SessionId id(unsigned value) { SessionId result{}; result[0] = static_cast<uint8_t>(value); return result; }
Peer peer() { return {std::string(64, 'a'), "Ubuntu", capabilities()}; }
const auto now = Coordinator::TimePoint{} + 1h;
Key outgoing(Coordinator& c) {
  check(c.request(peer(), id(1), id(2), Routing::Clear, now).accepted, "start");
  return c.snapshot().key;
}
Key preparing(Coordinator& c) {
  auto key = outgoing(c);
  check(c.peerResponse(key, true, Reason::None, now), "peer response");
  check(c.permission(key, Permission::Granted, now), "permission");
  return c.snapshot().key;
}
bool has(const std::vector<Effect>& effects, EffectKind kind) {
  for (auto& effect : effects) if (effect.kind == kind) return true;
  return false;
}
void test(const std::string& name) {
  Coordinator c(capabilities());
  if (name == "ConsentBeforeResources") {
    auto key = outgoing(c);
    check(!has(c.takeEffects(), EffectKind::PrepareHost), "created before consent");
    c.peerResponse(key, true, Reason::None, now);
    check(c.snapshot().state == State::Authorizing, "permission must wait");
    check(!has(c.takeEffects(), EffectKind::PrepareHost), "created before system permission");
    c.permission(key, Permission::Granted, now);
    check(has(c.takeEffects(), EffectKind::PrepareHost), "missing prepare");
  } else if (name == "ExpiryAndLateConsent") {
    c.incoming(peer(), id(1), id(2), now); auto key = c.snapshot().key;
    c.tick(now + 30s);
    check(c.snapshot().state == State::Idle && c.snapshot().reason == Reason::RequestExpired, "expiry");
    check(!c.respond(key, true, now + 31s).accepted, "late allow accepted");
    check(!has(c.takeEffects(), EffectKind::PrepareReceiver), "late resource creation");
  } else if (name == "PermissionWaitPaused") {
    auto key = outgoing(c); c.peerResponse(key, true, Reason::None, now + 1s);
    c.tick(now + 5min);
    check(c.snapshot().state == State::Authorizing, "permission wait expired");
    check(c.permission(key, Permission::Granted, now + 5min), "permission continuation");
    c.tick(now + 5min + 10s);
    check(c.snapshot().state == State::Stopping && c.snapshot().reason == Reason::PresentationFailed, "first frame timeout");
  } else if (name == "ReceiverPermissionBarrier") {
    c.incoming(peer(), id(1), id(2), now); auto key = c.snapshot().key;
    c.respond(key, true, now);
    c.tick(now + 5min);
    check(c.snapshot().state == State::Authorizing, "receiver timed out while sender awaited permission");
    check(!has(c.takeEffects(), EffectKind::PrepareReceiver), "receiver prepared before sender permission");
    check(c.peerReady(key, now + 5min), "sender permission completion ignored");
    check(has(c.takeEffects(), EffectKind::PrepareReceiver), "receiver not prepared");
  } else if (name == "PresentationGate") {
    auto key = preparing(c); c.prepared(key, "display-owned", now);
    check(c.snapshot().state == State::Preparing, "prepared is not presented");
    check(!c.snapshot().firstFramePresented, "false presentation");
    c.presented(key, now + 1s);
    check(c.snapshot().state == State::Streaming, "not streaming after presentation");
  } else if (name == "PresentationRace") {
    auto key = preparing(c); c.presented(key, now);
    check(c.snapshot().state == State::Preparing, "must wait for resource setup");
    c.prepared(key, "display-owned", now);
    check(c.snapshot().state == State::Streaming, "presentation race failed");
  } else if (name == "CancelGeneration") {
    auto key = preparing(c); c.takeEffects();
    c.stop(key, Reason::Cancelled, now);
    check(!c.prepared(key, "late-output", now), "stale creation callback accepted");
    check(!c.presented(key, now), "stale presentation callback accepted");
    auto effects = c.takeEffects();
    check(effects.back().kind == EffectKind::CleanupResources && effects.back().ownerGeneration == key.generation,
      "resource ownership lost when callbacks invalidated");
    auto cleanupKey = c.snapshot().key; c.cleaned(cleanupKey, true);
    c.request(peer(), id(3), id(4), Routing::Clear, now);
    check(!c.cleaned(cleanupKey, false), "old cleanup poisoned new request");
  } else if (name == "CleanupRequired") {
    auto key = preparing(c); c.prepared(key, "display-owned", now);
    c.stop(key, Reason::UserStopped, now); auto cleanupKey = c.snapshot().key;
    c.tick(now + 5s);
    check(c.snapshot().state == State::CleanupRequired && c.snapshot().virtualDisplayId == "display-owned", "lost residual ownership");
    check(!c.request(peer(), id(3), id(4), Routing::Clear, now).accepted, "created while dirty");
    c.retryCleanup(cleanupKey, now + 6s); auto retryKey = c.snapshot().key;
    check(!c.cleaned(cleanupKey, true), "old cleanup allowed after retry");
    c.cleaned(retryKey, true);
    check(c.snapshot().state == State::Idle && c.snapshot().virtualDisplayId.empty(), "retry failed");
    check(c.snapshot().terminalReason == Reason::UserStopped, "terminal reason lost");
  } else if (name == "BusyAndPreference") {
    auto key = preparing(c); c.prepared(key, "display-owned", now); c.presented(key, now);
    c.setReceiveRequestsEnabled(false);
    check(c.snapshot().state == State::Streaming, "preference stopped existing session");
    check(!c.incoming(peer(), id(3), id(4), now).accepted, "concurrent opposite role");
    c.stop(key, Reason::UserStopped, now); c.cleaned(c.snapshot().key, true);
    check(!c.incoming(peer(), id(3), id(4), now).accepted, "disabled receiving accepted request");
    check(c.request(peer(), id(3), id(4), Routing::Clear, now).accepted, "receiving preference disabled sending");
  } else if (name == "InputConflict") {
    for (auto status : {Routing::Unknown, Routing::ConflictingPeer, Routing::UnverifiedOtherRoutes})
      check(c.request(peer(), id(1), id(2), status, now).reason == Reason::InputConflict, "unsafe route allowed");
    check(c.snapshot().state == State::Idle && c.takeEffects().empty(), "conflict created resource");
  } else if (name == "IdentityAndCapability") {
    auto bad = peer(); bad.id = "Ubuntu";
    check(!c.incoming(bad, id(1), id(2), now).accepted, "name trusted as identity");
    bad = peer(); bad.capabilities.protocol = 2;
    check(c.incoming(bad, id(1), id(2), now).reason == Reason::UnsupportedPeerVersion, "version ignored");
    bad = peer(); bad.capabilities.send = Availability::Unknown;
    check(!c.incoming(bad, id(1), id(2), now).accepted, "unknown capability treated supported");
    c.incoming(peer(), id(1), id(2), now); c.respond(c.snapshot().key, false, now);
    check(!c.incoming(peer(), id(3), id(2), now).accepted, "replayed request accepted");
  } else if (name == "LockDuringSession") {
    auto key = preparing(c); c.prepared(key, "display-owned", now); c.presented(key, now);
    auto unavailable = capabilities(); unavailable.sessionAvailable = false;
    c.setCapabilities(unavailable, now);
    check(c.snapshot().state == State::Stopping && c.snapshot().reason == Reason::SessionUnavailable, "lock did not stop");
    c.tick(now + 15s);
    check(c.snapshot().state == State::CleanupRequired, "abnormal absolute cleanup deadline");
    c.cleaned(c.snapshot().key, true); c.setCapabilities(capabilities(), now + 16s);
    check(c.snapshot().state == State::Idle, "unlock restarted session");
  } else if (name == "DirectionalCapabilities") {
    auto sender=capabilities();sender.receive=Availability::Unsupported;
    auto receiver=peer();receiver.capabilities.send=Availability::Unsupported;
    Coordinator host(sender);
    check(host.request(receiver,id(1),id(2),Routing::Clear,now).accepted,"receiver dependencies blocked sender");
    auto key=host.snapshot().key;host.peerResponse(key,true,Reason::None,now);
    check(host.snapshot().permission!=Permission::Granted,"capture permission was assumed");
    check(!has(host.takeEffects(),EffectKind::PrepareHost),"capture before permission");
    auto receiveOnly=capabilities();receiveOnly.send=Availability::Unsupported;
    Coordinator sink(receiveOnly);auto source=peer();source.capabilities.receive=Availability::Unsupported;
    check(sink.incoming(source,id(3),id(4),now).accepted,"sender dependencies blocked receiver");
    auto unknown=capabilities();unknown.sessionAvailable=false;unknown.sessionReason=Reason::CapabilityCheckRequired;
    Coordinator uncertain(unknown);
    check(uncertain.request(peer(),id(5),id(6),Routing::Clear,now).reason==Reason::CapabilityCheckRequired,"unknown desktop treated as unsupported");
  } else if (name == "NoInputOrTelemetry") {
    auto key = preparing(c); c.prepared(key, "display-owned", now); c.presented(key, now);
    check(!c.snapshot().inputEnabled && !c.snapshot().inputRoutingSuspended, "invented input support");
    check(!c.snapshot().confirmedLayout.has_value(), "invented layout");
    check(!c.showWindow(key).accepted, "host cannot show receiver window");
  } else throw std::runtime_error("unknown case");
}
int main(int argc, char** argv) {
  try { if (argc != 2) return 2; test(argv[1]); return 0; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
