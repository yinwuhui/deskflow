// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "net/Fingerprint.h"
#include "net/MdnsCommon.h"
#include <QUuid>
#include <QWidget>
#include <functional>
class QVBoxLayout;
class QLabel;
namespace deskflow::gui::workspace {
class SetupAssistant : public QWidget
{
  Q_OBJECT
public:
  enum class Step
  {
    Role,
    Permission,
    Wait,
    Discover,
    Manual,
    Identity,
    AwaitPeer,
    RegisterDevice,
    Position,
    Verify,
    Done
  };
  explicit SetupAssistant(QWidget *parent = nullptr);
  void begin(bool source, bool chooseRole, const QStringList &existingPeers, const QString &savedAddress, int port);
  bool active() const
  {
    return m_active;
  }
  bool identityPending() const
  {
    return m_identityPending;
  }
  Step step() const
  {
    return m_step;
  }
  QUuid attempt() const
  {
    return m_attempt;
  }
  void setCandidates(const QUuid &attempt, const QVector<deskflow::mdns::DiscoveredServer> &servers);
  void requestIdentity(const Fingerprint &peer, const Fingerprint &local, std::function<bool(bool)> decision);
  void setIdentityContext(const QString &identifier, const QByteArray &previous);
  bool showIncomingDevice(const QString &identifier);
  void waitForPeer();
  void showCompletion(bool verified, const QString &message);
  void showIdentityCompletion();
  void observeConnected(const QString &identifier);
  void observeDisconnected(const QString &identifier);
  void showFailure(const QString &message);
  void cancel();
Q_SIGNALS:
  void connectRequested(bool source, const QString &address, int port);
  void discoveryRequested(const QUuid &attempt);
  void permissionHelpRequested();
  void localFingerprintRequested();
  void canceled();
  void finished(const QString &verifiedTarget);
  void dismissed();
  void arrangeRequested();
  void deviceRequested(const QString &identifier);

private:
  void render();
  void go(Step step);
  void startAttempt();
  bool permissionAvailable() const;
  QWidget *m_content = nullptr;
  QVBoxLayout *m_layout = nullptr;
  QLabel *m_error = nullptr;
  bool m_active = false, m_source = true, m_chooseRole = false, m_attemptStarted = false, m_connected = false,
       m_identityPending = false;
  bool m_localApproved = false;
  bool m_completionVerified = false;
  bool m_identityOnlyCompletion = false;
  quint64 m_connectionRevision = 0, m_identityRequest = 0;
  Step m_step = Step::Role;
  QUuid m_attempt;
  QStringList m_existingPeers;
  QString m_address, m_target, m_failure, m_identityIdentifier, m_requestedIdentifier;
  QByteArray m_previousIdentity;
  int m_port = 24800;
  Fingerprint m_peer, m_local;
  QVector<deskflow::mdns::DiscoveredServer> m_servers;
  std::function<bool(bool)> m_decision;
};
} // namespace deskflow::gui::workspace
