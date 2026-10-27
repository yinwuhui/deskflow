// SPDX-License-Identifier: MIT
#include "ScreenInventoryService.h"
#include "CoreInputChannel.h"
#include "PeerUsageService.h"
#include <QGuiApplication>
#include <QScreen>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>
#include <cmath>
#include <limits>
#ifdef Q_OS_MACOS
#include <CoreGraphics/CoreGraphics.h>
#endif
#ifdef DESKFLOW_SCREEN_DBUS
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#endif
namespace deskflow::display {
namespace {
QString hash(const QString& value){return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(),QCryptographicHash::Sha256).toHex());}
bool identity(const QString& value,int size){return value.size()==size&&QRegularExpression("^[0-9a-f]+$").match(value).hasMatch();}
bool number(const QJsonValue& value,double low,double high,bool integral=false){const auto v=value.toDouble(-1);return value.isDouble()&&std::isfinite(v)&&v>=low&&v<=high&&(!integral||std::floor(v)==v);}
QJsonObject findScreen(const QJsonObject& inventory,const QString& id){for(const auto& value:inventory.value("screens").toArray())if(value.toObject().value("screenId")==id)return value.toObject();return {};}
QJsonObject fixedMode(){return {{"width",1920},{"height",1080},{"fps",30},{"codec","h264"},{"color","SDR"}};}
}
ScreenInventoryService* ScreenInventoryService::applicationService(){
  static QPointer<ScreenInventoryService> service;
  if(!service)service=new ScreenInventoryService(QCoreApplication::instance());return service;
}
ScreenInventoryService::ScreenInventoryService(QObject* parent,const QString& cacheFile,bool watchScreens):QObject(parent){
  auto path=cacheFile.isEmpty()?QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)+"/screen-inventory.ini":cacheFile;
  QDir().mkpath(QFileInfo(path).absolutePath());m_settings=std::make_unique<QSettings>(path,QSettings::IniFormat);
  m_local={{"schemaVersion",1},{"instanceId",QUuid::createUuid().toString(QUuid::Id128)},{"revision","1"},{"screens",QJsonArray{}}};
  m_watch=watchScreens&&qobject_cast<QGuiApplication*>(QCoreApplication::instance());
  m_debounce.setSingleShot(true);m_debounce.setInterval(200);connect(&m_debounce,&QTimer::timeout,this,[this]{refreshLocalScreens();queryNativeScreens();});
  if(m_watch){
    const auto schedule=[this]{m_debounce.start();};
    connect(qGuiApp,&QGuiApplication::screenAdded,this,schedule);connect(qGuiApp,&QGuiApplication::screenRemoved,this,schedule);
    connect(qGuiApp,&QGuiApplication::primaryScreenChanged,this,schedule);refreshLocalScreens();queryNativeScreens();
  }
}
void ScreenInventoryService::attach(CoreInputChannel* channel,PeerUsageService* usage){
  if(m_attached)return;m_attached=true;
  connect(channel,&CoreInputChannel::authenticatedConnectionObserved,this,&ScreenInventoryService::observeConnection);
  connect(channel,&CoreInputChannel::peerControlReceived,this,&ScreenInventoryService::receiveControl);
  connect(usage,&PeerUsageService::connectionChanged,this,[this](const QJsonObject& c){if(!c.value("baseConnected").toBool())observeConnection(c);});
  connect(this,&ScreenInventoryService::outboundControl,channel,&CoreInputChannel::sendPeerControl);
}
bool ScreenInventoryService::validInventory(const QJsonObject& data){
  if(data.size()!=4||QJsonDocument(data).toJson(QJsonDocument::Compact).size()>14000||data.value("schemaVersion")!=1||!identity(data.value("instanceId").toString(),32)||!data.value("screens").isArray())return false;
  bool ok=false;const auto revision=data.value("revision").toString().toULongLong(&ok);if(!ok||!revision||QString::number(revision)!=data.value("revision"))return false;
  const auto screens=data.value("screens").toArray();if(screens.size()>16)return false;QSet<QString> ids;int primary=0;
  const QSet<QString> fields{"screenId","name","logicalWidth","logicalHeight","x","y","pixelWidth","pixelHeight","pixelSizeSource","scale","refreshRate","physicalWidthMm","physicalHeightMm","physicalSizeSource","primary","orientation"};
  for(const auto& v:screens){if(!v.isObject())return false;const auto s=v.toObject();
    if(s.keys().size()!=fields.size())return false;for(auto it=s.begin();it!=s.end();++it)if(!fields.contains(it.key()))return false;
    const auto id=s.value("screenId").toString(),name=s.value("name").toString();
    if(!identity(id,64)||ids.contains(id)||name.isEmpty()||name.size()>128||name.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")))return false;ids.insert(id);
    for(auto key:{"logicalWidth","logicalHeight","pixelWidth","pixelHeight"})if(!number(s.value(key),1,32768,true))return false;
    for(auto key:{"x","y"})if(!number(s.value(key),-131072,131072,true))return false;
    if(!number(s.value("scale"),.5,8)||!s.value("primary").isBool()||(s.value("pixelSizeSource")!="qtGeometryTimesDpr"&&s.value("pixelSizeSource")!="gnomeDisplayConfig"&&s.value("pixelSizeSource")!="coreGraphicsDisplayMode")||s.value("physicalSizeSource")!="systemReportedUnverified")return false;
    if(s.value("orientation")!="landscape"&&s.value("orientation")!="portrait")return false;
    if(!s.value("refreshRate").isNull()&&!number(s.value("refreshRate"),1,1000))return false;
    for(auto key:{"physicalWidthMm","physicalHeightMm"})if(!s.value(key).isNull()&&!number(s.value(key),1,10000))return false;
    primary+=s.value("primary").toBool();
  }return primary<=1;
}
bool ScreenInventoryService::updateLocalScreens(const QJsonArray& screens){
  auto next=m_local;next.insert("screens",screens);if(!validInventory(next))return false;
  if(screens==m_local.value("screens").toArray())return true;
  const auto revision=m_local.value("revision").toString().toULongLong();if(revision==std::numeric_limits<qulonglong>::max())return false;
  next.insert("revision",QString::number(revision+1));m_local=next;
  for(const auto& peer:m_connections.keys())publish(peer);Q_EMIT changed();return true;
}
void ScreenInventoryService::refreshLocalScreens(){
  if(!m_watch)return;QJsonArray screens;QHash<QString,int> occurrences;
  for(auto* screen:QGuiApplication::screens()){
    if(screens.size()>=16)break;
    if(!m_watched.contains(screen)){
      m_watched.insert(screen);const auto schedule=[this]{m_debounce.start();};
      connect(screen,&QScreen::geometryChanged,this,schedule);connect(screen,&QScreen::availableGeometryChanged,this,schedule);
      connect(screen,&QScreen::physicalSizeChanged,this,schedule);connect(screen,&QScreen::physicalDotsPerInchChanged,this,schedule);
      connect(screen,&QScreen::logicalDotsPerInchChanged,this,schedule);connect(screen,&QScreen::orientationChanged,this,schedule);
      connect(screen,&QScreen::refreshRateChanged,this,schedule);connect(screen,&QObject::destroyed,this,[this,screen]{m_watched.remove(screen);m_debounce.start();});
    }
    auto name=screen->name().left(128);name.remove(QRegularExpression("[\\x00-\\x1f\\x7f]"));if(name.isEmpty())name=QStringLiteral("Display %1").arg(screens.size()+1);
    const auto fingerprint=screen->name()+"\n"+screen->manufacturer()+"\n"+screen->model()+"\n"+screen->serialNumber();
    const auto id=hash(fingerprint+"\n"+QString::number(occurrences[fingerprint]++));const auto g=screen->geometry();const auto scale=screen->devicePixelRatio();const auto physical=screen->physicalSize();
    if(g.isEmpty())continue;
    auto optional=[](double v){return std::isfinite(v)&&v>0?QJsonValue(v):QJsonValue(QJsonValue::Null);};
    QJsonObject description{{"screenId",id},{"name",name},{"logicalWidth",g.width()},{"logicalHeight",g.height()},{"x",g.x()},{"y",g.y()},
      {"pixelWidth",qRound(g.width()*scale)},{"pixelHeight",qRound(g.height()*scale)},{"pixelSizeSource","qtGeometryTimesDpr"},{"scale",scale},
      {"refreshRate",optional(screen->refreshRate())},{"physicalWidthMm",optional(physical.width())},{"physicalHeightMm",optional(physical.height())},
      {"physicalSizeSource","systemReportedUnverified"},{"primary",screen==QGuiApplication::primaryScreen()},{"orientation",g.height()>g.width()?"portrait":"landscape"}};
    const auto native=m_nativeScreens.value(screen->name());
    const auto nativeScale=native.value("scale").toDouble();
    if(nativeScale>0&&std::abs(native.value("pixelWidth").toInt()-g.width()*nativeScale)<=2&&std::abs(native.value("pixelHeight").toInt()-g.height()*nativeScale)<=2){
      for(auto key:{"pixelWidth","pixelHeight","scale","refreshRate"})description.insert(key,native.value(key));
      description.insert("pixelSizeSource","gnomeDisplayConfig");
    }
#ifdef Q_OS_MACOS
    CGDirectDisplayID displays[32];uint32_t count=0;
    if(CGGetActiveDisplayList(32,displays,&count)==kCGErrorSuccess){
      for(uint32_t i=0;i<count;++i){const auto bounds=CGDisplayBounds(displays[i]);
        if(QRect(qRound(bounds.origin.x),qRound(bounds.origin.y),qRound(bounds.size.width),qRound(bounds.size.height))!=g)continue;
        if(auto mode=CGDisplayCopyDisplayMode(displays[i])){
          auto width=CGDisplayModeGetPixelWidth(mode),height=CGDisplayModeGetPixelHeight(mode);
          if(qRound(CGDisplayRotation(displays[i]))%180)std::swap(width,height);
          if(width&&height){description.insert("pixelWidth",static_cast<int>(width));description.insert("pixelHeight",static_cast<int>(height));description.insert("scale",double(width)/g.width());description.insert("pixelSizeSource","coreGraphicsDisplayMode");}
          const auto hz=CGDisplayModeGetRefreshRate(mode);if(hz>0)description.insert("refreshRate",hz);CGDisplayModeRelease(mode);
        }break;
      }
    }
#endif
    screens.append(description);
  }updateLocalScreens(screens);
}
void ScreenInventoryService::queryNativeScreens(){
#ifdef DESKFLOW_SCREEN_DBUS
  if(!m_watch||m_nativePending)return;
  auto message=QDBusMessage::createMethodCall("org.gnome.Mutter.DisplayConfig","/org/gnome/Mutter/DisplayConfig","org.gnome.Mutter.DisplayConfig","GetCurrentState");
  m_nativePending=true;auto watcher=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message,2000),this);
  connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,watcher]{
    m_nativePending=false;const auto reply=watcher->reply();watcher->deleteLater();QHash<QString,QJsonObject> native;
    if(reply.type()==QDBusMessage::ReplyMessage&&reply.arguments().size()==4){
      const auto physical=qvariant_cast<QDBusArgument>(reply.arguments().at(1));physical.beginArray();
      while(!physical.atEnd()){
        QString connector,vendor,product,serial;physical.beginStructure();physical.beginStructure();physical>>connector>>vendor>>product>>serial;physical.endStructure();physical.beginArray();
        while(!physical.atEnd()){
          QString mode;int width=0,height=0;double hz=0,preferred=0;QList<double> scales;QVariantMap properties;
          physical.beginStructure();physical>>mode>>width>>height>>hz>>preferred>>scales>>properties;physical.endStructure();
          auto current=properties.value("is-current");if(current.metaType()==QMetaType::fromType<QDBusVariant>())current=current.value<QDBusVariant>().variant();
          if(current.toBool()&&width>0&&height>0)native[connector]={{"pixelWidth",width},{"pixelHeight",height},{"refreshRate",hz}};
        }
        physical.endArray();QVariantMap properties;physical>>properties;physical.endStructure();
      }physical.endArray();
      const auto logical=qvariant_cast<QDBusArgument>(reply.arguments().at(2));logical.beginArray();
      while(!logical.atEnd()){
        int x=0,y=0;double scale=0;uint transform=0;bool primary=false;
        logical.beginStructure();logical>>x>>y>>scale>>transform>>primary;logical.beginArray();
        while(!logical.atEnd()){
          QString connector,vendor,product,serial;logical.beginStructure();logical>>connector>>vendor>>product>>serial;logical.endStructure();
          if(native.contains(connector)){auto& value=native[connector];value.insert("scale",scale);if(transform%2){const auto width=value.value("pixelWidth");value.insert("pixelWidth",value.value("pixelHeight"));value.insert("pixelHeight",width);}}
        }logical.endArray();QVariantMap properties;logical>>properties;logical.endStructure();
      }logical.endArray();
    }
    m_nativeScreens=native;refreshLocalScreens();
  });
