// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "deskflow/ProtocolUtil.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <string>
namespace deskflow::input {
inline bool readUsageControl(IStream* stream,QJsonObject& body)
{
  uint32_t length=0;
  if(!ProtocolUtil::readf(stream,"%4i",&length)||!length||length>16384)return false;
  std::string payload;
  const auto format="%"+std::to_string(length)+"s";
  if(!ProtocolUtil::readf(stream,format.c_str(),&payload))return false;
  const auto doc=QJsonDocument::fromJson(QByteArray::fromStdString(payload));
  if(!doc.isObject())return false;body=doc.object();return true;
}
inline bool writeUsageControl(IStream* stream,const char* opcode,const QJsonObject& body)
{
  const auto payload=QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString();
  if(payload.empty()||payload.size()>16384)return false;
  ProtocolUtil::writef(stream,opcode,&payload);return true;
}
}
