// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "CoreInputChannel.h"
#include "common/Constants.h"
#include "common/VersionInfo.h"
#include <QJsonDocument>
namespace deskflow::display {
CoreInputChannel::CoreInputChannel(InputRouteAdapter* adapter,QObject* parent):QObject(parent),m_adapter(adapter){
  connect(adapter,&InputRouteAdapter::coreCommand,this,&CoreInputChannel::send);
  connect(&m_socket,&QLocalSocket::connected,this,[this]{
    m_socket.write(QStringLiteral("hello=%1+%2\n").arg(kVersion,kVersionGitSha).toUtf8());
  });
  connect(&m_socket,&QLocalSocket::readyRead,this,&CoreInputChannel::read);
  connect(&m_socket,&QLocalSocket::disconnected,this,&CoreInputChannel::failed);
  connect(&m_socket,&QLocalSocket::errorOccurred,this,[this]{failed();});
}
void CoreInputChannel::send(const QJsonObject& command){
  auto bytes="inputControl="+QJsonDocument(command).toJson(QJsonDocument::Compact)+'\n';
  if(bytes.size()>65536||m_pending.size()>=32||m_socket.bytesToWrite()>65536){failed();return;}
  if(m_authenticated){m_socket.write(bytes);return;}
  m_pending.append(bytes);
  if(m_socket.state()==QLocalSocket::UnconnectedState)m_socket.connectToServer(kCoreIpcName);
}
void CoreInputChannel::read(){
  while(m_socket.bytesAvailable()>0){
    const auto room=65536-m_buffer.size();if(room<=0){failed();m_socket.abort();return;}
    m_buffer.append(m_socket.read(room));
    int newline=0;
    while((newline=m_buffer.indexOf('\n'))>=0){
      auto line=m_buffer.left(newline);m_buffer.remove(0,newline+1);
      if(!m_authenticated){
        if(line!=QStringLiteral("hello=%1+%2").arg(kVersion,kVersionGitSha).toUtf8()){failed();m_socket.abort();return;}
        m_authenticated=true;
        auto pending=std::move(m_pending);m_pending.clear();for(const auto& command:pending)m_socket.write(command);
      }else if(line.startsWith("inputControlResult=")){
        auto document=QJsonDocument::fromJson(line.mid(sizeof("inputControlResult=")-1));
        if(document.isObject())m_adapter->coreResult(document.object());
      }
    }
  }
}
void CoreInputChannel::failed(){m_authenticated=false;m_buffer.clear();m_pending.clear();m_adapter->coreUnavailable();}
}
