// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "UsageCoordinator.h"
#include <QObject>
#include <QTimer>
#include <QJsonObject>
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
  explicit PeerUsageService(QObject* parent=nullptr);
  static PeerUsageService* applicationService();
  void execute(const QJsonObject& command);
  QJsonObject snapshot(const QString& peerId) const;
  void authenticatedPeer(Peer peer, UsageCapabilities capabilities);
  void disconnected(const QString& peerId);
  bool complete(const QString& peerId, const UsageKey& key, Completion kind,
                Reason reason=Reason::None, bool verified=false);
  bool inputStatus(const QString& peerId, const UsageKey& key, Permission permission, const QString& focusOwner);
Q_SIGNALS:
  void commandFinished(const QJsonObject& reply);
  void snapshotChanged(const QJsonObject& snapshot);
  // Internal driver contract; never connect this signal to a GUI command parser.
  void backendCommand(const QJsonObject& command);
private:
  struct Entry {
    Peer peer;
    UsageCapabilities capabilities;
    std::unique_ptr<UsageCoordinator> coordinator;
    uint64_t published=0;
  };
  void ownerThread() const;
  void run(const QJsonObject& command);
  void publish(const QString& peerId);
  QJsonObject capabilities(const Entry& entry) const;
  bool displayBusy(const QString& peerId) const;
  std::map<QString,Entry> m_peers;
  QString m_selectedPeer;
  QTimer m_timer;
};
}
