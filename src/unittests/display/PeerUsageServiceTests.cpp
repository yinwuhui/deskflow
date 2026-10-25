// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/PeerUsageService.h"
#include "display/UsageDisplayBridge.h"
#include "display/InputRouteAdapter.h"
#include <QCoreApplication>
#include <QSignalSpy>
#include <iostream>
#include <stdexcept>
using namespace deskflow::display;
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
void drain(){for(int i=0;i<8;++i)QCoreApplication::processEvents();}
QString identity(64,'a');
QString operation="01000000000000000000000000000000";
UsageCapabilities caps(){return {Availability::Supported,Availability::Supported,Availability::Supported,Availability::Supported,Availability::Supported,Availability::Supported,Availability::Supported,true};}
QJsonObject command(QString op,QJsonObject args={}){return {{"id","1"},{"operation",op},{"args",args}};}
UsageKey key(const QJsonObject& snapshot){auto bytes=QByteArray::fromHex(snapshot.value("operationId").toString().toLatin1());UsageKey k;std::copy(bytes.begin(),bytes.end(),k.operation.begin());k.generation=snapshot.value("operationGeneration").toString().toULongLong();return k;}
QJsonObject last(const QSignalSpy& replies){return replies.last().at(0).toJsonObject();}
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);
 try{
  if(argc!=2)return 2;QString name=QString::fromUtf8(argv[1]);PeerUsageService s;
  s.authenticatedPeer({identity.toStdString(),"Ubuntu",{}},caps());QSignalSpy replies(&s,&PeerUsageService::commandFinished),effects(&s,&PeerUsageService::backendCommand);
  auto start=[&](QString mode){s.execute(command("startPeerUsage",{{"peerId",identity},{"mode",mode},{"operationId",operation}}));drain();};
  if(name=="SelectionAndStatus"){
   s.execute(command("selectPeer",{{"peerId",identity}}));s.execute(command("selectUsageMode",{{"mode","extended"}}));check(replies.empty(),"synchronous API");drain();
   auto snap=s.snapshot(identity);check(snap.value("selectedMode")=="extended"&&snap.value("effectiveMode")=="none"&&effects.empty(),"idle selection started work");
   check(snap.value("inputFocusOwner").isNull()&&!snap.value("inputPermissionsReady").toBool(),"invented input permission/focus");
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
   s.authenticatedPeer({std::string(64,'b'),"Mac",{}},caps());start("extended");
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
  }else throw std::runtime_error("unknown case");
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
