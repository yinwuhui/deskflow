// SPDX-License-Identifier: MIT
#include "InfrastructureControlChannel.h"
#include <QJsonDocument>
namespace deskflow::display {
using namespace infra;
InfrastructureControlChannel::InfrastructureControlChannel(
    QString local, CoreInputChannel *core, PeerUsageService *usage, QObject *parent
)
    : QObject(parent),
      m_local(std::move(local)),
      m_core(core)
{
  connect(core, &CoreInputChannel::authenticatedConnectionObserved, this, &InfrastructureControlChannel::observed);
  connect(core, &CoreInputChannel::peerControlReceived, this, &InfrastructureControlChannel::received);
  connect(usage, &PeerUsageService::connectionChanged, this, &InfrastructureControlChannel::closed);
}
void InfrastructureControlChannel::observed(const QJsonObject &record)
{
  const auto peer = record.value("peerId").toString(), connection = record.value("baseConnectionId").toString();
  if (!record.value("baseConnected").toBool() || !validId(m_local.toStdString(), 32) ||
      !validId(peer.toStdString(), 32) || peer == m_local || !validId(connection.toStdString(), 16))
    return;
  if (m_entries.contains(connection))
    return;
  if (m_entries.size() >= 256)
    return;
  // A replacement connection invalidates the old nonce transcript immediately.
  for (auto it = m_entries.begin(); it != m_entries.end();) {
    if (it->second.peer == peer) {
      const auto old = it->first;
      it = m_entries.erase(it);
      Q_EMIT bindingLost(peer, old);
    } else
      ++it;
  }
  auto [it, inserted] = m_entries.emplace(connection, Entry{peer, ControlBinding(m_local, peer, connection)});
  if (inserted)
    m_core->sendPeerControl(peer, connection, QJsonDocument::fromJson(it->second.binding.hello()).object());
}
void InfrastructureControlChannel::closed(const QJsonObject &record)
{
  if (record.value("baseConnected").toBool())
    return;
  const auto connection = record.value("baseConnectionId").toString();
  auto it = m_entries.find(connection);
  if (it == m_entries.end() || it->second.peer != record.value("peerId").toString())
    return;
  const auto peer = it->second.peer;
  m_entries.erase(it);
  Q_EMIT bindingLost(peer, connection);
}
void InfrastructureControlChannel::received(const QJsonObject &event)
{
  const auto peer = event.value("peerId").toString(), connection = event.value("baseConnectionId").toString();
  auto it = m_entries.find(connection);
  if (it == m_entries.end() || it->second.peer != peer || !event.value("body").isObject())
    return;
  auto &binding = it->second.binding;
  const auto body = event.value("body").toObject();
  const auto protocol = body.value("protocol").toString();
  const auto bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
  ControlError error;
  if (protocol == "display-infra-bind/1") {
    const bool ready = binding.ready();
    if (!binding.acceptHello(bytes, peer, connection, error))
      return;
    if (!ready) {
      // Completes startup if the first hello preceded the remote core observation.
      m_core->sendPeerControl(peer, connection, QJsonDocument::fromJson(binding.hello()).object());
      Q_EMIT bindingReady(peer, connection, binding.id());
    }
  } else if (protocol == "display-infra/1") {
    if (binding.receive(bytes, peer, connection, error))
      Q_EMIT envelopeReceived(peer, connection, body);
  }
}
bool InfrastructureControlChannel::send(const QString &connection, const Envelope &envelope)
{
  auto it = m_entries.find(connection);
  if (it == m_entries.end())
    return false;
  // Encode a candidate transcript first. A rejected local write must not consume
  // its sequence. CoreInputChannel enforces the actual wrapped 16 KiB budget.
  auto candidate = it->second.binding;
  ControlError error;
  const auto bytes = candidate.encode(envelope, error);
  if (bytes.isEmpty() || !m_core->sendPeerControl(it->second.peer, connection, QJsonDocument::fromJson(bytes).object()))
    return false;
  it->second.binding = std::move(candidate);
  return true;
}
} // namespace deskflow::display
