// SPDX-License-Identifier: MIT
#pragma once
#include "LocalTransportIdentity.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QString>
#include <QUuid>
#include <map>
#include <mutex>

// Authenticated core-thread observations, consumed by the same-user GUI IPC.
// This registry cannot be populated by a frontend command.
class PeerConnectionRegistry
{
public:
  static void update(const QJsonObject& connection)
  {
    const auto id=connection.value("baseConnectionId").toString();
    if(id.size()!=32||connection.value("peerId").toString().size()!=64)return;
    std::lock_guard lock(m_mutex);if(!m_connections.contains(id)&&m_connections.size()>=256)return;m_connections[id]=connection;
  }
  static void remove(const QString& id,bool inputRevoked=false)
  {
    std::lock_guard lock(m_mutex);auto found=m_connections.find(id);
    if(found!=m_connections.end()){
      if(m_closed.size()>=256)m_closed.erase(m_closed.begin());
      m_closed[id]={{"peerId",found->second.value("peerId")},{"baseConnectionId",id},{"inputRevoked",inputRevoked}};
      m_connections.erase(found);
    }
  }
  static void uiContext(const QString& peer,const QJsonObject& context){
    if(peer.size()!=64)return;
    std::lock_guard lock(m_mutex);if(m_context.size()>=256&&!m_context.contains(peer))m_context.erase(m_context.begin());m_context[peer]=context;
  }
  static QJsonObject snapshot()
  {
    std::lock_guard lock(m_mutex);QJsonArray peers,events;
    for(const auto& [id,connection]:m_connections){
      auto record=connection;
      // Direct native TLS connections use the network. Managed tunnels override
      // this from their explicit policy, regardless of the currently active lane.
      auto policy=m_context.find(record.value("peerId").toString());
      const bool managed=LocalTransportIdentity::containsPeer(record.value("peerId").toString().toStdString());
      record.insert("displayNetworkPolicy",policy==m_context.end()?QJsonValue(managed?"unknown":"networkAllowed"):policy->second.value("displayNetworkPolicy"));
      if(record.value("originalInputRole")=="receiver"){
        auto context=m_context.find(record.value("peerId").toString());
        if(context!=m_context.end()){record.insert("peerName",context->second.value("peerName"));record.insert("deviceKeys",context->second.value("deviceKeys"));}
      }
      peers.append(record);
    }
    for(const auto& event:m_events)events.append(event);m_events.clear();
    QJsonArray closed;for(const auto& [id,record]:m_closed)closed.append(record);m_closed.clear();
    return {{"connections",peers},{"events",events},{"closed",closed}};
  }
  static bool controlReceived(const QString& peer,const QString& connection,const QJsonObject& body)
  {
    if(QJsonDocument(body).toJson(QJsonDocument::Compact).size()>16384)return false;
    std::lock_guard lock(m_mutex);
    if(!m_connections.contains(connection)||m_connections.at(connection).value("peerId")!=peer||m_events.size()>=128)return false;
    m_events.push_back({{"peerId",peer},{"baseConnectionId",connection},{"body",body}});return true;
  }
private:
  inline static std::mutex m_mutex;
  inline static std::map<QString,QJsonObject> m_connections;
  inline static QList<QJsonObject> m_events;
  inline static std::map<QString,QJsonObject> m_closed,m_context;
};
