// SPDX-License-Identifier: MIT
#pragma once
#include "CoreInputChannel.h"
#include "MediaSupervisor.h"
#include "UsageDisplayBridge.h"
#include <QProcess>
namespace deskflow::display {
// Owns the product composition. Only the authenticated core can introduce peers.
struct ProductDisplayConfiguration {
  MediaRuntime media;
  QString certificate;
  QString resources;
  QString scriptDirectory;
};
class ProductDisplayRuntime final : public QObject {
  Q_OBJECT
public:
  ProductDisplayRuntime(PeerUsageService*,DisplayService*,UsageDisplayBridge*,CoreInputChannel*,QObject* parent=nullptr,const ProductDisplayConfiguration* configuration=nullptr);
  ~ProductDisplayRuntime() override;
Q_SIGNALS:
  void outboundControl(const QString& peer,const QString& connection,const QJsonObject& body);
private:
  void probe(bool requestPermission=false);
  void observed(const QJsonObject&);
  void received(const QJsonObject&);
  void effect(const QJsonObject&);
  void send(const QString&,QJsonObject);
  void hello(const QString&);
  void capabilities(const QString&);
  QJsonObject wire(const QString&) const;
  void resourceState(const QJsonObject&);
  void localCleanup(const QJsonObject&);
  void finishCleanup();
  QString proofKey(const QString&,const QJsonObject&)const;
  void rememberProof(const QString&,const QJsonObject&);
  bool current(const QString&,const QJsonObject&) const;
  PeerUsageService* m_usage;
  DisplayService* m_display;
  UsageDisplayBridge* m_bridge;
  CoreInputChannel* m_channel;
  MediaSupervisor* m_media=nullptr;
  MediaRuntime m_runtime;
  Capabilities m_local;
  QString m_permission="unknown",m_root,m_publicCertificate,m_peer;
  QHash<QString,QJsonObject> m_connections,m_remote;
  QJsonObject m_wire,m_cleanupReply;
  QString m_inputStateFile,m_resourcePeer,m_displayBase;
  Key m_cleanupKey;
  bool m_localResourceGone=false,m_peerTermination=false;
  bool m_haveLocalCleanup=false,m_localCleanup=false,m_havePeerCleanup=false,m_peerCleanup=false;
  QSet<QString> m_cleanupProofs;
  QProcess* m_probe=nullptr;
  QTimer m_probeTimer;
  bool m_permissionPending=false,m_receiverReady=false;
};
}
