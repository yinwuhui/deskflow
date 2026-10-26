// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "CoreInputChannel.h"
#include "common/Constants.h"
#include "common/VersionInfo.h"
#include <QJsonDocument>
#include <QJsonArray>
namespace deskflow::display {
CoreInputChannel::CoreInputChannel(InputRouteAdapter* adapter,PeerUsageService* usage,QObject* parent,const QString& endpoint,bool enabled):QObject(parent),m_endpoint(endpoint.isEmpty()?kCoreIpcName:endpoint),m_adapter(adapter),m_usage(usage){
  if(!enabled)return; // Internal manual harness; never selected by GUI commands.
  connect(adapter,&InputRouteAdapter::coreCommand,this,&CoreInputChannel::send);
  m_socket.setReadBufferSize(65536);m_poll.setInterval(500);connect(&m_poll,&QTimer::timeout,this,&CoreInputChannel::poll);m_poll.start();
  QTimer::singleShot(0,this,&CoreInputChannel::poll);
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
  if(m_socket.state()==QLocalSocket::UnconnectedState)m_socket.connectToServer(m_endpoint);
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
      }else if(line=="peerUsageSnapshotBegin"){
        m_seen.clear();m_scanning=true;
      }else if(line.startsWith("peerUsageConnection=")){
        auto doc=QJsonDocument::fromJson(line.mid(sizeof("peerUsageConnection=")-1));if(doc.isObject())observe(doc.object());
      }else if(line.startsWith("peerUsageConnectionClosed=")){
        const auto closed=QJsonDocument::fromJson(line.mid(sizeof("peerUsageConnectionClosed=")-1)).object();
        const auto id=closed.value("baseConnectionId").toString();
        if(m_records.contains(id)&&m_records.value(id).value("peerId")==closed.value("peerId")){
          const auto record=m_records.take(id);m_seen.remove(id);
          m_usage->connectionClosed(record.value("peerId").toString(),id,m_generations.take(id),closed.value("inputRevoked").toBool());
        }
      }else if(line.startsWith("peerUsageSnapshotEnd=")){
        if(m_scanning){
          const auto known=m_records.keys();
          for(const auto& id:known)if(!m_seen.contains(id)){
            auto record=m_records.take(id);m_usage->connectionClosed(record.value("peerId").toString(),id,m_generations.take(id));
          }
          m_scanning=false;
        }
      }else if(line.startsWith("peerUsageControlEvent=")){
        auto doc=QJsonDocument::fromJson(line.mid(sizeof("peerUsageControlEvent=")-1));if(doc.isObject())Q_EMIT peerControlReceived(doc.object());
      }else if(line.startsWith("inputControlResult=")){
        auto document=QJsonDocument::fromJson(line.mid(sizeof("inputControlResult=")-1));
        if(document.isObject())m_adapter->coreResult(document.object());
      }
    }
  }
}
void CoreInputChannel::failed(){
  m_authenticated=false;m_buffer.clear();m_pending.clear();m_adapter->coreUnavailable();
  auto records=m_records;m_records.clear();m_seen.clear();m_scanning=false;
  for(auto it=records.begin();it!=records.end();++it)m_usage->connectionClosed(it.value().value("peerId").toString(),it.key(),m_generations.take(it.key()));
}
void CoreInputChannel::poll(){
  if(m_authenticated){if(m_socket.bytesToWrite()<65536)m_socket.write("peerUsageSnapshot\n");}
  else if(m_socket.state()==QLocalSocket::UnconnectedState)m_socket.connectToServer(m_endpoint);
}
void CoreInputChannel::observe(const QJsonObject& record){
  const auto peer=record.value("peerId").toString(),id=record.value("baseConnectionId").toString();
  if(peer.size()!=64||id.size()!=32||!record.value("baseConnected").isBool()||!record.value("inputControlSupported").isBool()||
    !record.value("inputActive").isBool()||!record.value("inputPermissionsReady").isBool())return;
  m_seen.insert(id);if(m_records.value(id)==record)return;
  if(!m_generations.contains(id))m_generations[id]=++m_nextGeneration;
  PeerUsageService::BaseConnection connection;connection.id=id.toStdString();connection.generation=m_generations.value(id);
  connection.connected=record.value("baseConnected").toBool();
  auto role=record.value("originalInputRole").toString();connection.role=role=="source"?PeerUsageService::InputRole::Source:role=="receiver"?PeerUsageService::InputRole::Receiver:PeerUsageService::InputRole::Unknown;
  connection.permission=record.value("inputPermissionsReady").toBool()?Permission::Granted:Permission::Unknown;
  connection.inputActive=record.value("inputActive").toBool();connection.focusOwner=record.value("inputFocusOwner").toString().toStdString();
  for(const auto& key:record.value("deviceKeys").toArray())connection.deviceKeys.push_back(key.toString().toStdString());
  const auto supported=record.value("inputControlSupported").toBool()?Availability::Supported:Availability::Checking;
  UsageCapabilities caps;caps.input=caps.keyboard=caps.focusSync=caps.targetIsolation=caps.inputRelease=supported;
  caps.displayRouting=supported;
  // Display readiness is supplied by the authenticated display capability probe.
  auto existing=m_usage->snapshot(peer).value("inputCapabilities").toObject();
  if(existing.value("extended")=="supported")caps.extended=Availability::Supported;
  caps.networkAvailable=existing.value("networkAvailable").toBool();
  Peer known{peer.toStdString(),record.value("peerName").toString().toStdString(),{}};
  auto outcome=m_usage->syncConnectedPeer(std::move(known),caps,std::move(connection));
  if(outcome.accepted||outcome.reason==Reason::CleanupFailed){m_records[id]=record;Q_EMIT authenticatedConnectionObserved(record);}
}
void CoreInputChannel::sendPeerControl(const QString& peer,const QString& connection,const QJsonObject& body){
  if(!m_authenticated||!m_records.contains(connection)||m_records.value(connection).value("peerId")!=peer)return;
  auto bytes=QJsonDocument(QJsonObject{{"kind","control"},{"peerId",peer},{"baseConnectionId",connection},{"body",body}}).toJson(QJsonDocument::Compact);
  if(bytes.size()<=16384&&m_socket.bytesToWrite()<65536)m_socket.write("inputControl="+bytes+'\n');
}
}