#endif
}
void ScreenInventoryService::publish(const QString& peer,const QString& type){
  const auto c=m_connections.value(peer);if(c.isEmpty())return;
  QJsonObject body{{"protocol","device-screens/1"},{"type",type}};if(type=="inventory")body.insert("inventory",m_local);
  Q_EMIT outboundControl(peer,c.value("baseConnectionId").toString(),body);
}
void ScreenInventoryService::observeConnection(const QJsonObject& c){
  const auto peer=c.value("peerId").toString(),connection=c.value("baseConnectionId").toString();if(!identity(peer,64)||!identity(connection,32))return;
  if(!c.value("baseConnected").toBool()){
    if(m_connections.value(peer).value("baseConnectionId")!=connection)return;
    m_connections.remove(peer);if(m_peers.contains(peer)){m_peers[peer].insert("fresh",false);cache(peer);}Q_EMIT changed();return;
  }
  for(const auto& key:c.value("deviceKeys").toArray()){
    const auto device=key.toString();if(device.isEmpty()||device.size()>256)continue;
    m_devices[device]=peer;m_settings->setValue("bindings/"+hash(device),peer);
  }
  const bool fresh=m_connections.value(peer).value("baseConnectionId")!=connection;m_connections[peer]=c;
  if(fresh){if(m_peers.contains(peer))m_peers[peer].insert("fresh",false);publish(peer);publish(peer,"request");Q_EMIT changed();}
}
void ScreenInventoryService::receiveControl(const QJsonObject& event){
  const auto peer=event.value("peerId").toString();const auto c=m_connections.value(peer);
  if(c.isEmpty()||event.value("baseConnectionId")!=c.value("baseConnectionId"))return;
  const auto body=event.value("body").toObject();if(body.value("protocol")!="device-screens/1")return;
  if(body.value("type")=="request"){publish(peer);return;}
  if(body.value("type")!="inventory"||!body.value("inventory").isObject())return;
  const auto inventory=body.value("inventory").toObject();if(!validInventory(inventory))return;
  const auto old=m_peers.value(peer);const auto previous=old.value("inventory").toObject();
  if(old.value("fresh").toBool()&&previous.value("instanceId")==inventory.value("instanceId")&&inventory.value("revision").toString().toULongLong()<=previous.value("revision").toString().toULongLong())return;
  m_peers[peer]={{"inventory",inventory},{"fresh",true},{"lastSeenAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};cache(peer);Q_EMIT changed();
}
void ScreenInventoryService::cache(const QString& peer){
  auto value=m_peers.value(peer);value.insert("fresh",false);m_settings->setValue("peers/"+peer,QJsonDocument(value).toJson(QJsonDocument::Compact));
  m_settings->sync();QFile::setPermissions(m_settings->fileName(),QFile::ReadOwner|QFile::WriteOwner);
}
QJsonObject ScreenInventoryService::preference(const QString& peer)const{
  return {{"screenId",m_settings->value("preferences/"+peer+"/screenId").toString()},{"presentationMode",m_settings->value("preferences/"+peer+"/presentationMode","window").toString()}};
}
QJsonObject ScreenInventoryService::peerInventory(const QString& peer)const{
  if(!identity(peer,64))return {};auto result=m_peers.value(peer);
  if(result.isEmpty()){
    result=QJsonDocument::fromJson(m_settings->value("peers/"+peer).toByteArray()).object();
    if(!validInventory(result.value("inventory").toObject()))return {};result.insert("fresh",false);
  }
  result.insert("peerId",peer);result.insert("preference",preference(peer));result.insert("supportedStreamMode",fixedMode());
  result.insert("fresh",result.value("fresh").toBool()&&m_connections.contains(peer));return result;
}
QJsonObject ScreenInventoryService::forDevice(const QString& device)const{
  const auto peer=m_devices.value(device,m_settings->value("bindings/"+hash(device)).toString());return peerInventory(peer);
}
bool ScreenInventoryService::setPreference(const QString& peer,const QString& screenId,const QString& mode){
  const auto info=peerInventory(peer);if(!info.value("fresh").toBool()||(mode!="window"&&mode!="fullscreen")||(!screenId.isEmpty()&&findScreen(info.value("inventory").toObject(),screenId).isEmpty()))return false;
  m_settings->setValue("preferences/"+peer+"/screenId",screenId);m_settings->setValue("preferences/"+peer+"/presentationMode",mode);m_settings->sync();Q_EMIT changed();return true;
}
QJsonObject ScreenInventoryService::selectionForPeer(const QString& peer)const{
  const auto info=peerInventory(peer);if(!info.value("fresh").toBool())return {};const auto data=info.value("inventory").toObject();
  auto id=preference(peer).value("screenId").toString();
  if(id.isEmpty())for(const auto& value:data.value("screens").toArray()){const auto screen=value.toObject();if(id.isEmpty()||screen.value("primary").toBool())id=screen.value("screenId").toString();if(screen.value("primary").toBool())break;}
  if(findScreen(data,id).isEmpty())return {};return {{"instanceId",data.value("instanceId")},{"revision",data.value("revision")},{"screenId",id},{"presentationMode",preference(peer).value("presentationMode")},{"streamMode",fixedMode()}};
}
bool ScreenInventoryService::resolveLocalSelection(const QJsonObject& selection,QString* screenName){
  refreshLocalScreens();if(selection.value("instanceId")!=m_local.value("instanceId")||selection.value("revision")!=m_local.value("revision")||selection.value("streamMode").toObject()!=fixedMode()||(selection.value("presentationMode")!="window"&&selection.value("presentationMode")!="fullscreen"))return false;
  const auto screen=findScreen(m_local,selection.value("screenId").toString());if(screen.isEmpty())return false;if(screenName)*screenName=screen.value("name").toString();return true;
}
}
