// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "display/core/InfrastructureTypes.h"
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <optional>

namespace deskflow::display::infra {
struct ControlError { QString code; QString detail; };
std::optional<QJsonObject> parseControlObject(const QByteArray&, ControlError&);
std::optional<QJsonObject> parseBoundedObject(const QByteArray&, qsizetype localByteBudget, ControlError&);
QByteArray canonicalPlan(const SessionPlan&);
QString planHash(const SessionPlan&);
std::optional<SessionPlan> decodePlan(const QByteArray&, const QString& expectedHash,
  const CapabilitySnapshot& sender, const CapabilitySnapshot& receiver,
  const Requirements&, const SelectionPolicy&, ControlError&);

struct Envelope {
  QString type, messageId, sessionId, planHash;
  uint64_t attempt = 0;
  uint64_t sequence = 0;
  QJsonObject body;
};
// Local connection reference is supplied by the authenticated transport, never
// read from peer JSON. Nonces bind this logical control channel to that instance.
class ControlBinding {
public:
  ControlBinding(QString localIdentity, QString peerIdentity, QString connectionReference);
  QByteArray hello() const;
  bool acceptHello(const QByteArray&, const QString& authenticatedPeer,
                   const QString& connectionReference, ControlError&);
  bool ready() const { return !m_binding.isEmpty(); }
  QString id() const { return m_binding; }
  QByteArray encode(const Envelope&, ControlError&);
  std::optional<Envelope> receive(const QByteArray&, const QString& authenticatedPeer,
                                  const QString& connectionReference, ControlError&);
private:
  QString m_local, m_peer, m_connection, m_nonce, m_remoteNonce, m_binding;
  uint64_t m_tx = 0, m_rx = 0;
};
} // namespace deskflow::display::infra
