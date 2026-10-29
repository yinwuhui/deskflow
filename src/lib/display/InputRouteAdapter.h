// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "DisplayService.h"
#include "PeerUsageService.h"
#include <QElapsedTimer>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <map>
namespace deskflow::display {
// Opaque native-core isolation acknowledgement. It cannot be reconstructed from
// a peer JSON assertion. Copies expire and revoke together, never reactivating.
class InputIsolationProof
{
public:
  bool active() const;
  const QString &peer() const
  {
    return m_state->peer;
  }
  const QString &connection() const
  {
    return m_state->connection;
  }
  const QString &connectionGeneration() const
  {
    return m_state->connectionGeneration;
  }
  const QString &reference() const
  {
    return m_state->reference;
  }
  const UsageKey &operation() const
  {
    return m_state->key;
  }

private:
  friend class InputRouteAdapter;
  struct State
  {
    QString peer, connection, connectionGeneration, reference;
    UsageKey key;
    uint32_t sequence = 0;
    std::atomic<bool> valid{true};
    std::atomic<int64_t> expires{0};
  };
  explicit InputIsolationProof(std::shared_ptr<State> state) : m_state(std::move(state))
  {
  }
  std::shared_ptr<State> m_state;
};
// Converts transaction effects into generation-tagged core IPC operations.
// Completion requires an exact peer/operation/sequence response from the core.
class InputRouteAdapter : public QObject
{
  Q_OBJECT
public:
  InputRouteAdapter(PeerUsageService *usage, DisplayService *display, QObject *parent = nullptr);
  ~InputRouteAdapter() override;
  std::optional<InputIsolationProof> isolationProof(const QString &peer, const UsageKey &) const;
  bool verifyIsolation(const InputIsolationProof &);
  void coreResult(const QJsonObject &result);
  void coreUnavailable();
Q_SIGNALS:
  void isolationRevoked(const QString &peer, const QString &reference);
  void coreCommand(const QJsonObject &command);

private:
  struct Pending
  {
    UsageKey key;
    QString effect;
    QString connectionId, connectionGeneration;
    QString requestId;
    QString kind;
    uint32_t sequence = 0;
    bool enabled = false;
    bool committing = false;
    QElapsedTimer elapsed;
  };
  void effect(const QJsonObject &command);
  void send(const QString &peer, Pending &pending, const QString &kind);
  void fail(const QString &peer, Reason reason);
  void tick();
  void revokeIsolation(const QString &peer);
  void revokeAllIsolation();
  PeerUsageService *m_usage;
  DisplayService *m_display;
  std::map<QString, Pending> m_pending;
  std::map<QString, uint32_t> m_sequence;
  std::map<QString, InputIsolationProof> m_isolation;
  std::map<QString, std::pair<UsageKey, uint32_t>> m_released;
  QTimer m_timer;
};
} // namespace deskflow::display
