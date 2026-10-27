// SPDX-License-Identifier: MIT
#include "display/ScreenInventoryService.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QJsonDocument>
#include <iostream>
#include <stdexcept>
using namespace deskflow::display;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
QJsonObject screen(QChar id='1',bool primary=true){return {{"screenId",QString(64,id)},{"name",QString("Monitor %1").arg(id)},
 {"logicalWidth",1920},{"logicalHeight",1080},{"x",0},{"y",0},{"pixelWidth",3840},{"pixelHeight",2160},{"pixelSizeSource","qtGeometryTimesDpr"},
 {"scale",2.0},{"refreshRate",60.0},{"physicalWidthMm",600.0},{"physicalHeightMm",340.0},{"physicalSizeSource","systemReportedUnverified"},{"primary",primary},{"orientation","landscape"}};}
QJsonObject connection(QString peer,QString id, bool connected=true){return {{"peerId",peer},{"baseConnectionId",id},{"baseConnected",connected},{"deviceKeys",QJsonArray{"computer:remote"}},{"displayNetworkPolicy","bluetoothOnly"}};}
int main(int argc,char** argv){QCoreApplication app(argc,argv);QTemporaryDir dir;
 try{
 const QString a(64,'a'),b(64,'b'),ca(32,'c'),cb(32,'d');
 ScreenInventoryService left(nullptr,dir.filePath("left.ini"),false),right(nullptr,dir.filePath("right.ini"),false);
 require(left.updateLocalScreens({screen()}),"local inventory rejected");require(right.updateLocalScreens({screen('2'),screen('3',false)}),"second inventory rejected");
 QObject::connect(&left,&ScreenInventoryService::outboundControl,&right,[&](const QString&,const QString&,const QJsonObject& body){right.receiveControl({{"peerId",a},{"baseConnectionId",cb},{"body",body}});});
 QObject::connect(&right,&ScreenInventoryService::outboundControl,&left,[&](const QString&,const QString&,const QJsonObject& body){left.receiveControl({{"peerId",b},{"baseConnectionId",ca},{"body",body}});});
 left.observeConnection(connection(b,ca));right.observeConnection(connection(a,cb));
 require(left.peerInventory(b).value("fresh").toBool()&&right.peerInventory(a).value("fresh").toBool(),"Bluetooth-only metadata exchange failed");
 require(left.forDevice("computer:remote").value("inventory").toObject().value("screens").toArray().size()==2,"device binding missing");
 auto selected=left.selectionForPeer(b);require(selected.value("screenId")==QString(64,'2'),"automatic selection did not use primary");QString name;
 require(right.resolveLocalSelection(selected,&name)&&name=="Monitor 2","selection could not resolve");
 require(left.setPreference(b,QString(64,'3'),"fullscreen"),"selection not saved");selected=left.selectionForPeer(b);
 require(right.resolveLocalSelection(selected)&&selected.value("presentationMode")=="fullscreen","preference not used");
 const auto before=left.peerInventory(b);auto bad=right.localInventory();bad.insert("screens",QJsonArray{screen('2'),screen('2')});
 left.receiveControl({{"peerId",b},{"baseConnectionId",ca},{"body",QJsonObject{{"protocol","device-screens/1"},{"type","inventory"},{"inventory",bad}}}});
 require(left.peerInventory(b)==before,"duplicate IDs accepted");
 bad=right.localInventory();bad.insert("revision","999");auto s=screen('2');s.insert("name",QString(200,'x'));bad.insert("screens",QJsonArray{s});
 left.receiveControl({{"peerId",b},{"baseConnectionId",ca},{"body",QJsonObject{{"protocol","device-screens/1"},{"type","inventory"},{"inventory",bad}}}});
 require(left.peerInventory(b)==before,"unbounded name accepted");
 bad=right.localInventory();bad.insert("revision","999");
 left.receiveControl({{"peerId",b},{"baseConnectionId",QString(32,'e')},{"body",QJsonObject{{"protocol","device-screens/1"},{"type","inventory"},{"inventory",bad}}}});
 require(left.peerInventory(b)==before,"wrong connection accepted");
 QSignalSpy updates(&right,&ScreenInventoryService::outboundControl);right.updateLocalScreens({screen('2'),screen('3',false)});require(updates.isEmpty(),"unchanged inventory retransmitted");
 auto changed=screen('2');changed.insert("pixelWidth",2560);right.updateLocalScreens({changed});
 require(updates.size()==1,"screen change not pushed");require(!right.resolveLocalSelection(selected),"old revision authorized session");
 require(left.selectionForPeer(b).isEmpty(),"removed preferred screen silently replaced");
 auto older=right.localInventory();older.insert("revision","1");auto current=left.peerInventory(b);
 left.receiveControl({{"peerId",b},{"baseConnectionId",ca},{"body",QJsonObject{{"protocol","device-screens/1"},{"type","inventory"},{"inventory",older}}}});
 require(left.peerInventory(b)==current,"out-of-order revision accepted");
 require(left.setPreference(b,{},"window"),"automatic selection rejected");require(!left.selectionForPeer(b).isEmpty(),"automatic selection not restored");
 left.observeConnection(connection(b,ca,false));require(!left.forDevice("computer:remote").value("fresh").toBool(),"disconnect cache still fresh");
 require(left.selectionForPeer(b).isEmpty()&&!left.setPreference(b,QString(64,'2'),"window"),"offline cache authorized session");
 {ScreenInventoryService restored(nullptr,dir.filePath("left.ini"),false);require(!restored.forDevice("computer:remote").isEmpty(),"disk cache lost");require(!restored.forDevice("computer:remote").value("fresh").toBool(),"disk cache marked live");}
 left.observeConnection(connection(b,QString(32,'f')));current=left.peerInventory(b);
 left.receiveControl({{"peerId",b},{"baseConnectionId",ca},{"body",QJsonObject{{"protocol","device-screens/1"},{"type","inventory"},{"inventory",right.localInventory()}}}});
 require(left.peerInventory(b)==current,"retired connection refreshed cache");
 std::cout<<"Screen inventory exchange, validation, preferences, revisions and cache passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
