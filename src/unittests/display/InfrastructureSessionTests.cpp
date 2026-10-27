// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/core/InfrastructureSession.h"
#include <functional>
#include <iostream>
#include <stdexcept>
using namespace deskflow::display::infra;
using namespace std::chrono_literals;
static void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
struct Fixture {
  TimePoint now{100s};
  InfrastructureSession session;
  CapabilitySnapshot sender, receiver;
  SelectionPolicy selection;
  Requirements requirements;
  SessionPlan selected;
  Fixture() {
    selection.allowUncertifiedDiagnostics = true;
    sender.peerIdentity = std::string(64,'a'); receiver.peerIdentity = std::string(64,'b');
    sender.revision = receiver.revision = 1;
    sender.environment = std::string(64,'c'); receiver.environment = std::string(64,'d');
    sender.directNetworkAllowed = receiver.directNetworkAllowed = true;
    sender.sessionAvailable = receiver.sessionAvailable = true;
    sender.cleanupVerified = receiver.cleanupVerified = true;
    PipelineProfile p;
    p.id = "test.webrtc.sender"; p.implementation = "test"; p.implementationVersion = "1";
    p.environment = sender.environment; p.readiness = Readiness::Available;
    p.security = {SecurityProfile::WebRtcAuthenticatedDtlsSrtp}; p.formats = {VideoFormat{}};
    p.rateFeedback = p.refreshFeedback = p.presentationEvidence = p.verifiedCleanup = true;
    sender.pipelines = {p}; p.id = "test.webrtc.receiver"; p.environment = receiver.environment;
    p.direction = Direction::Receive; receiver.pipelines = {p};
    selected = {std::string(64,'e'), std::string(32,'1'), 1, sender.peerIdentity, receiver.peerIdentity,
      1, 1, plan(sender,receiver,requirements,selection).candidates.front(), std::string(64,'f'), {}};
  }
  SessionToken token() const { return session.snapshot().token; }
  std::string owner() const { return token().owner; }
  void reserved() {
    require(session.reserve({sender.peerIdentity,receiver.peerIdentity,selected.controlBinding,selected.session,
      std::string(32,'2'),Direction::Send,1},now),"reserve rejected");
  }
  void negotiated() {
    reserved(); require(session.beginNegotiation(token(),now),"negotiate rejected");
    require(session.acceptPlan(token(),selected,std::string(64,'a'),sender,receiver,requirements,selection,now)
      == Rejection::None,"plan rejected");
  }
  void committed() {
    negotiated(); require(session.commit(token(),{true,true,true,true,true,now+60s},now),"commit rejected");
  }
  SessionToken dispatchedIntent() {
    committed(); auto t=token(); const auto e=session.takeEffects();
    require(e.size()==1 && e.front().kind==EffectKind::PersistIntent,"create before durable intent"); return t;
  }
  SessionToken dispatchedPreparation() {
    auto t=dispatchedIntent(); require(session.intentPersisted(t,true,now),"persist rejected");
    const auto e=session.takeEffects(); require(e.size()==1&&e.front().kind==EffectKind::PrepareBackend,"prepare effect missing"); return t;
  }
  void running() {
    auto t=dispatchedPreparation();require(session.attachWorker(t,std::string(32,'3'),now),"attach rejected");
    require(session.prepared(token(),now),"prepared rejected");session.takeEffects();
    require(session.presented(token(),PresentationEvidence::NativePaintConfirmed,true,now),"first frame rejected");
  }
};
static void authorizationAndDurability() {
  Fixture f; f.negotiated();
  require(!f.session.commit(f.token(),{true,true,false,true,true,f.now+60s},f.now),"unreleased input bypass");
  require(f.session.takeEffects().empty(),"resource effect before barriers");
  require(f.session.commit(f.token(),{true,true,true,true,true,f.now+60s},f.now),"valid commit rejected");
  require(!f.session.prepared(f.token(),f.now),"prepared before durable intent");
  require(!f.session.attachWorker(f.token(),std::string(32,'3'),f.now),"worker before durable intent");
  auto e=f.session.takeEffects();require(e.size()==1&&e.front().kind==EffectKind::PersistIntent,"intent missing");
}
static void presentationGate() {
  Fixture f;auto t=f.dispatchedPreparation();f.session.attachWorker(t,std::string(32,'3'),f.now);
  f.session.prepared(f.token(),f.now);f.session.takeEffects();
  require(!f.session.presented(f.token(),PresentationEvidence::Decoded,true,f.now),"decode treated as presentation");
  require(!f.session.presented(f.token(),PresentationEvidence::Submitted,true,f.now),"submit treated as presentation");
  require(!f.session.presented(f.token(),PresentationEvidence::NativePaintConfirmed,false,f.now),"hidden target accepted");
  require(f.session.presented(f.token(),PresentationEvidence::CompositorPresented,true,f.now),"native presentation rejected");
}
static void cancellationDuringPersistence() {
  Fixture f;auto old=f.dispatchedIntent();f.session.stop("Cancelled",f.now);
  require(f.session.snapshot().state==Lifecycle::Stopping,"in-flight durable write skipped");
  require(f.session.takeEffects().empty(),"destroy raced in-flight journal");
  require(f.session.intentPersisted(old,true,f.now+10ms),"old intent ownership completion lost");
  const auto e=f.session.takeEffects();
  require(e.size()==1&&e.front().kind==EffectKind::DestroyResources,"cancelled write started media");
  require(!f.session.cleanupEvidence(f.owner(),true,f.now),"cleanup accepted before destroy and verify");
  require(f.session.resourcesDestroyed(f.owner(),f.now),"destroy completion rejected");f.session.takeEffects();
  require(f.session.cleanupEvidence(f.owner(),true,f.now),"verified owner not released");
}
static void cancellationDuringSpawn() {
  Fixture f;auto old=f.dispatchedPreparation();f.session.stop("Cancelled",f.now);
  auto e=f.session.takeEffects();require(e.size()==1&&e.front().kind==EffectKind::CancelPreparation,"in-flight spawn not cancelled");
  require(!f.session.resourcesDestroyed(f.owner(),f.now),"destroy before spawn quiescence");
  require(f.session.attachWorker(old,std::string(32,'4'),f.now+20ms),"late worker lost for cleanup");
  require(!f.session.prepared(old,f.now),"old prepare activated cancelled session");
  e=f.session.takeEffects();require(e.size()==1&&e.front().kind==EffectKind::StopMedia,"late worker left alive");
  require(!f.session.workerQuiesced(f.owner(),std::string(32,'5'),f.now),"wrong worker cleanup accepted");
  require(f.session.workerQuiesced(f.owner(),std::string(32,'4'),f.now),"old instance cleanup rejected");
}
static void staleFramesAndCleanup() {
  Fixture f;f.running();const auto old=f.token();f.session.stop("ConnectionLost",f.now);
  f.session.takeEffects();
  require(!f.session.presented(old,PresentationEvidence::NativePaintConfirmed,true,f.now),"stale frame reactivated session");
  require(!f.session.cleanupEvidence(f.owner(),true,f.now),"worker death assumed from peer ACK");
  require(f.session.workerQuiesced(f.owner(),old.worker,f.now),"worker stop lost");f.session.takeEffects();
  require(f.session.resourcesDestroyed(f.owner(),f.now),"destroy lost");f.session.takeEffects();
  require(!f.session.cleanupEvidence(std::string(32,'8'),true,f.now),"unrelated owner released");
  f.session.cleanupEvidence(f.owner(),false,f.now);
  require(f.session.snapshot().state==Lifecycle::CleanupRequired,"failed verification cleared barrier");
  require(f.session.retryCleanup(f.now),"cleanup retry rejected");f.session.takeEffects();
  f.session.resourcesDestroyed(f.owner(),f.now);f.session.takeEffects();f.session.cleanupEvidence(f.owner(),true,f.now);
  require(f.session.snapshot().state==Lifecycle::Idle && f.session.snapshot().cleanupVerified,"verified cleanup incomplete");
  require(f.session.snapshot().primaryError=="ConnectionLost","cleanup replaced primary failure");
}
static void absoluteDeadlines() {
  Fixture f;auto old=f.dispatchedIntent();f.session.intentPersisted(old,true,f.now+9s);
  require(f.session.snapshot().state==Lifecycle::Preparing,"prepare rejected before deadline");
  f.session.takeEffects();f.session.tick(f.now+10s);
  require(f.session.snapshot().state==Lifecycle::Stopping,"startup deadline extended by progress");
  Fixture g;g.negotiated();g.session.tick(g.now+30s);
  require(g.session.snapshot().state==Lifecycle::Idle,"reservation not expired");
}
static void recoveryBudget() {
  Fixture f;f.running();require(f.session.beginRecovery(f.token(),f.now),"recovery rejected");
  f.session.takeEffects();require(f.session.retryRefresh(f.token(),f.now+1s),"second bounded refresh rejected");
  require(!f.session.retryRefresh(f.token(),f.now+2s),"unbounded refresh attempts");f.session.takeEffects();
  f.session.tick(f.now+3s);
  require(f.session.snapshot().state==Lifecycle::Stopping,"refresh renewed absolute deadline");
  require(f.session.snapshot().primaryError=="CongestionUnrecoverable","recovery cause lost");
}
static void revokedGrantDoesNotSkipCleanupBudget() {
  Fixture f;f.running();f.session.tick(f.now+60s);f.session.takeEffects();
  require(f.session.snapshot().state==Lifecycle::Stopping,"grant revocation failed");
  f.session.tick(f.now+61s);
  require(f.session.snapshot().state==Lifecycle::Stopping,"expired grant skipped safe stop budget");
  f.session.tick(f.now+65s);auto e=f.session.takeEffects();
  require(f.session.snapshot().state==Lifecycle::CleanupRequired,"cleanup timeout hidden");
  require(e.size()==1&&e.front().kind==EffectKind::TerminateWorker,"hung original worker not fenced");
}
static void queuedCreateCancelled() {
  Fixture f;f.committed();f.session.stop("Cancelled",f.now);auto e=f.session.takeEffects();
  require(e.size()==1&&e.front().kind==EffectKind::DestroyResources,"queued intent executed after cancellation");
  require(!f.session.intentPersisted(f.token(),true,f.now),"cancelled undelivered intent accepted");
}
int main(int argc,char** argv) {
  const std::vector<std::pair<std::string,std::function<void()>>> cases{
    {"AuthorizationDurability",authorizationAndDurability},{"PresentationEvidence",presentationGate},
    {"CancelDuringPersistence",cancellationDuringPersistence},{"CancelDuringSpawn",cancellationDuringSpawn},
    {"StaleFramesCleanup",staleFramesAndCleanup},{"AbsoluteDeadlines",absoluteDeadlines},
    {"RecoveryBudget",recoveryBudget},{"GrantRevocationCleanup",revokedGrantDoesNotSkipCleanupBudget},
    {"QueuedCreateCancelled",queuedCreateCancelled}};
  try {
    if(argc!=2)throw std::runtime_error("one test case required");
    for(const auto& [name,run]:cases)if(name==argv[1]){run();return 0;}
    throw std::runtime_error("unknown test case");
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
