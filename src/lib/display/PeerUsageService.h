// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "UsageCoordinator.h"
#include <QObject>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>
#include <optional>
#include <map>
#include <memory>
namespace deskflow::display {
// Application-facing transaction API. Only authenticated adapters can supply
// capability, permission, route and presentation acknowledgements.
class PeerUsageService : public QObject {
  Q_OBJECT
public:
  enum class Completion { Preflight, Permission, InputsReleased, InputStopped, InputCleaned,
    DisplayPresented, DisplayEnded, DisplayFailed, DisplayStopped, InputStarted, RollbackAuthorization };
  enum class InputRole { Unknown, Source, Receiver };
  struct BaseConnection {
    std::string id;
    uint64_t generation=0;
    bool connected=false;
    InputRole role=InputRole::Unknown;
    bool inputActive=false;
    Permission permission=Permission::Unknown;
    std::string focusOwner;
    std::vector<std::string> deviceKeys;
  };
  explicit PeerUsageService(QObject* parent=nullptr,const QString& preferencesFile={});
  static PeerUsageService* applicationService();
  void execute(const QJsonObject& command);
  QJsonObject snapshot(const QString& peerId) const;
  void authenticatedPeer(Peer peer, UsageCapabilities capabilities);
  void disconnected(const QString& peerId,bool inputReleaseVerified=false);
  Outcome beginIncomingDisplay(const QString& peerId,SessionId request);
  void updateDisplayCapabilities(const QString& peerId,Availability outgoing,Availability incoming,bool networkAvailable);
  Outcome syncConnectedPeer(Peer peer,UsageCapabilities capabilities,BaseConnection connection);
  bool connectionClosed(const QString& peerId,const QString& connectionId,uint64_t generation,bool inputReleaseVerified=false);
  QJsonArray connectedPeers() const;
  QJsonArray retainedSnapshots() const;
  Outcome restoreDisplayCleanup(Peer peer,SessionId owner);
  Outcome retryManagedCleanup(const QString& peerId);
  void peerStopped(const QString& peerId,bool cancel);
  void invalidateRollback(const QString& peerId);
  bool complete(const QString& peerId, const UsageKey& key, Completion kind,
                Reason reason=Reason::None, bool verified=false);
  bool inputStatus(const QString& peerId, const UsageKey& key, Permission permission, const QString& focusOwner);
Q_SIGNALS:
  void commandFinished(const QJsonObject& reply);
  void snapshotChanged(const QJsonObject& snapshot);
  void connectionChanged(const QJsonObject& connection);
  // Internal driver contract; never connect this signal to a GUI command parser.
  void backendCommand(const QJsonObject& command);
private:
  struct Entry {
    Peer peer;
    UsageCapabilities capabilities;
    std::unique_ptr<UsageCoordinator> coordinator;
    uint64_t published=0;
    BaseConnection connection;
    std::optional<BaseConnection> pendingConnection;
    UsageCapabilities pendingCapabilities;
    Peer pendingPeer;
  };
  void ownerThread() const;
  void run(const QJsonObject& command);
  void publish(const QString& peerId);
  QJsonObject capabilities(const Entry& entry) const;
  bool displayBusy(const QString& peerId) const;
  QJsonObject connectionJson(const QString& peerId,const Entry& entry) const;
  std::map<QString,Entry> m_peers;
  QString m_selectedPeer;
  std::unique_ptr<QSettings> m_preferences;
  QTimer m_timer;
};
}
