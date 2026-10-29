// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "InputRouteAdapter.h"
#include <QLocalSocket>
#include <QSet>
#include <QHash>
#include <QTimer>
namespace deskflow::display {
// Same-user local core IPC. No TCP endpoint, executable path or unauthenticated
// peer identifier can be supplied by the GUI through this transport.
class CoreInputChannel : public QObject {
  Q_OBJECT
public:
  explicit CoreInputChannel(InputRouteAdapter* adapter,PeerUsageService* usage,QObject* parent=nullptr,const QString& endpoint={},bool enabled=true);
  bool sendPeerControl(const QString& peerId,const QString& connectionId,const QJsonObject& body);
Q_SIGNALS:
  void peerControlReceived(const QJsonObject& event);
  void authenticatedConnectionObserved(const QJsonObject& connection);
private:
  QString m_endpoint;
  void send(const QJsonObject& command);
  void read();
  void failed();
  void observe(const QJsonObject& record);
  void poll();
  QLocalSocket m_socket;
  QByteArray m_buffer;
  QList<QByteArray> m_pending;
  bool m_authenticated=false;
  InputRouteAdapter* m_adapter;
  PeerUsageService* m_usage;
  QHash<QString,QJsonObject> m_records;
  QSet<QString> m_seen;
  QHash<QString,uint64_t> m_generations;
  uint64_t m_nextGeneration=0;
  QTimer m_poll;
  bool m_scanning=false;
};
}
