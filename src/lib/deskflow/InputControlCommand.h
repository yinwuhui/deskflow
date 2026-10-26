// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "base/Event.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <limits>
namespace deskflow::input {
class InputControlCommandData : public EventData
{
public:
  explicit InputControlCommandData(QString text) : json(std::move(text)) {}
  QString json;
};
inline bool lowerHex(const QString& text,int size)
{
  if(text.size()!=size)return false;
  for(auto c:text)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
  return true;
}
template<class Route> QJsonObject executeInputControl(const Event& event, Route* route)
{
  const auto* data=static_cast<const InputControlCommandData*>(event.getDataObject());
  const auto document=QJsonDocument::fromJson(data->json.toUtf8());
  const auto command=document.object();
  auto result=QJsonObject{{"accepted",false},{"reasonCode","InvalidRequest"}};
  if(command.value("kind")=="control"){
    const auto peer=command.value("peerId").toString(),connection=command.value("baseConnectionId").toString();
    const bool valid=document.isObject()&&command.size()==4&&lowerHex(peer,64)&&lowerHex(connection,32)&&
      command.value("body").isObject()&&data->json.toUtf8().size()<=16384;
    const bool accepted=valid&&route&&route->sendUsageControl(peer,connection,command.value("body").toObject());
    return {{"accepted",accepted},{"reasonCode",accepted?"None":"ConnectionLost"},{"kind","control"}};
  }
  const QSet<QString> required{"id","kind","peerId","sequence","operationId","operationGeneration","enabled","baseConnectionId","baseConnectionGeneration"};
  bool valid=document.isObject() && data->json.size()<=65536 && command.size()==required.size();
  for(const auto& key:required)valid=valid&&command.contains(key);
  const auto peer=command.value("peerId").toString();
  const auto operation=command.value("operationId").toString();
  const auto sequenceText=command.value("sequence").toString();
  bool sequenceValid=false;auto sequence=sequenceText.toULongLong(&sequenceValid);
  const auto generationText=command.value("operationGeneration").toString();
  bool generationValid=false;auto generation=generationText.toULongLong(&generationValid);
  const auto kind=command.value("kind").toString();
  const auto id=command.value("id").toString();
  const auto enabled=command.value("enabled");
  const auto binding=command.value("baseConnectionId").toString(),bindingGeneration=command.value("baseConnectionGeneration").toString();
  bool bindingValid=false;auto bindingNumber=bindingGeneration.toULongLong(&bindingValid);
  valid=valid&&lowerHex(binding,32)&&bindingValid&&bindingNumber>0&&QString::number(bindingNumber)==bindingGeneration;
  valid=valid&&!id.isEmpty()&&id.size()<=64&&lowerHex(peer,64)&&lowerHex(operation,32)&&operation!=QString(32,'0')&&
    sequenceValid&&sequence<=std::numeric_limits<uint32_t>::max()&&QString::number(sequence)==sequenceText&&
    generationValid&&generation>0&&QString::number(generation)==generationText&&
    (kind=="probe"||kind=="status"||kind=="suspend"||kind=="resume"||kind=="commitResume")&&enabled.isBool()&&
    (kind=="status"||kind=="probe"||(kind=="suspend"?!enabled.toBool():enabled.toBool()));
  if(valid){
    for(auto key:{"id","peerId","sequence","operationId","operationGeneration","kind","enabled","baseConnectionId","baseConnectionGeneration"})result.insert(key,command.value(key));
    if(!route||!route->matchesUsageConnection(peer,binding)){
      result.insert("reasonCode","ConnectionLost");result.insert("connected",false);result.insert("supported",false);
    }
    else{
      bool accepted=kind=="status";
      if(kind=="probe")accepted=route->requestInputProbe(peer.toStdString(),static_cast<uint32_t>(sequence));
      else if(kind=="suspend"||kind=="resume")accepted=route->requestInputRoute(peer.toStdString(),static_cast<uint32_t>(sequence),kind=="resume");
      else if(kind=="commitResume")accepted=route->commitInputResume(peer.toStdString(),static_cast<uint32_t>(sequence));
      const auto status=route->inputRouteStatus(peer.toStdString(),static_cast<uint32_t>(sequence),enabled.toBool());
      result.insert("accepted",accepted);result.insert("reasonCode",accepted?"None":!status.connected?"ConnectionLost":!status.supported?"InputIsolationUnsupported":"InputReleaseFailed");
      result.insert("connected",status.connected);result.insert("supported",status.supported);result.insert("suspended",status.suspended);
      result.insert("inputPermissionsReady",status.permissionsReady);
      result.insert("acknowledgementReceived",status.responseReceived);
      result.insert("lastSequence",QString::number(status.lastSequence));
      result.insert("otherRoutesIsolated",status.otherRoutesIsolated);
      result.insert("localFocus",status.localFocus);result.insert("acknowledged",status.acknowledged);
    }
  }
  return result;
}
}
