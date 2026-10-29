// SPDX-License-Identifier: MIT
#pragma once
#include "CoreInputChannel.h"
#include "control/InfrastructureControl.h"
#include <map>
namespace deskflow::display {
// Product adapter: only core-observed certificate identities introduce a binding.
// Schema admission is not PlanCommit, user authorization or permission to start.
class InfrastructureControlChannel final : public QObject
{
  Q_OBJECT
public:
  InfrastructureControlChannel(
      QString localIdentity, CoreInputChannel *, PeerUsageService *, QObject *parent = nullptr
  );
  bool send(const QString &connection, const infra::Envelope &);
Q_SIGNALS:
  void bindingReady(const QString &peer, const QString &connection, const QString &binding);
  void bindingLost(const QString &peer, const QString &connection);
  void envelopeReceived(const QString &peer, const QString &connection, const QJsonObject &envelope);

private:
  void observed(const QJsonObject &);
  void received(const QJsonObject &);
  void closed(const QJsonObject &);
  struct Entry
  {
    QString peer;
    infra::ControlBinding binding;
  };
  QString m_local;
  CoreInputChannel *m_core;
  std::map<QString, Entry> m_entries;
};
} // namespace deskflow::display
