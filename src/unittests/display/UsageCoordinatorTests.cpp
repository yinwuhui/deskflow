// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/UsageCoordinator.h"
#include <iostream>
#include <stdexcept>
using namespace deskflow::display;
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
SessionId id(unsigned value){SessionId result{};result[0]=static_cast<uint8_t>(value);return result;}
UsageCapabilities caps(){return {Availability::Supported,Availability::Supported,Availability::Supported,Availability::Supported,
 Availability::Supported,Availability::Supported,Availability::Supported,true};}
bool has(const std::vector<UsageEffect>& effects,UsageEffectKind kind){for(const auto& effect:effects)if(effect.kind==kind)return true;return false;}
UsageKey beginInput(UsageCoordinator& c){
 check(c.begin(UsageMode::Input,id(1)).accepted,"input begin");auto key=c.snapshot().key;
 c.inputStatus(Permission::Granted,"local");c.preflight(key,Reason::None);c.permitted(key,Reason::None);c.inputStarted(key,true);c.takeEffects();return key;
}
UsageKey beginExtended(UsageCoordinator& c){
 check(c.begin(UsageMode::Extended,id(1)).accepted,"extended begin");auto key=c.snapshot().key;
 c.preflight(key,Reason::None);c.permitted(key,Reason::None);c.displayStarted(key);c.takeEffects();return key;
}
void test(const std::string& name){
 UsageCoordinator c(std::string(64,'a'),caps());c.inputStatus(Permission::Granted,"local");
 if(name=="IdleSelection"){
  c.select(UsageMode::Extended);check(c.snapshot().selectedMode==UsageMode::Extended && c.snapshot().effectiveMode==UsageMode::None,"selection started usage");
  check(c.takeEffects().empty(),"selection asked permission");
 }else if(name=="KeepInputBeforeConsent"){
  beginInput(c);c.begin(UsageMode::Extended,id(2));auto key=c.snapshot().key;c.preflight(key,Reason::None);
  check(c.snapshot().effectiveMode==UsageMode::Input,"old input stopped before consent");
  check(!has(c.takeEffects(),UsageEffectKind::StopInput),"old route stopped during preflight");
  c.permitted(key,Reason::PeerRejected);check(c.snapshot().effectiveMode==UsageMode::Input && c.snapshot().selectedMode==UsageMode::Input,"refusal lost old mode");
 }else if(name=="BluetoothPreflightFailure"){
  beginInput(c);auto unsupported=caps();unsupported.networkAvailable=false;c.setCapabilities(unsupported);
  check(c.begin(UsageMode::Extended,id(2)).reason==Reason::NetworkUnavailable,"no LAN allowed video");
  check(c.snapshot().effectiveMode==UsageMode::Input && c.takeEffects().empty(),"preflight stopped Bluetooth input");
 }else if(name=="ReleaseBeforeDisplay"){
  beginInput(c);c.begin(UsageMode::Extended,id(2));auto key=c.snapshot().key;c.preflight(key,Reason::None);c.permitted(key,Reason::None);
  check(c.snapshot().stage==SwitchStage::ReleasingInputs,"missing input release barrier");
  check(!has(c.takeEffects(),UsageEffectKind::StartDisplay),"display created before release");
  c.inputsReleased(key,true);check(!has(c.takeEffects(),UsageEffectKind::StartDisplay),"display created before old route stopped");
  c.inputStopped(key,true);check(c.snapshot().effectiveMode==UsageMode::None,"both modes effective");
  check(has(c.takeEffects(),UsageEffectKind::StartDisplay),"no display start");c.displayStarted(key);
  check(c.snapshot().effectiveMode==UsageMode::Extended,"keyboard policy applied to display or forgotten");
 }else if(name=="ReleaseFailureBlocks"){
  beginInput(c);c.begin(UsageMode::Extended,id(2));auto key=c.snapshot().key;c.preflight(key,Reason::None);c.permitted(key,Reason::None);
  c.inputsReleased(key,false);check(c.snapshot().state==UsageState::CleanupRequired,"unconfirmed input release ignored");
  check(!c.begin(UsageMode::Extended,id(3)).accepted,"started with unconfirmed input");check(!has(c.takeEffects(),UsageEffectKind::StartDisplay),"unsafe display start");
 }else if(name=="RollbackOnlyAfterCleanup"){
  beginInput(c);c.begin(UsageMode::Extended,id(2));auto key=c.snapshot().key;c.preflight(key,Reason::None);c.permitted(key,Reason::None);
  c.inputsReleased(key,true);c.inputStopped(key,true);c.takeEffects();c.displayFailed(key,Reason::DecodeFailed,false);
  check(!has(c.takeEffects(),UsageEffectKind::RestoreInput),"restored before display cleanup");c.displayStopped(key,true);
  check(c.snapshot().stage==SwitchStage::CheckingRollback,"restored without checking authorization");
  c.rollbackAuthorized(key,true);check(c.snapshot().effectiveMode==UsageMode::None,"marked restored before route ack");
  c.inputStarted(key,true);check(c.snapshot().effectiveMode==UsageMode::Input&&c.snapshot().rollback==RollbackResult::Succeeded,"failed to restore exact old policy");
 }else if(name=="NoRollbackWhenRevoked"){
  beginInput(c);c.begin(UsageMode::Extended,id(2));auto key=c.snapshot().key;c.preflight(key,Reason::None);c.permitted(key,Reason::None);
  c.inputsReleased(key,true);c.inputStopped(key,true);c.displayFailed(key,Reason::PresentationFailed,true);c.takeEffects();c.rollbackAuthorized(key,false);
  check(c.snapshot().effectiveMode==UsageMode::None&&c.snapshot().rollback==RollbackResult::Unavailable,"restored expired authorization");
  check(!has(c.takeEffects(),UsageEffectKind::RestoreInput),"revoked input route restored");
 }else if(name=="DisplayBeforeInput"){
  beginExtended(c);c.begin(UsageMode::Input,id(2));auto key=c.snapshot().key;c.preflight(key,Reason::None);c.permitted(key,Reason::None);
  check(c.snapshot().effectiveMode==UsageMode::Extended,"display lost before cleanup confirmation");
  check(!has(c.takeEffects(),UsageEffectKind::StartInput),"input started before display removal");c.displayStopped(key,true);c.inputStarted(key,false);
  check(c.snapshot().effectiveMode==UsageMode::None&&c.snapshot().reason==Reason::SwitchFailed,"recreated old display after input failure");
  check(!has(c.takeEffects(),UsageEffectKind::StartDisplay),"reused old display consent");
 }else if(name=="CleanupBlocksInput"){
  beginExtended(c);c.begin(UsageMode::Input,id(2));auto key=c.snapshot().key;c.preflight(key,Reason::None);c.permitted(key,Reason::None);c.takeEffects();c.displayStopped(key,false);
  check(c.snapshot().state==UsageState::CleanupRequired&&!c.begin(UsageMode::Input,id(3)).accepted,"dirty display allowed input");
  check(!has(c.takeEffects(),UsageEffectKind::StartInput),"input started after failed cleanup");
 }else if(name=="StopCancelsRollback"){
  beginInput(c);c.begin(UsageMode::Extended,id(2));auto stale=c.snapshot().key;c.preflight(stale,Reason::None);c.permitted(stale,Reason::None);
  c.inputsReleased(stale,true);c.inputStopped(stale,true);c.displayFailed(stale,Reason::DecodeFailed,true);c.stop(id(3));auto key=c.snapshot().key;
  check(!c.rollbackAuthorized(stale,true)&&!c.inputStarted(stale,true),"late rollback reactivated after stop-all");
  c.displayStopped(key,true);check(c.snapshot().effectiveMode==UsageMode::None,"stop-all left usage active");
 }else if(name=="CompleteInputPermission"){
  c.inputStatus(Permission::Denied,"local");c.begin(UsageMode::Input,id(1));auto key=c.snapshot().key;
  c.preflight(key,Reason::None);c.takeEffects();c.permitted(key,Reason::None);
  check(c.snapshot().effectiveMode==UsageMode::None && c.snapshot().reason==Reason::InputPermissionDenied,"partial permission enabled input");
  check(!has(c.takeEffects(),UsageEffectKind::StartInput),"started without full input permissions");
 }else if(name=="UnknownCapabilities"){
  auto unknown=caps();unknown.keyboard=Availability::Unknown;c.setCapabilities(unknown);
  check(c.begin(UsageMode::Input,id(1)).reason==Reason::CapabilityCheckRequired,"unknown input-only advertised supported");
  unknown.keyboard=Availability::Unsupported;c.setCapabilities(unknown);
  check(c.begin(UsageMode::Input,id(1)).reason==Reason::InputCapabilityIncomplete,"missing explicit input-only failure");
 }else if(name=="StopDoesNotChangeMode"){
  beginExtended(c);c.stop(id(2));auto key=c.snapshot().key;c.displayStopped(key,true);
  check(c.snapshot().effectiveMode==UsageMode::None&&c.snapshot().selectedMode==UsageMode::Extended,"ending display silently switched to input");
  check(!has(c.takeEffects(),UsageEffectKind::StartInput),"user stop started input");
 }else if(name=="CancelDuringRelease"){
  beginInput(c);c.begin(UsageMode::Extended,id(2));auto key=c.snapshot().key;c.preflight(key,Reason::None);c.permitted(key,Reason::None);c.takeEffects();
  check(c.cancel(key).accepted,"cancel release rejected");c.inputsReleased(key,true);c.inputStopped(key,true);
  check(c.snapshot().stage==SwitchStage::CheckingRollback&&!has(c.takeEffects(),UsageEffectKind::StartDisplay),"cancel still started display");
 }else if(name=="FocusReleaseBarrier"){
  beginInput(c);c.inputStatus(Permission::Granted,"peer");c.begin(UsageMode::Extended,id(2));auto key=c.snapshot().key;
  c.preflight(key,Reason::None);c.permitted(key,Reason::None);c.takeEffects();c.inputsReleased(key,true);
  check(c.snapshot().state==UsageState::CleanupRequired,"accepted release without local focus");
  check(!has(c.takeEffects(),UsageEffectKind::StartDisplay),"started display with remote input focus");
 }else if(name=="CancelDisplayTeardown"){
  beginExtended(c);c.begin(UsageMode::Input,id(2));auto key=c.snapshot().key;c.preflight(key,Reason::None);c.permitted(key,Reason::None);c.takeEffects();
  check(c.cancel(key).accepted,"cancel rejected during teardown");c.displayStopped(key,true);
  check(c.snapshot().effectiveMode==UsageMode::None && !has(c.takeEffects(),UsageEffectKind::StartInput),"cancel enabled replacement input");
 }else if(name=="ConnectionLostDuringInputStart"){
  c.begin(UsageMode::Input,id(1));auto key=c.snapshot().key;c.preflight(key,Reason::None);c.permitted(key,Reason::None);c.takeEffects();c.connectionLost();
  check(has(c.takeEffects(),UsageEffectKind::RevokeInput),"pending input start was not revoked");
  check(!c.inputStarted(key,true),"late start survived disconnection");
 }else if(name=="CancelRollback"){
  beginInput(c);c.begin(UsageMode::Extended,id(2));auto key=c.snapshot().key;c.preflight(key,Reason::None);c.permitted(key,Reason::None);c.inputsReleased(key,true);c.inputStopped(key,true);
  c.displayFailed(key,Reason::DecodeFailed,true);c.takeEffects();check(c.cancel(key).accepted,"cancel rollback rejected");
  check(!c.rollbackAuthorized(key,true) && c.snapshot().effectiveMode==UsageMode::None,"cancelled rollback restored input");
 }else if(name=="ReleaseTimeout"){
  beginInput(c);c.begin(UsageMode::Extended,id(2));auto key=c.snapshot().key;c.preflight(key,Reason::None);c.permitted(key,Reason::None);c.takeEffects();
  c.tick(Coordinator::Clock::now()+std::chrono::seconds(6));
  check(c.snapshot().state==UsageState::CleanupRequired&&has(c.takeEffects(),UsageEffectKind::RevokeInput),"release timeout didn't revoke input");
 }else if(name=="OperationReplay"){
  beginInput(c);c.stop(id(2));auto key=c.snapshot().key;c.inputsReleased(key,true);c.inputStopped(key,true);
  check(!c.begin(UsageMode::Extended,id(1)).accepted,"reused old operationId");
 }else if(name=="InputStartTimeout"){
  c.begin(UsageMode::Input,id(1));auto key=c.snapshot().key;c.preflight(key,Reason::None);c.permitted(key,Reason::None);c.takeEffects();
  c.tick(Coordinator::Clock::now()+std::chrono::seconds(16));
  check(c.snapshot().state==UsageState::CleanupRequired&&has(c.takeEffects(),UsageEffectKind::RevokeInput),"pending input activation timed out without revocation");
  check(!c.inputStarted(key,true),"late start callback escaped cleanup barrier");
  check(!c.inputCleaned(key,false),"unverified cleanup acknowledged");
  check(c.inputCleaned(key,true)&&c.snapshot().effectiveMode==UsageMode::None&&c.snapshot().state==UsageState::Inactive,"verified route cleanup did not clear barrier");
 }else throw std::runtime_error("unknown case");
}
int main(int argc,char** argv){try{if(argc!=2)return 2;test(argv[1]);return 0;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
