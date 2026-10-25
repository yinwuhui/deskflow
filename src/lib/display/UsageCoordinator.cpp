// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "UsageCoordinator.h"
#include <limits>
#include <algorithm>
namespace deskflow::display {
namespace {
Reason support(Availability value, Reason unsupported) {
  return value==Availability::Supported?Reason::None:
    value==Availability::Unsupported?unsupported:Reason::CapabilityCheckRequired;
}
}
UsageCoordinator::UsageCoordinator(std::string peerId,UsageCapabilities capabilities):m_capabilities(capabilities) {
  m_snapshot.peerId=std::move(peerId);changed();
}
bool UsageCoordinator::matches(const UsageKey& key)const{return key==m_snapshot.key && key.operation!=SessionId{};}
void UsageCoordinator::changed(){
  if(m_snapshot.revision==std::numeric_limits<uint64_t>::max())throw std::overflow_error("Usage revision exhausted");
  ++m_snapshot.revision;
  if(m_timedKey!=m_snapshot.key||m_timedStage!=m_snapshot.stage){
    m_timedKey=m_snapshot.key;m_timedStage=m_snapshot.stage;
    auto seconds=m_snapshot.stage==SwitchStage::ReleasingInputs||m_snapshot.stage==SwitchStage::StoppingInput?5:15;
    m_deadline=Coordinator::Clock::now()+std::chrono::seconds(seconds);
  }
}
void UsageCoordinator::effect(UsageEffectKind kind){m_effects.push_back({kind,m_snapshot.key});}
std::vector<UsageEffect> UsageCoordinator::takeEffects(){auto effects=std::move(m_effects);m_effects.clear();return effects;}
Outcome UsageCoordinator::select(UsageMode mode){
  if(mode==UsageMode::None)return {false,Reason::InvalidRequest};
  if(m_snapshot.state==UsageState::Switching||m_snapshot.state==UsageState::CleanupRequired)return {false,Reason::ModeConflict};
  if(m_snapshot.effectiveMode!=UsageMode::None && m_snapshot.effectiveMode!=mode)return {false,Reason::ModeConflict};
  m_snapshot.selectedMode=mode;changed();return {true,Reason::None};
}
Outcome UsageCoordinator::check(UsageMode mode)const{
  if(mode==UsageMode::Input){
    if(auto r=support(m_capabilities.input,Reason::UnsupportedPlatform);r!=Reason::None)return {false,r};
    if(auto r=support(m_capabilities.focusSync,Reason::InputCapabilityIncomplete);r!=Reason::None)return {false,r};
    if(auto r=support(m_capabilities.keyboard,Reason::InputCapabilityIncomplete);r!=Reason::None)return {false,r};
  }else if(mode==UsageMode::Extended){
    if(!m_capabilities.networkAvailable)return {false,Reason::NetworkUnavailable};
    if(auto r=support(m_capabilities.extended,Reason::UnsupportedPlatform);r!=Reason::None)return {false,r};
    if(auto r=support(m_capabilities.displayRouting,Reason::InputIsolationUnsupported);r!=Reason::None)return {false,r};
  }else return {false,Reason::InvalidRequest};
  if(m_snapshot.effectiveMode==UsageMode::Input || mode==UsageMode::Input){
    if(auto r=support(m_capabilities.targetIsolation,Reason::InputIsolationUnsupported);r!=Reason::None)return {false,r};
    if(auto r=support(m_capabilities.inputRelease,Reason::InputReleaseFailed);r!=Reason::None)return {false,r};
  }
  return {true,Reason::None};
}
Outcome UsageCoordinator::begin(UsageMode mode,SessionId operation){
  if(m_snapshot.state==UsageState::Switching||m_snapshot.state==UsageState::CleanupRequired)return {false,Reason::ModeConflict};
  if(operation==SessionId{}||std::find(m_recentOperations.begin(),m_recentOperations.end(),operation)!=m_recentOperations.end())return {false,Reason::InvalidRequest};
  auto outcome=check(mode);if(!outcome.accepted)return outcome;
  if(m_snapshot.effectiveMode==mode)return {true,Reason::None};
  if(m_snapshot.key.generation==std::numeric_limits<uint64_t>::max())throw std::overflow_error("Usage generation exhausted");
  m_original=m_snapshot.effectiveMode;
  m_allowRollback=m_original==UsageMode::Input&&mode==UsageMode::Extended;
  m_stopAll=m_cancelRequested=false;
  m_recentOperations.push_back(operation);if(m_recentOperations.size()>128)m_recentOperations.pop_front();
  m_snapshot.key={operation,m_snapshot.key.generation+1};m_snapshot.selectedMode=mode;m_snapshot.targetMode=mode;
  m_snapshot.state=UsageState::Switching;m_snapshot.stage=SwitchStage::Preflight;
  m_snapshot.oldModeStopped=m_original==UsageMode::None;m_snapshot.reason=Reason::None;m_snapshot.rollback=RollbackResult::NotNeeded;
  effect(mode==UsageMode::Input?UsageEffectKind::PreflightInput:UsageEffectKind::PreflightDisplay);changed();return {true,Reason::None};
}
bool UsageCoordinator::preflight(const UsageKey& key,Reason result){
  if(!matches(key)||m_snapshot.stage!=SwitchStage::Preflight)return false;
  if(result!=Reason::None){complete(m_original,result);return true;}
  m_snapshot.stage=SwitchStage::AwaitingPermission;
  effect(m_snapshot.targetMode==UsageMode::Input?UsageEffectKind::RequestInputPermission:UsageEffectKind::RequestDisplayPermission);
  changed();return true;
}
bool UsageCoordinator::permitted(const UsageKey& key,Reason result){
  if(!matches(key)||m_snapshot.stage!=SwitchStage::AwaitingPermission)return false;
  if(result!=Reason::None){complete(m_original,result);return true;}
  if(m_snapshot.targetMode==UsageMode::Input && m_snapshot.inputPermission!=Permission::Granted){complete(m_original,Reason::InputPermissionDenied);return true;}
  afterPermission();return true;
}
void UsageCoordinator::afterPermission(){
  if(m_original==UsageMode::Input){
    m_snapshot.stage=SwitchStage::ReleasingInputs;m_snapshot.pressedInputsReleased=false;
    effect(UsageEffectKind::ReleaseInputs);
  }else if(m_original==UsageMode::Extended){m_snapshot.stage=SwitchStage::StoppingDisplay;effect(UsageEffectKind::StopDisplay);}
  else {m_snapshot.stage=m_snapshot.targetMode==UsageMode::Input?SwitchStage::StartingInput:SwitchStage::StartingDisplay;
    effect(m_snapshot.targetMode==UsageMode::Input?UsageEffectKind::StartInput:UsageEffectKind::StartDisplay);}
  changed();
}
bool UsageCoordinator::inputsReleased(const UsageKey& key,bool verified){
  if(!matches(key)||m_snapshot.state==UsageState::CleanupRequired||m_snapshot.stage!=SwitchStage::ReleasingInputs)return false;
  if(verified && m_snapshot.inputFocusOwner!="local")verified=false;
  m_snapshot.pressedInputsReleased=verified;
  if(!verified){m_snapshot.state=UsageState::CleanupRequired;m_snapshot.reason=Reason::InputReleaseFailed;m_allowRollback=false;effect(UsageEffectKind::RevokeInput);}
  else {m_snapshot.stage=SwitchStage::StoppingInput;effect(UsageEffectKind::StopInput);}
  changed();return true;
}
bool UsageCoordinator::inputStopped(const UsageKey& key,bool verified){
  if(!matches(key)||m_snapshot.state==UsageState::CleanupRequired||m_snapshot.stage!=SwitchStage::StoppingInput)return false;
  if(!verified){m_snapshot.state=UsageState::CleanupRequired;m_snapshot.reason=Reason::InputReleaseFailed;m_allowRollback=false;effect(UsageEffectKind::RevokeInput);changed();return true;}
  m_snapshot.effectiveMode=UsageMode::None;m_snapshot.oldModeStopped=true;
  if(m_stopAll){complete(UsageMode::None,m_snapshot.reason);return true;}
  if(m_cancelRequested){m_snapshot.reason=Reason::Cancelled;rollbackOrEnd();return true;}
  m_snapshot.stage=SwitchStage::StartingDisplay;effect(UsageEffectKind::StartDisplay);changed();return true;
}
bool UsageCoordinator::inputCleaned(const UsageKey& key,bool verified){
  if(!matches(key)||m_snapshot.state!=UsageState::CleanupRequired||
    (m_snapshot.stage!=SwitchStage::ReleasingInputs&&m_snapshot.stage!=SwitchStage::StoppingInput&&
     m_snapshot.stage!=SwitchStage::StartingInput&&m_snapshot.stage!=SwitchStage::RestoringInput))return false;
  if(!verified||m_snapshot.inputFocusOwner!="local")return false;
  m_allowRollback=false;m_stopAll=true;m_snapshot.pressedInputsReleased=true;m_snapshot.oldModeStopped=true;
  complete(UsageMode::None,m_snapshot.reason);return true;
}
bool UsageCoordinator::displayStarted(const UsageKey& key){
  if(!matches(key)||m_snapshot.stage!=SwitchStage::StartingDisplay||m_stopAll)return false;
  complete(UsageMode::Extended);return true;
}
bool UsageCoordinator::displayEnded(const UsageKey& key,Reason reason,bool verified){
  if(!matches(key)||m_snapshot.state!=UsageState::DisplayActive)return false;
  m_allowRollback=false;m_stopAll=true;
  if(verified)complete(UsageMode::None,reason);
  else{m_snapshot.state=UsageState::CleanupRequired;m_snapshot.stage=SwitchStage::CleaningDisplay;m_snapshot.reason=Reason::CleanupFailed;changed();}
  return true;
}
void UsageCoordinator::complete(UsageMode mode,Reason reason){
  m_snapshot.effectiveMode=mode;m_snapshot.selectedMode=mode==UsageMode::None?m_snapshot.selectedMode:mode;
  m_snapshot.state=mode==UsageMode::Input?UsageState::InputActive:mode==UsageMode::Extended?UsageState::DisplayActive:UsageState::Inactive;
  m_snapshot.targetMode=UsageMode::None;m_snapshot.stage=SwitchStage::None;m_snapshot.reason=reason;
  changed();
}
void UsageCoordinator::failedAfterStop(Reason reason,bool verified){
  m_snapshot.reason=reason;
  if(!verified){m_snapshot.state=UsageState::CleanupRequired;m_snapshot.stage=SwitchStage::CleaningDisplay;m_allowRollback=false;changed();return;}
  m_snapshot.effectiveMode=UsageMode::None;rollbackOrEnd();
}
void UsageCoordinator::rollbackOrEnd(){
  if(m_allowRollback&&!m_stopAll){m_snapshot.rollback=RollbackResult::Pending;m_snapshot.stage=SwitchStage::CheckingRollback;effect(UsageEffectKind::VerifyRollbackAuthorization);changed();}
  else complete(UsageMode::None,m_snapshot.reason);
}
bool UsageCoordinator::displayFailed(const UsageKey& key,Reason reason,bool verified){
  if(!matches(key)||m_snapshot.stage!=SwitchStage::StartingDisplay)return false;
  if(!verified){m_snapshot.reason=reason;m_snapshot.stage=SwitchStage::CleaningDisplay;effect(UsageEffectKind::StopDisplay);changed();}
  else failedAfterStop(reason,true);
  return true;
}
bool UsageCoordinator::displayStopped(const UsageKey& key,bool verified){
  if(!matches(key)||(m_snapshot.stage!=SwitchStage::StoppingDisplay&&m_snapshot.stage!=SwitchStage::CleaningDisplay))return false;
  if(!verified){m_snapshot.state=UsageState::CleanupRequired;m_snapshot.reason=Reason::CleanupFailed;m_allowRollback=false;changed();return true;}
  m_snapshot.effectiveMode=UsageMode::None;m_snapshot.oldModeStopped=true;
  if(m_stopAll){complete(UsageMode::None,m_snapshot.reason);return true;}
  if(m_snapshot.stage==SwitchStage::CleaningDisplay){rollbackOrEnd();return true;}
  if(m_cancelRequested){complete(UsageMode::None,Reason::Cancelled);return true;}
  m_snapshot.stage=SwitchStage::StartingInput;effect(UsageEffectKind::StartInput);changed();return true;
}
bool UsageCoordinator::inputStarted(const UsageKey& key,bool verified){
  if(!matches(key)||m_snapshot.state==UsageState::CleanupRequired||(m_snapshot.stage!=SwitchStage::StartingInput&&m_snapshot.stage!=SwitchStage::RestoringInput)||m_stopAll)return false;
  const bool rollback=m_snapshot.stage==SwitchStage::RestoringInput;
  if(verified && (m_snapshot.inputPermission!=Permission::Granted || m_snapshot.inputFocusOwner!="local" || !check(UsageMode::Input).accepted))verified=false;
  if(rollback)m_snapshot.rollback=verified?RollbackResult::Succeeded:RollbackResult::Failed;
  if(!verified){
    m_snapshot.effectiveMode=UsageMode::None;m_snapshot.state=UsageState::CleanupRequired;
    m_snapshot.reason=rollback?Reason::RollbackFailed:Reason::SwitchFailed;m_allowRollback=false;
    effect(UsageEffectKind::RevokeInput);changed();return true;
  }
  complete(UsageMode::Input,rollback?m_snapshot.reason:Reason::None);return true;
}
bool UsageCoordinator::rollbackAuthorized(const UsageKey& key,bool valid){
  if(!matches(key)||m_snapshot.stage!=SwitchStage::CheckingRollback||m_stopAll)return false;
  if(!valid || m_snapshot.inputPermission!=Permission::Granted){m_snapshot.rollback=RollbackResult::Unavailable;complete(UsageMode::None,m_snapshot.reason);return true;}
  m_snapshot.stage=SwitchStage::RestoringInput;effect(UsageEffectKind::RestoreInput);changed();return true;
}
Outcome UsageCoordinator::cancel(const UsageKey& key){
  if(!matches(key)||m_snapshot.state!=UsageState::Switching)return {false,Reason::InvalidRequest};
  if(m_snapshot.stage==SwitchStage::Preflight||m_snapshot.stage==SwitchStage::AwaitingPermission){
    effect(UsageEffectKind::CancelDisplayRequest);complete(m_original,Reason::Cancelled);return {true,Reason::None};
  }
  // If input release/stop is in flight, finish that barrier before any rollback.
  // Explicit stop-all uses a new generation and permanently cancels restoration.
  if(m_snapshot.stage==SwitchStage::StartingDisplay){
    m_snapshot.reason=Reason::Cancelled;m_snapshot.stage=SwitchStage::CleaningDisplay;effect(UsageEffectKind::StopDisplay);changed();return {true,Reason::None};
  }
  if(m_snapshot.stage==SwitchStage::ReleasingInputs || m_snapshot.stage==SwitchStage::StoppingInput){
    m_cancelRequested=true;m_snapshot.reason=Reason::Cancelled;changed();return {true,Reason::None};
  }
  if(m_snapshot.stage==SwitchStage::StartingInput || m_snapshot.stage==SwitchStage::RestoringInput){
    m_stopAll=true;m_allowRollback=false;m_snapshot.reason=Reason::Cancelled;
    m_snapshot.stage=SwitchStage::ReleasingInputs;m_snapshot.pressedInputsReleased=false;
    effect(UsageEffectKind::RevokeInput);effect(UsageEffectKind::ReleaseInputs);changed();return {true,Reason::None};
  }
  if(m_snapshot.stage==SwitchStage::CheckingRollback){
    m_allowRollback=false;m_snapshot.rollback=RollbackResult::Cancelled;complete(UsageMode::None,Reason::Cancelled);return {true,Reason::None};
  }
  if(m_snapshot.stage==SwitchStage::StoppingDisplay || m_snapshot.stage==SwitchStage::CleaningDisplay){
    m_cancelRequested=true;m_allowRollback=false;m_snapshot.reason=Reason::Cancelled;changed();return {true,Reason::None};
  }
  return {false,Reason::ModeConflict};
}
Outcome UsageCoordinator::stop(SessionId operation){
  if(operation==SessionId{}||std::find(m_recentOperations.begin(),m_recentOperations.end(),operation)!=m_recentOperations.end())return {false,Reason::InvalidRequest};
  if(m_snapshot.state==UsageState::CleanupRequired)return {false,Reason::CleanupFailed};
  if(m_snapshot.key.generation==std::numeric_limits<uint64_t>::max())throw std::overflow_error("Usage generation exhausted");
  const bool inputMayStart=m_snapshot.stage==SwitchStage::StartingInput || m_snapshot.stage==SwitchStage::RestoringInput;
  m_recentOperations.push_back(operation);if(m_recentOperations.size()>128)m_recentOperations.pop_front();
  m_snapshot.key={operation,m_snapshot.key.generation+1};m_stopAll=true;m_allowRollback=false;
  m_snapshot.rollback=RollbackResult::Cancelled;m_snapshot.targetMode=UsageMode::None;m_snapshot.reason=Reason::UserStopped;
  if(m_snapshot.effectiveMode==UsageMode::Input || inputMayStart){
    m_snapshot.state=UsageState::Switching;m_snapshot.stage=SwitchStage::ReleasingInputs;m_snapshot.pressedInputsReleased=false;
    effect(UsageEffectKind::CancelDisplayRequest);effect(UsageEffectKind::ReleaseInputs);changed();
  }else if(m_snapshot.effectiveMode==UsageMode::Extended || m_snapshot.state==UsageState::Switching){
    m_snapshot.state=UsageState::Switching;m_snapshot.stage=SwitchStage::CleaningDisplay;effect(UsageEffectKind::StopDisplay);changed();
  }else complete(UsageMode::None,Reason::UserStopped);
  return {true,Reason::None};
}
void UsageCoordinator::connectionLost(){
  m_allowRollback=false;m_stopAll=true;
  if(m_snapshot.effectiveMode==UsageMode::Input || m_snapshot.stage==SwitchStage::StartingInput || m_snapshot.stage==SwitchStage::RestoringInput){
    m_snapshot.state=UsageState::Switching;m_snapshot.stage=SwitchStage::ReleasingInputs;
    m_snapshot.reason=Reason::ConnectionLost;m_snapshot.pressedInputsReleased=false;
    effect(UsageEffectKind::RevokeInput);effect(UsageEffectKind::ReleaseInputs);changed();return;
  }
  if(m_snapshot.effectiveMode==UsageMode::Extended || m_snapshot.stage==SwitchStage::StartingDisplay || m_snapshot.stage==SwitchStage::StoppingDisplay || m_snapshot.stage==SwitchStage::CleaningDisplay){
    m_snapshot.reason=Reason::ConnectionLost;m_snapshot.stage=SwitchStage::CleaningDisplay;m_snapshot.state=UsageState::Switching;
    effect(UsageEffectKind::StopDisplay);changed();
  }else complete(UsageMode::None,Reason::ConnectionLost);
}
void UsageCoordinator::tick(Coordinator::TimePoint now){
  if(m_snapshot.state!=UsageState::Switching||now<m_deadline)return;
  auto key=m_snapshot.key;
  switch(m_snapshot.stage){
  case SwitchStage::Preflight:preflight(key,Reason::RequestExpired);break;
  // System permission dialogs can remain open. The display subservice owns
  // peer request expiry; no resource is created while awaiting these results.
  case SwitchStage::AwaitingPermission:break;
  case SwitchStage::ReleasingInputs:inputsReleased(key,false);break;
  case SwitchStage::StoppingInput:inputStopped(key,false);break;
  case SwitchStage::StartingDisplay:displayFailed(key,Reason::PresentationFailed,false);break;
  case SwitchStage::StoppingDisplay:case SwitchStage::CleaningDisplay:displayStopped(key,false);break;
  case SwitchStage::StartingInput:case SwitchStage::RestoringInput:
    m_allowRollback=false;m_snapshot.state=UsageState::CleanupRequired;m_snapshot.reason=Reason::InputReleaseFailed;
    effect(UsageEffectKind::RevokeInput);changed();break;
  case SwitchStage::CheckingRollback:rollbackAuthorized(key,false);break;
  case SwitchStage::None:break;
  }
}
void UsageCoordinator::inputStatus(Permission permission,std::string focusOwner){
  m_snapshot.inputPermission=permission;m_snapshot.inputFocusOwner=std::move(focusOwner);changed();
  if(permission!=Permission::Granted && m_snapshot.effectiveMode==UsageMode::Input && !m_stopAll){
    connectionLost();m_snapshot.reason=Reason::InputPermissionDenied;changed();
  }
}
const char* usageModeName(UsageMode mode){switch(mode){case UsageMode::None:return "none";case UsageMode::Input:return "input";case UsageMode::Extended:return "extended";}return "none";}
const char* usageStateName(UsageState value){
#define E(x) case UsageState::x:return #x
  switch(value){E(Inactive);E(InputActive);E(DisplayActive);E(Switching);E(CleanupRequired);}
#undef E
  return "Inactive";
}
const char* switchStageName(SwitchStage value){
#define E(x) case SwitchStage::x:return #x
  switch(value){E(None);E(Preflight);E(AwaitingPermission);E(ReleasingInputs);E(StoppingInput);E(StartingDisplay);E(StoppingDisplay);E(StartingInput);E(CleaningDisplay);E(CheckingRollback);E(RestoringInput);}
#undef E
  return "None";
}
const char* rollbackName(RollbackResult value){
#define E(x) case RollbackResult::x:return #x
  switch(value){E(NotNeeded);E(Pending);E(Succeeded);E(Unavailable);E(Failed);E(Cancelled);}
#undef E
  return "NotNeeded";
}
const char* usageEffectName(UsageEffectKind value){
#define E(x) case UsageEffectKind::x:return #x
  switch(value){E(PreflightInput);E(PreflightDisplay);E(RequestInputPermission);E(RequestDisplayPermission);E(ReleaseInputs);E(StopInput);E(StartDisplay);E(StopDisplay);E(StartInput);E(CancelDisplayRequest);E(VerifyRollbackAuthorization);E(RestoreInput);E(RevokeInput);}
#undef E
  return "None";
}
} // namespace deskflow::display
