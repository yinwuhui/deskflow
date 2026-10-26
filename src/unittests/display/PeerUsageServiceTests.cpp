// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/PeerUsageService.h"
#include "display/UsageDisplayBridge.h"
#include "display/InputRouteAdapter.h"
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
using namespace deskflow::display;
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
void drain(){for(int i=0;i<8;++i)QCoreApplication::processEvents();}
QString identity(64,'a');
QString operation="01000000000000000000000000000000";
UsageCapabilities caps(){return {Availability::Supported,Availability::Supported,Availability::Supported,Availability::Supported,Availability::Supported,Availability::Supported,Availability::Supported,true};}
QJsonObject command(QString op,QJsonObject args={}){
 if((op=="startPeerUsage"||op=="switchPeerUsageMode"||op=="stopPeerUsage"||op=="cancelPeerUsageOperation"||op=="retryPeerUsageCleanup")&&!args.contains("baseConnectionId")){
  args.insert("baseConnectionId",QString(32,'c'));args.insert("baseConnectionGeneration","1");
 }
 return {{"id","1"},{"operation",op},{"args",args}};}
UsageKey key(const QJsonObject& snapshot){auto bytes=QByteArray::fromHex(snapshot.value("operationId").toString().toLatin1());UsageKey k;std::copy(bytes.begin(),bytes.end(),k.operation.begin());k.generation=snapshot.value("operationGeneration").toString().toULongLong();return k;}
QJsonObject last(const QSignalSpy& replies){return replies.last().at(0).toJsonObject();}
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);
 try{
  if(argc!=2)return 2;QString name=QString::fromUtf8(argv[1]);PeerUsageService s;
  PeerUsageService::BaseConnection connection;
  connection.id=std::string(32,'c');connection.generation=1;connection.connected=true;
  connection.role=PeerUsageService::InputRole::Source;connection.permission=Permission::Granted;connection.focusOwner="local";
  connection.deviceKeys={"computer:Ubuntu"};
  s.syncConnectedPeer({identity.toStdString(),"Ubuntu",{}},caps(),connection);QSignalSpy replies(&s,&PeerUsageService::commandFinished),effects(&s,&PeerUsageService::backendCommand);
  auto start=[&](QString mode){s.execute(command("startPeerUsage",{{"peerId",identity},{"mode",mode},{"operationId",operation}}));drain();};
  if(name=="BluetoothOnlyBlocksExtended"){
   connection.inputActive=true;connection.displayNetworkPolicy="bluetoothOnly";
   s.syncConnectedPeer({identity.toStdString(),"source",{}},caps(),connection);effects.clear();
   s.updateDisplayCapabilities(identity,Availability::Supported,Availability::Supported,true);
   s.execute(command("switchPeerUsageMode",{{"peerId",identity},{"targetMode","extended"},{"operationId",operation}}));drain();
   check(last(replies).value("reasonCode")=="BluetoothOnlyMode","LAN bypassed Bluetooth-only policy");
   check(s.snapshot(identity).value("effectiveMode")=="input","rejected switch stopped existing input");
   check(!s.snapshot(identity).value("inputCapabilities").toObject().value("displayNetworkAllowed").toBool(),"policy not exposed");
   SessionId incoming{};incoming[0]=3;check(s.beginIncomingDisplay(identity,incoming).reason==Reason::BluetoothOnlyMode,"incoming request bypassed policy");
   check(effects.isEmpty(),"forbidden request scheduled backend work");
   connection.inputActive=false;s.syncConnectedPeer({identity.toStdString(),"source",{}},caps(),connection);effects.clear();start("extended");
   check(last(replies).value("reasonCode")=="BluetoothOnlyMode","direct start bypassed policy");check(effects.isEmpty(),"forbidden start scheduled work");
   connection.displayNetworkPolicy="networkAllowed";s.syncConnectedPeer({identity.toStdString(),"source",{}},caps(),connection);
   auto revision=s.snapshot(identity).value("revision").toString().toULongLong();
   s.updateDisplayCapabilities(identity,Availability::Supported,Availability::Supported,true,Reason::BluetoothOnlyMode);
   check(s.snapshot(identity).value("revision").toString().toULongLong()>revision,"policy-only update did not advance revision");
   s.updateDisplayCapabilities(identity,Availability::Supported,Availability::Supported,true);start("extended");
   check(last(replies).value("accepted").toBool(),"network re-enable did not restore capability");
  }else if(name=="BluetoothPolicyRevokedDuringConsent"){
   connection.inputActive=true;s.syncConnectedPeer({identity.toStdString(),"source",{}},caps(),connection);
   s.execute(command("switchPeerUsageMode",{{"peerId",identity},{"targetMode","extended"},{"operationId",operation}}));drain();auto k=key(s.snapshot(identity));
   s.complete(identity,k,PeerUsageService::Completion::Preflight);drain();effects.clear();
   connection.displayNetworkPolicy="bluetoothOnly";s.syncConnectedPeer({identity.toStdString(),"source",{}},caps(),connection);drain();
   check(s.snapshot(identity).value("effectiveMode")=="input","policy revocation during consent stopped input");
   check(s.snapshot(identity).value("reasonCode")=="BluetoothOnlyMode","missing policy reason");
   s.complete(identity,k,PeerUsageService::Completion::Permission);drain();
   for(const auto& e:effects){auto op=e.at(0).toJsonObject().value("operation");check(op!="ReleaseInputs"&&op!="StartDisplay"&&op!="RestoreInput","late consent bypassed policy");}
  }else if(name=="BluetoothPolicyRevokedDuringDisplay"){
   start("extended");auto k=key(s.snapshot(identity));s.complete(identity,k,PeerUsageService::Completion::Preflight);s.complete(identity,k,PeerUsageService::Completion::Permission);
   s.complete(identity,k,PeerUsageService::Completion::DisplayPresented);drain();effects.clear();
   s.updateDisplayCapabilities(identity,Availability::Supported,Availability::Supported,true,Reason::BluetoothOnlyMode);drain();
   check(s.snapshot(identity).value("effectiveMode")=="extended","display vanished before verified cleanup");
   bool stopped=false;for(const auto& e:effects)stopped|=e.at(0).toJsonObject().value("operation")=="StopDisplay";check(stopped,"policy revocation did not stop display");
   auto generation=s.snapshot(identity).value("operationGeneration");
   s.updateDisplayCapabilities(identity,Availability::Supported,Availability::Supported,true,Reason::BluetoothOnlyMode);
   check(s.snapshot(identity).value("operationGeneration")==generation,"repeated policy revoked pending cleanup generation");
   s.complete(identity,key(s.snapshot(identity)),PeerUsageService::Completion::DisplayStopped,Reason::None,true);drain();
   check(s.snapshot(identity).value("effectiveMode")=="none","policy cleanup restored input");
  }else if(name=="UnavailableSessionDisablesRollback"){
   connection.inputActive=true;check(s.syncConnectedPeer({identity.toStdString(),"source",{}},caps(),connection).accepted,"input import failed");
   s.execute(command("switchPeerUsageMode",{{"peerId",identity},{"targetMode","extended"},{"operationId",operation}}));drain();auto k=key(s.snapshot(identity));
   s.complete(identity,k,PeerUsageService::Completion::Preflight);s.complete(identity,k,PeerUsageService::Completion::Permission);
   s.inputStatus(identity,k,Permission::Granted,"local");s.complete(identity,k,PeerUsageService::Completion::InputsReleased,Reason::None,true);
   s.complete(identity,k,PeerUsageService::Completion::InputStopped,Reason::None,true);drain();effects.clear();
   s.invalidateRollback(identity);s.complete(identity,k,PeerUsageService::Completion::DisplayFailed,Reason::SessionUnavailable,true);drain();
   check(s.snapshot(identity).value("effectiveMode")=="none","unavailable session restored input");
   for(const auto& event:effects)check(event.at(0).toJsonObject().value("operation")!="RestoreInput","restored revoked rollback");
  }else if(name=="PeerCancelPreservesUnreleasedInput"){
   connection.inputActive=true;s.syncConnectedPeer({identity.toStdString(),"source",{}},caps(),connection);
   s.execute(command("switchPeerUsageMode",{{"peerId",identity},{"targetMode","extended"},{"operationId",operation}}));drain();auto k=key(s.snapshot(identity));
   s.complete(identity,k,PeerUsageService::Completion::Preflight);drain();effects.clear();s.peerStopped(identity,true);drain();
   check(s.snapshot(identity).value("effectiveMode")=="input"&&s.snapshot(identity).value("usageState")=="InputActive","peer cancel stopped old input");
   check(!s.complete(identity,k,PeerUsageService::Completion::Permission),"late permission survived peer cancel");
   for(const auto& event:effects)check(event.at(0).toJsonObject().value("operation")!="ReleaseInputs","peer cancel released old input");
  }else if(name=="SelectionPreferenceDoesNotStartUse"){
   QTemporaryDir directory;check(directory.isValid(),"preferences directory");
   {PeerUsageService saved(nullptr,directory.filePath("usage.ini"));saved.syncConnectedPeer({identity.toStdString(),"peer",{}},caps(),connection);
    saved.execute(command("selectUsageMode",{{"peerId",identity},{"mode","extended"}}));drain();}
   PeerUsageService restored(nullptr,directory.filePath("usage.ini"));QSignalSpy starts(&restored,&PeerUsageService::backendCommand);
   restored.syncConnectedPeer({identity.toStdString(),"peer",{}},caps(),connection);drain();
   check(restored.snapshot(identity).value("selectedMode")=="extended"&&restored.snapshot(identity).value("effectiveMode")=="none"&&starts.empty(),"preference activated a use");
  }else if(name=="PendingReconnectRetiresOnlyMatchingConnection"){
   connection.inputActive=true;check(s.syncConnectedPeer({identity.toStdString(),"old",{}},caps(),connection).accepted,"input import failed");
   auto next=connection;next.id=std::string(32,'d');next.generation=2;
   check(s.syncConnectedPeer({identity.toStdString(),"new",{}},caps(),next).reason==Reason::CleanupFailed,"new connection bypassed old lease");
   check(s.connectionClosed(identity,QString(32,'c'),1,true),"old native retirement missing");
   check(s.snapshot(identity).value("baseConnectionId")==QString(32,'d')&&s.snapshot(identity).value("effectiveMode")=="input","pending connection was not adopted");
   check(s.connectionClosed(identity,QString(32,'d'),2,false),"unverified close missing");
   auto pending=next;pending.id=std::string(32,'e');pending.generation=3;
   check(s.syncConnectedPeer({identity.toStdString(),"third",{}},caps(),pending).reason==Reason::CleanupFailed,"third connection bypassed cleanup");
   check(s.connectionClosed(identity,QString(32,'e'),3,true),"pending close was not discarded");
   check(s.connectionClosed(identity,QString(32,'d'),2,true),"verified old retirement failed");
   check(!s.snapshot(identity).value("baseConnected").toBool()&&s.snapshot(identity).value("baseConnectionId")==QString(32,'d'),"adopted a closed pending connection");
  }else if(name=="NormalDisconnectRetiresImportedInput"){
   connection.inputActive=true;check(s.syncConnectedPeer({identity.toStdString(),"source",{}},caps(),connection).accepted,"input readiness update not imported");
   check(key(s.snapshot(identity)).operation!=SessionId{},"imported route has no owner");
   check(s.connectionClosed(identity,QString(32,'c'),1,true),"verified native retirement rejected");drain();
   check(s.snapshot(identity).value("usageState")=="Inactive"&&s.snapshot(identity).value("effectiveMode")=="none","retired input remains dirty");
   connection.id=std::string(32,'d');connection.generation=2;
   check(s.syncConnectedPeer({identity.toStdString(),"source",{}},caps(),connection).accepted,"new connection blocked by retired input");
   check(!s.connectionClosed(identity,QString(32,'c'),1,true),"old close retired new input");
   check(s.snapshot(identity).value("effectiveMode")=="input","new native input not imported");
  }else if(name=="RecoveryUsesUnifiedCleanup"){
   DisplayService display({Availability::Supported,Availability::Supported,false,false,true,1},{});UsageDisplayBridge bridge(&s,&display);
   Key owner{};owner.session[0]=9;owner.request[0]=8;owner.generation=3;
   check(display.restorePendingResource({identity.toStdString(),"previous",{}},owner,Role::Host,"owned-output"),"pending journal not restored");
   check(bridge.adoptCleanup().accepted,"uniform cleanup not restored");
   check(s.snapshot(identity).value("usageState")=="CleanupRequired"&&!s.retainedSnapshots().empty(),"missing startup cleanup status");
   s.execute(command("retryPeerUsageCleanup",{{"peerId",identity},{"operationId",operation}}));drain();
   check(last(replies).value("accepted").toBool(),"uniform recovery retry rejected");
   check(display.snapshot().value("state")=="Stopping","lower cleanup retry not invoked");
   display.cleanupFinished(display.currentKey(),true);drain();
   check(s.snapshot(identity).value("usageState")=="Inactive"&&s.snapshot(identity).value("effectiveMode")=="none","cleanup activated a use");
  }else if(name=="IncomingDisplayRetainsOriginalDirection"){
   connection.id=std::string(32,'d');connection.generation=2;connection.role=PeerUsageService::InputRole::Receiver;connection.inputActive=true;
   s.syncConnectedPeer({identity.toStdString(),"source",{}},caps(),connection);
   s.updateDisplayCapabilities(identity,Availability::Unsupported,Availability::Supported,true);
   DisplayService display({Availability::Supported,Availability::Supported,false,false,true,1},{});
   UsageDisplayBridge bridge(&s,&display);QSignalSpy ready(&bridge,&UsageDisplayBridge::receiverInputReady);
   SessionId session{},request{};session[0]=3;request[0]=4;
   check(display.incomingAuthenticated({identity.toStdString(),"source",{Availability::Supported,Availability::Supported,false,false,true,1}},session,request).accepted,"incoming rejected");
   check(bridge.adoptIncoming().accepted,"incoming usage not reserved");auto k=key(s.snapshot(identity));
   s.complete(identity,k,PeerUsageService::Completion::Preflight);drain();
   check(s.snapshot(identity).value("switchStage")=="ReleasingInputs","incoming skipped release");
   check(ready.empty(),"accepted before original input released");
   s.inputStatus(identity,k,Permission::Granted,"local");s.complete(identity,k,PeerUsageService::Completion::InputsReleased,Reason::None,true);
   s.complete(identity,k,PeerUsageService::Completion::InputStopped,Reason::None,true);drain();
   check(ready.size()==1,"missing receiver readiness");
   check(s.snapshot(identity).value("originalInputDirection")=="peerToLocal","incoming reversed original role");
   check(s.snapshot(identity).value("effectiveMode")=="none","incoming marked streaming early");
  }else if(name=="SelectionAndStatus"){
   s.execute(command("selectPeer",{{"peerId",identity}}));s.execute(command("selectUsageMode",{{"mode","extended"}}));check(replies.empty(),"synchronous API");drain();
   auto snap=s.snapshot(identity);check(snap.value("selectedMode")=="extended"&&snap.value("effectiveMode")=="none"&&effects.empty(),"idle selection started work");
   check(snap.value("inputFocusOwner")=="local"&&snap.value("inputPermissionsReady").toBool(),"lost authoritative input permission/focus");
   check(!snap.contains("shareKeyboardPreference")&&!snap.contains("shareKeyboardEffective"),"retained v1.1 keyboard fields");
   s.execute(command("getInputControlStatus",{{"peerId",identity}}));drain();check(last(replies).value("accepted").toBool(),"missing status API");
  }else if(name=="RejectSplitInput"){
   for(const auto& option:{QJsonObject{{"shareKeyboard",false}},QJsonObject{{"keyboardEnabled",true}}}){
    s.execute(command("startPeerUsage",{{"peerId",identity},{"mode","input"},{"operationId",operation},{"options",option}}));drain();
    check(last(replies).value("reasonCode")=="InvalidRequest"&&effects.empty(),"accepted split keyboard option");
   }
   start("mouse");check(last(replies).value("reasonCode")=="InvalidRequest","accepted obsolete mouse mode");
   s.execute(command("setMouseSharingOptions",{{"peerId",identity}}));drain();check(!last(replies).value("accepted").toBool(),"retained v1.1 API");
  }else if(name=="NoForgedCompletion"){
   start("input");auto k=key(s.snapshot(identity));
   s.execute(command("inputStarted",{{"peerId",identity},{"verified",true}}));drain();check(!last(replies).value("accepted").toBool(),"GUI forged input activation");
   s.inputStatus(identity,k,Permission::Granted,"local");s.complete(identity,k,PeerUsageService::Completion::Preflight);
   s.complete(identity,k,PeerUsageService::Completion::Permission);s.complete(identity,k,PeerUsageService::Completion::InputStarted,Reason::None,true);
   check(s.snapshot(identity).value("effectiveMode")=="input","full input didn't activate");
   s.execute(command("stopPeerUsage",{{"peerId",identity}}));drain();auto stop=key(s.snapshot(identity));
   check(!s.complete(identity,k,PeerUsageService::Completion::InputStarted,Reason::None,true),"stale activation accepted");
   s.inputStatus(identity,stop,Permission::Granted,"local");s.complete(identity,stop,PeerUsageService::Completion::InputsReleased,Reason::None,true);
   s.complete(identity,stop,PeerUsageService::Completion::InputStopped,Reason::None,true);check(s.snapshot(identity).value("effectiveMode")=="none","stop didn't complete");
  }else if(name=="SingleDisplayReservation"){
   s.syncConnectedPeer({std::string(64,'b'),"Mac",{}},caps(),connection);start("extended");
   s.execute(command("startPeerUsage",{{"peerId",QString(64,'b')},{"mode","extended"},{"operationId",operation}}));drain();
   check(last(replies).value("reasonCode")=="PeerBusy","reserved second display");
   check(s.snapshot(QString(64,'b')).value("effectiveMode")=="none","affected another peer");
  }else if(name=="CancelQueuedStart"){
   start("extended");auto k=key(s.snapshot(identity));s.complete(identity,k,PeerUsageService::Completion::Preflight);drain();effects.clear();
   QObject::connect(&s,&PeerUsageService::snapshotChanged,&s,[&](const QJsonObject& snap){
    if(snap.value("switchStage")=="StartingDisplay")s.execute(command("stopPeerUsage",{{"peerId",identity}}));
   });
   s.complete(identity,k,PeerUsageService::Completion::Permission);drain();
   for(auto event:effects)check(event.at(0).toJsonObject().value("operation")!="StartDisplay","queued start survived stop");
  }else if(name=="StalePermission"){
   start("input");auto k=key(s.snapshot(identity));s.execute(command("stopPeerUsage",{{"peerId",identity}}));drain();
   check(!s.inputStatus(identity,k,Permission::Granted,"local"),"late permission applied to next generation");
  }else if(name=="DisplayTransactionBridge"||name=="PeerRefusalPreservesInput"){
   DisplayService display({Availability::Supported,Availability::Supported,false,false,true,1},{});
   display.authenticatedPeer({identity.toStdString(),"Ubuntu",{Availability::Supported,Availability::Supported,false,false,true,1}});
   UsageDisplayBridge bridge(&s,&display);QSignalSpy media(&display,&DisplayService::backendCommand);
   start("input");auto k=key(s.snapshot(identity));s.inputStatus(identity,k,Permission::Granted,"local");
   s.complete(identity,k,PeerUsageService::Completion::Preflight);s.complete(identity,k,PeerUsageService::Completion::Permission);
   s.complete(identity,k,PeerUsageService::Completion::InputStarted,Reason::None,true);drain();effects.clear();
   s.execute(command("switchPeerUsageMode",{{"peerId",identity},{"targetMode","extended"},{"operationId","02000000000000000000000000000000"}}));drain();k=key(s.snapshot(identity));
   s.complete(identity,k,PeerUsageService::Completion::Preflight);drain();auto dk=display.currentKey();
   check(display.snapshot().value("state")=="Connecting","didn't request display consent");
   if(name=="PeerRefusalPreservesInput"){
    display.peerResponse(dk,false,Reason::PeerRejected);drain();
    check(s.snapshot(identity).value("effectiveMode")=="input"&&s.snapshot(identity).value("reasonCode")=="PeerRejected","refusal lost old input");
   }else{
    display.peerResponse(dk,true,Reason::None);display.permissionResult(dk,Permission::Granted);drain();
    check(s.snapshot(identity).value("switchStage")=="ReleasingInputs","authorization didn't reach input barrier");
    check(display.snapshot().value("state")=="Authorizing","created display before stopping input");
    for(auto event:media)check(event.at(0).toJsonObject().value("operation")!="PrepareHost","early display resource creation");
    s.complete(identity,k,PeerUsageService::Completion::InputsReleased,Reason::None,true);drain();
    display.setInputRouting(Routing::Clear);s.complete(identity,k,PeerUsageService::Completion::InputStopped,Reason::None,true);drain();
    check(display.snapshot().value("state")=="Preparing","didn't resume authorized preparation");
    display.backendPrepared(dk,"Meta-test");drain();check(s.snapshot(identity).value("effectiveMode")=="none","marked active before presentation");
    display.firstFramePresented(dk);drain();check(s.snapshot(identity).value("effectiveMode")=="extended","native presentation not propagated");
    // Ending from the display side cannot silently restore keyboard/mouse input.
    effects.clear();display.stopManagedSession(dk,Reason::UserStopped);drain();display.cleanupFinished(display.currentKey(),true);drain();
    check(s.snapshot(identity).value("effectiveMode")=="none","peer end kept usage active");
    for(auto event:effects)check(event.at(0).toJsonObject().value("operation")!="StartInput","peer end started input");
   }
  }else if(name=="InputRouteAdapterAcknowledgements"||name=="InputRouteAdapterStaleResult"){
   DisplayService display({},{});InputRouteAdapter input(&s,&display);QSignalSpy commands(&input,&InputRouteAdapter::coreCommand);
   if(name=="InputRouteAdapterStaleResult"){
    start("input");check(!commands.empty(),"no core preflight");auto request=commands.last().at(0).toJsonObject();
    auto malformed=request;malformed.insert("operationId","ff000000000000000000000000000000");malformed.insert("accepted",true);malformed.insert("connected",true);malformed.insert("supported",true);
    input.coreResult(malformed);check(s.snapshot(identity).value("switchStage")=="Preflight","stale core response advanced transaction");
    input.coreUnavailable();drain();check(s.snapshot(identity).value("reasonCode")=="ConnectionLost","core loss wasn't propagated");
   }else{
    uint32_t sequence=0;bool suspended=false,received=false,ready=false;unsigned resultMode=0;
    QObject::connect(&input,&InputRouteAdapter::coreCommand,&input,[&](const QJsonObject& command){
     // Simulate core replies only; actual wire parsing/platform release are
     // covered separately by ClientProxyTests and InputReleaseTests.
     auto response=command;auto kind=command.value("kind").toString();auto requested=command.value("sequence").toString().toUInt();
     if(kind=="probe"){sequence=requested;received=true;ready=true;resultMode=3;}
     else if(kind=="suspend"){sequence=requested;suspended=true;received=true;ready=false;resultMode=1;}
     else if(kind=="resume"){sequence=requested;received=true;ready=false;resultMode=2;}
     else if(kind=="commitResume")suspended=false;
     response.insert("accepted",true);response.insert("connected",true);response.insert("supported",true);
     response.insert("suspended",suspended);response.insert("localFocus",true);
     response.insert("acknowledgementReceived",requested==sequence&&received);
     response.insert("acknowledged",requested==sequence&&resultMode==(command.value("enabled").toBool()?2U:1U));
     response.insert("inputPermissionsReady",requested==sequence&&ready);response.insert("otherRoutesIsolated",true);
     response.insert("lastSequence",QString::number(sequence));input.coreResult(response);
    });
    start("input");check(s.snapshot(identity).value("effectiveMode")=="input","verified resume did not activate input");
    check(s.snapshot(identity).value("inputPermissionsReady").toBool(),"input permission probe missing");
    s.execute(command("stopPeerUsage",{{"peerId",identity}}));drain();
    check(s.snapshot(identity).value("effectiveMode")=="none"&&s.snapshot(identity).value("pressedInputsReleased").toBool(),"acknowledged pause did not finish stop");
    bool probed=false,paused=false,resumed=false,committed=false;
    for(auto event:commands){auto kind=event.at(0).toJsonObject().value("kind").toString();probed|=kind=="probe";paused|=kind=="suspend";resumed|=kind=="resume";committed|=kind=="commitResume";}
    check(probed&&paused&&resumed&&committed,"input protocol barriers were skipped");
   }
  }else if(name=="ExistingConnectionSnapshot"){
   connection.role=PeerUsageService::InputRole::Receiver;connection.inputActive=true;
   // A role change requires a new authenticated connection identity.
   connection.id=std::string(32,'d');connection.generation=2;
   check(s.syncConnectedPeer({identity.toStdString(),"Ubuntu",{}},caps(),connection).accepted,"existing connection import failed");
   s.execute(command("queryConnectedPeerUsage",{{"peerId",identity}}));drain();auto snap=last(replies).value("data").toObject();
   check(snap.value("effectiveMode")=="input"&&snap.value("originalInputDirection")=="peerToLocal"&&snap.value("baseConnected").toBool(),"lost existing input direction/state");
   check(!s.connectionClosed(identity,QString(32,'c'),1),"old disconnect closed new relationship");
  }else if(name=="RejectStaleConnectionCommand"){
   s.execute(command("startPeerUsage",{{"peerId",identity},{"mode","extended"},{"operationId",operation},{"baseConnectionId",QString(32,'d')},{"baseConnectionGeneration","1"}}));drain();
   check(last(replies).value("reasonCode")=="InvalidRequest"&&effects.empty(),"stale connection command started usage");
   check(s.connectionClosed(identity,QString(32,'c'),1),"current disconnect rejected");start("input");
   check(last(replies).value("reasonCode")=="ConnectionLost","started on disconnected relationship");
  }else if(name=="UnifiedInputCleanup"){
   start("input");auto k=key(s.snapshot(identity));s.inputStatus(identity,k,Permission::Granted,"local");
   s.complete(identity,k,PeerUsageService::Completion::Preflight);s.complete(identity,k,PeerUsageService::Completion::Permission);
   s.complete(identity,k,PeerUsageService::Completion::InputStarted,Reason::None,false);drain();effects.clear();
   s.execute(command("retryPeerUsageCleanup",{{"peerId",identity},{"operationId","03000000000000000000000000000000"}}));drain();auto cleanup=key(s.snapshot(identity));
   check(last(replies).value("accepted").toBool()&&cleanup!=k,"cleanup retry didn't invalidate stale completion");
   check(!s.complete(identity,k,PeerUsageService::Completion::InputCleaned,Reason::None,true),"stale cleanup accepted");
   s.complete(identity,cleanup,PeerUsageService::Completion::InputCleaned,Reason::None,true);
   check(s.snapshot(identity).value("effectiveMode")=="none"&&s.snapshot(identity).value("usageState")=="Inactive","cleanup resumed a usage");
  }else throw std::runtime_error("unknown case");
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
