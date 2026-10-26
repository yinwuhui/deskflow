// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/DisplayService.h"
#include "display/ResourceJournal.h"
#include <QFile>
#include <QDir>
#include <QCoreApplication>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
using namespace deskflow::display;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Capabilities caps() { return {Availability::Supported, Availability::Supported, false, false, true, 1}; }
Peer peer() { return {std::string(64,'a'),"Ubuntu",caps()}; }
void drain() { for (int n=0;n<8;++n) QCoreApplication::processEvents(); }
QJsonObject command(const QString& operation, QJsonObject args = {}) { return {{"id","ui-1"},{"operation",operation},{"args",args}}; }
QJsonObject keyJson(const QJsonObject& snapshot) {
  QJsonObject result; for (auto key : {"sessionId","requestId","generation","displayEpoch"}) result.insert(key,snapshot.value(key)); return result;
}
Key key(const QJsonObject& snapshot) {
  Key result;
  auto session=QByteArray::fromHex(snapshot.value("sessionId").toString().toLatin1());
  auto request=QByteArray::fromHex(snapshot.value("requestId").toString().toLatin1());
  std::copy(session.begin(),session.end(),result.session.begin()); std::copy(request.begin(),request.end(),result.request.begin());
  result.generation=snapshot.value("generation").toString().toULongLong(); result.epoch=snapshot.value("displayEpoch").toInt(); return result;
}
QJsonObject reply(const QSignalSpy& spy) { return spy.last().at(0).toJsonObject(); }
int main(int argc,char** argv) {
  QCoreApplication app(argc,argv); QTemporaryDir directory;
  try {
    if(argc!=2)return 2; const QString name=QString::fromUtf8(argv[1]);
    DisplayService service(caps(), directory.filePath("display.ini"));
    QSignalSpy replies(&service,&DisplayService::commandFinished), effects(&service,&DisplayService::backendCommand);
    if(name=="BluetoothPolicyGatesLowerService") {
      service.authenticatedPeer(peer());service.setPeerNetworkPolicy(QString(64,'a'),false);
      SessionId request{};request[0]=1;check(service.requestManagedSession(QString(64,'a'),request).reason==Reason::BluetoothOnlyMode,"managed API bypassed policy");
      SessionId session{};session[0]=2;check(service.incomingAuthenticated(peer(),session,request).reason==Reason::BluetoothOnlyMode,"incoming API bypassed policy");drain();
      check(effects.isEmpty(),"forbidden request scheduled resources");
      service.setPeerNetworkPolicy(QString(64,'a'),true);service.setInputRouting(Routing::Clear);
      check(service.requestManagedSession(QString(64,'a'),request).accepted,"network re-enable failed");
      auto current=service.currentKey();service.peerResponse(current,true,Reason::None);service.permissionResult(current,Permission::Granted);service.resumeManagedSession(current);
      service.setPeerNetworkPolicy(QString(64,'a'),false);drain();
      for(const auto& e:effects)check(e.at(0).toJsonObject().value("operation")!="PrepareHost","queued creation bypassed revoked policy");
      check(service.snapshot().value("state")=="Stopping","revoked lower session not stopped");
    }else if(name=="AsyncAndSnapshot") {
      service.execute(command("getDisplaySessionSnapshot")); check(replies.isEmpty(),"synchronous GUI command"); drain();
      auto snapshot=reply(replies).value("snapshot").toObject();
      check(snapshot.value("generation").isString() && snapshot.value("revision").isString(),"lossy JSON counters");
      check(snapshot.value("statistics").isNull() && !snapshot.value("firstFramePresented").toBool(),"invented telemetry");
      check(!snapshot.value("inputEnabled").toBool(),"P0 input enabled");
    } else if(name=="PeerAndRouteGates") {
      service.execute(command("requestDisplaySession",{{"peerId",QString(64,'a')}}));drain();
      check(reply(replies).value("reasonCode")=="ModeConflict","GUI bypassed usage transaction");
      SessionId request{};request[0]=1;
      check(service.requestManagedSession(QString(64,'a'),request).reason==Reason::NetworkUnavailable,"unregistered identity accepted");
      service.authenticatedPeer(peer());
      check(service.requestManagedSession(QString(64,'a'),request).accepted,"managed authorization rejected");
      auto current=key(service.snapshot());service.peerResponse(current,true,Reason::None);service.permissionResult(current,Permission::Granted);drain();
      check(service.snapshot().value("state")=="Authorizing","started before input barrier");
      check(service.resumeManagedSession(current).reason==Reason::InputConflict,"unknown route bypassed barrier");
      service.setInputRouting(Routing::Clear);check(service.resumeManagedSession(current).accepted,"verified route rejected");
      check(service.snapshot().value("state")=="Preparing","no managed preparation");
    } else if(name=="NoForgedCallbacks") {
      service.execute(command("firstFramePresented")); drain(); check(!reply(replies).value("accepted").toBool(),"GUI forged renderer event");
      service.execute(command("queryLocalDisplayCapabilities",{{"trusted",true}})); drain();
      check(!reply(replies).value("accepted").toBool(),"unknown field accepted");
      service.execute(command("setReceiveRequestsEnabled",{{"enabled",1}})); drain();
      check(!reply(replies).value("accepted").toBool(),"number treated as consent boolean");
    } else if(name=="CancelQueuedPreparation") {
      service.authenticatedPeer(peer()); service.setInputRouting(Routing::Clear);
      SessionId request{};request[0]=1;service.requestManagedSession(QString(64,'a'),request);drain();auto current=key(service.snapshot());
      service.peerResponse(current,true,Reason::None);
      QObject::connect(&service,&DisplayService::snapshotChanged,&service,[&](const QJsonObject& snapshot){
        if(snapshot.value("state")=="Preparing")service.execute(command("cancelDisplayRequest",keyJson(snapshot)));
      });
      service.permissionResult(current,Permission::Granted);service.resumeManagedSession(current);drain();
      bool prepared=false,cleanup=false;
      for (const auto& event:effects) {
        const auto op=event.at(0).toJsonObject().value("operation").toString();
        prepared|=op=="PrepareHost"; cleanup|=op=="CleanupResources";
      }
      check(!prepared && cleanup,"cancelled queued work still created a display");
      service.backendPrepared(current,"late"); check(service.snapshot().value("state")=="Stopping","late callback revived session");
      auto stale=keyJson(service.snapshot()); stale.insert("generation",17);
      service.execute(command("retryDisplayCleanup",stale)); drain();
      check(!reply(replies).value("accepted").toBool(),"numeric generation accepted");
    } else if(name=="PreferencePersistence") {
      service.execute(command("setReceiveRequestsEnabled",{{"enabled",false}})); drain();
      // Force storage for this persistence test; product writes remain QSettings-managed.
      QSettings settings(directory.filePath("display.ini"),QSettings::IniFormat); settings.sync();
      DisplayService restored(caps(),directory.filePath("display.ini"));
      check(!restored.snapshot().value("receiveRequestsEnabled").toBool(),"receiving preference not persisted");
    } else if(name=="JournalOwnership") {
      OwnedResource record;record.owner.session[0]=1;record.owner.request[0]=2;record.owner.generation=9;
      record.peerIdentity=QString(64,'a');record.role=Role::Host;
      const auto path=directory.filePath("resources");
      {
        ResourceJournal journal(path);check(journal.status()==ResourceJournal::Status::Empty,"fresh journal");
        check(journal.claim(record),"ownership claim failed");
        ResourceJournal other(path);check(other.status()==ResourceJournal::Status::Unavailable,"second owner acquired lock");
        check(!journal.claim(record),"active resource overwritten");
        auto wrong=record.owner;++wrong.generation;
        check(!journal.updateDisplay(wrong,"Meta-0"),"foreign generation updated ownership");
        check(!journal.clearVerified(record.owner,false),"unverified cleanup cleared ownership");
        check(journal.updateDisplay(record.owner,"Meta-0"),"owned display not recorded");
      }
      {
        ResourceJournal restored(path);check(restored.status()==ResourceJournal::Status::Pending,"lost ownership at restart");
        check(restored.record()->displayId=="Meta-0" && restored.record()->owner.generation==9,"lost resource identity");
        check(restored.clearVerified(record.owner,true),"verified cleanup not committed");
      }
      ResourceJournal clean(path);check(clean.status()==ResourceJournal::Status::Empty,"clean journal not durable");
    } else if(name=="CorruptJournal") {
      const auto path=directory.filePath("resources");QDir().mkpath(path);
      QFile file(path+"/active.json");check(file.open(QFile::WriteOnly),"fixture file");file.write("{corrupt");file.close();
      ResourceJournal journal(path);check(journal.status()==ResourceJournal::Status::Corrupt,"invalid record treated as clean");
      OwnedResource record;record.owner.session[0]=1;record.owner.request[0]=2;record.owner.generation=1;record.peerIdentity=QString(64,'a');record.role=Role::Host;
      check(!journal.claim(record),"corrupt ownership overwritten");
    } else if(name=="LayoutOwnership") {
      auto capabilities=caps();capabilities.layout=true;service.updateCapabilities(capabilities);
      service.authenticatedPeer(peer());service.setInputRouting(Routing::Clear);SessionId request{};request[0]=1;
      check(service.requestManagedSession(QString(64,'a'),request).accepted,"managed start");auto current=key(service.snapshot());
      service.peerResponse(current,true,Reason::None);service.permissionResult(current,Permission::Granted);service.resumeManagedSession(current);
      service.backendPrepared(current,"owned-display");service.peerReady(current);service.firstFramePresented(current);drain();
      check(service.snapshot().value("state")=="Streaming","fixture not streaming");
      service.execute(command("queryDisplayLayout",keyJson(service.snapshot())));drain();
      auto effect=effects.last().at(0).toJsonObject();check(effect.value("operation")=="QueryDisplayLayout","missing layout adapter command");
      auto stale=current;++stale.generation;
      QJsonObject data{{"layout",QJsonObject{{"displayId","owned-display"},{"x",1920},{"y",0},{"scale",1}}},{"displays",QJsonArray{}}};
      service.layoutResult(stale,effect.value("commandId").toString(),true,data);check(service.snapshot().value("confirmedLayout").isNull(),"stale layout published");
      const auto oldRevision=service.snapshot().value("revision").toString().toULongLong();
      service.layoutResult(current,effect.value("commandId").toString(),true,data);check(service.snapshot().value("revision").toString().toULongLong()>oldRevision,"layout revision did not advance");check(reply(replies).value("accepted").toBool(),"confirmed layout rejected");
      auto args=keyJson(service.snapshot());args.insert("layout",QJsonObject{{"x",1.5},{"y",0}});
      service.execute(command("setDisplayLayout",args));drain();check(!reply(replies).value("accepted").toBool(),"fractional position accepted");
      service.execute(command("queryDisplayLayout",keyJson(service.snapshot())));drain();effect=effects.last().at(0).toJsonObject();
      service.stopManagedSession(current);drain();service.layoutResult(current,effect.value("commandId").toString(),true,data);
      check(!reply(replies).value("accepted").toBool(),"late layout revived stopping session");
      service.cleanupFinished(service.currentKey(),true);check(service.snapshot().value("confirmedLayout").isNull()&&service.snapshot().value("receiverWindow").isNull()&&service.snapshot().value("streamConfig").isNull(),"idle retained active presentation state");
    } else if(name=="ApplicationSingleton") {
      auto a=DisplayService::applicationService(); auto b=DisplayService::applicationService(); check(a==b,"multiple product services");
      check(a->localCapabilities().value("send")=="unknown","unprobed product capability advertised");
    } else throw std::runtime_error("unknown case");
    return 0;
  } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
