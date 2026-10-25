// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "ResourceJournal.h"
#include <QDir>
#include <algorithm>
#include <cmath>
#include <QSet>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
namespace deskflow::display {
namespace {
QString hex(const SessionId& id) { return QByteArray(reinterpret_cast<const char*>(id.data()),16).toHex(); }
bool idText(const QString& value, int size) {
  if(value.size()!=size)return false;
  for(auto c:value)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
  return true;
}
bool valid(const OwnedResource& record) {
  return record.owner.session!=SessionId{} && record.owner.request!=SessionId{} && record.owner.generation>0 &&
    record.owner.epoch>0 && idText(record.peerIdentity,64) && (record.role==Role::Host || record.role==Role::Receiver) &&
    record.displayId.size()<=256 && !record.displayId.contains(QChar::Null);
}
}
ResourceJournal::ResourceJournal(const QString& directory) {
  if(directory.isEmpty() || QFileInfo(directory).isSymLink())return;
  QDir dir(directory);if(!dir.exists() && !QDir().mkpath(directory))return;
  if(!QFile::setPermissions(directory,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner))return;
  m_lock=std::make_unique<QLockFile>(dir.filePath("owner.lock"));m_lock->setStaleLockTime(0);
  if(!m_lock->tryLock(0))return;
  m_path=dir.filePath("active.json");
  if(!QFileInfo::exists(m_path)){m_status=Status::Empty;return;}
  if(QFileInfo(m_path).isSymLink()){m_status=Status::Corrupt;return;}
  QFile file(m_path);if(!file.open(QFile::ReadOnly))return;
  if(file.size()>16384){m_status=Status::Corrupt;return;}
  QJsonParseError error;const auto doc=QJsonDocument::fromJson(file.readAll(),&error);
  if(error.error!=QJsonParseError::NoError || !doc.isObject()){m_status=Status::Corrupt;return;}
  const auto object=doc.object();
  if(object.size()!=2 || object.value("schema")!=1 || !object.contains("active")){m_status=Status::Corrupt;return;}
  if(object.value("active").isNull()){m_status=Status::Empty;return;}
  const auto active=object.value("active").toObject();
  const QSet<QString> keys{"sessionId","requestId","ownerGeneration","displayEpoch","peerIdentity","displayId","role"};
  for(auto it=active.begin();it!=active.end();++it)if(!keys.contains(it.key())){m_status=Status::Corrupt;return;}
  const auto session=active.value("sessionId").toString(),request=active.value("requestId").toString();
  bool ok=false;const auto generation=active.value("ownerGeneration").toString().toULongLong(&ok);
  const auto epoch=active.value("displayEpoch").toDouble(-1);
  if(!idText(session,32)||!idText(request,32)||!ok||generation==0||QString::number(generation)!=active.value("ownerGeneration").toString()||
     !std::isfinite(epoch)||epoch<1||epoch>UINT32_MAX||epoch!=static_cast<uint32_t>(epoch)||active.size()!=7) {m_status=Status::Corrupt;return;}
  OwnedResource resource;
  auto bytes=QByteArray::fromHex(session.toLatin1());std::copy(bytes.begin(),bytes.end(),resource.owner.session.begin());
  bytes=QByteArray::fromHex(request.toLatin1());std::copy(bytes.begin(),bytes.end(),resource.owner.request.begin());
  resource.owner.generation=generation;resource.owner.epoch=static_cast<uint32_t>(epoch);
  resource.peerIdentity=active.value("peerIdentity").toString();resource.displayId=active.value("displayId").toString();
  auto role=active.value("role").toString();resource.role=role=="host"?Role::Host:role=="receiver"?Role::Receiver:Role::None;
  if(!valid(resource)){m_status=Status::Corrupt;return;}
  m_record=resource;m_status=Status::Pending;
}
bool ResourceJournal::write(const std::optional<OwnedResource>& resource) {
  if(m_status==Status::Unavailable || m_status==Status::Corrupt || QFileInfo(m_path).isSymLink())return false;
  QJsonValue active=QJsonValue::Null;
  if(resource)active=QJsonObject{{"sessionId",hex(resource->owner.session)},{"requestId",hex(resource->owner.request)},
    {"ownerGeneration",QString::number(resource->owner.generation)},{"displayEpoch",static_cast<double>(resource->owner.epoch)},
    {"peerIdentity",resource->peerIdentity},{"displayId",resource->displayId},{"role",roleName(resource->role)}};
  QSaveFile file(m_path);file.setDirectWriteFallback(false);
  if(!file.open(QFile::WriteOnly)||!file.setPermissions(QFile::ReadOwner|QFile::WriteOwner))return false;
  const auto data=QJsonDocument(QJsonObject{{"schema",1},{"active",active}}).toJson(QJsonDocument::Compact);
  if(file.write(data)!=data.size() || !file.commit())return false;
  m_record=resource;m_status=resource?Status::Pending:Status::Empty;return true;
}
bool ResourceJournal::claim(const OwnedResource& resource) {
  if(m_status!=Status::Empty || !valid(resource))return false;
  return write(resource);
}
bool ResourceJournal::updateDisplay(const Key& owner,const QString& displayId) {
  if(!m_record || m_record->owner!=owner)return false;
  auto record=*m_record;record.displayId=displayId;if(!valid(record))return false;return write(record);
}
bool ResourceJournal::clearVerified(const Key& owner,bool verified) {
  if(!verified || !m_record || m_record->owner!=owner)return false;
  return write(std::nullopt);
}
} // namespace deskflow::display
