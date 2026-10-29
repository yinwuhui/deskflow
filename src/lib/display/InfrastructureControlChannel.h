// SPDX-License-Identifier: MIT
#pragma once
#include "CoreInputChannel.h"
#include "control/InfrastructureControl.h"
#include <atomic>
#include <map>
#include <memory>
namespace deskflow::display {
// Product adapter: only core-observed certificate identities introduce a binding.
// Schema admission is not PlanCommit, user authorization or permission to start.
class InfrastructureControlChannel final : public QObject
{
  Q_OBJECT
public:
  // Immutable, non-serializable observation of a current authenticated binding.
  // Only this adapter can mint it; disconnect/replacement/destruction revokes
  // every copy, including copies held outside the QObject owner thread.
  class Lease
  {
  public:
    bool active() const
    {
      return m_live && m_live->load(std::memory_order_acquire);
    }
    const QString &localIdentity() const
    {
      return m_local;
    }
    const QString &peerIdentity() const
    {
      return m_peer;
    }
    const QString &connection() const
    {
      return m_connection;
    }
    const QString &binding() const
    {
      return m_binding;
    }

  private:
    friend class InfrastructureControlChannel;
    Lease(QString local, QString peer, QString connection, QString binding, std::shared_ptr<std::atomic<bool>> live)
        : m_local(std::move(local)),
          m_peer(std::move(peer)),
          m_connection(std::move(connection)),
          m_binding(std::move(binding)),
          m_live(std::move(live))
    {
    }
    QString m_local, m_peer, m_connection, m_binding;
    std::shared_ptr<std::atomic<bool>> m_live;
  };
  ~InfrastructureControlChannel() override;
  std::optional<Lease> lease(const QString &connection) const;
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
    std::shared_ptr<std::atomic<bool>> live = std::make_shared<std::atomic<bool>>(true);
  };
  QString m_local;
  CoreInputChannel *m_core;
  std::map<QString, Entry> m_entries;
};
} // namespace deskflow::display
