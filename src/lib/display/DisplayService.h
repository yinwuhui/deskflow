// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "Coordinator.h"
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSettings>
#include <QTimer>
#include <memory>

namespace deskflow::display {
// GUI-facing product API. Independent of the legacy keyboard server/client role.
// execute() schedules work; adapter completions are not accepted as GUI commands.
class DisplayService : public QObject {
  Q_OBJECT
public:
  explicit DisplayService(Capabilities local, const QString& preferencesFile, QObject* parent = nullptr);
  static DisplayService* applicationService();
  QJsonObject snapshot() const;
  Key currentKey() const { ownerThread(); return m_coordinator.snapshot().key; }
  QJsonObject localCapabilities() const;
  void execute(const QJsonObject& command);
  // Adapter-only entry points. Call on the owner thread after authentication/probing.
  void updateCapabilities(Capabilities local);
  Outcome requestManagedSession(const QString& peerId, SessionId request);
  Outcome resumeManagedSession(const Key& key);
  Outcome stopManagedSession(const Key& key, Reason reason=Reason::Cancelled);
  void authenticatedPeer(Peer peer);
  void forgetPeer(const QString& identity);
  Outcome incomingAuthenticated(Peer peer, SessionId session, SessionId request);
  void peerResponse(const Key& key, bool allowed, Reason reason);
  void peerReady(const Key& key);
  void permissionResult(const Key& key, Permission permission);
  void backendPrepared(const Key& key, const QString& displayId);
  void firstFramePresented(const Key& key);
  void backendFailed(const Key& key, Reason reason);
  void cleanupFinished(const Key& key, bool verified);
  void setInputRouting(Routing routing);
  bool restorePendingResource(Peer peer, const Key& owner, Role role, const QString& displayId);
Q_SIGNALS:
  void snapshotChanged(const QJsonObject& snapshot);
  void commandFinished(const QJsonObject& reply);
  void backendCommand(const QJsonObject& effect);
private:
  void runCommand(const QJsonObject& command);
  void publish();
  void ownerThread() const;
  Coordinator m_coordinator;
  QHash<QString, Peer> m_peers;
  Routing m_routing = Routing::Unknown;
  std::unique_ptr<QSettings> m_preferences;
  QTimer m_timer;
  uint64_t m_publishedRevision = 0;
};
} // namespace deskflow::display
