// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "PeerUsageModel.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QPointer>
#include <QRegularExpression>
#include <QUuid>
#ifdef DESKFLOW_DISPLAY
#include "display/PeerUsageService.h"
#endif

PeerUsageModel::PeerUsageModel(QObject *parent) : QObject(parent)
{
}
PeerUsageModel *PeerUsageModel::applicationModel()
{
  static QPointer<PeerUsageModel> model;
  if (!model) {
    model = new PeerUsageModel(QCoreApplication::instance());
#ifdef DESKFLOW_DISPLAY
    auto service = deskflow::display::PeerUsageService::applicationService();
    connect(model, &PeerUsageModel::commandRequested, service, &deskflow::display::PeerUsageService::execute);
    connect(service, &deskflow::display::PeerUsageService::snapshotChanged, model, &PeerUsageModel::receiveSnapshot);
    connect(service, &deskflow::display::PeerUsageService::commandFinished, model, &PeerUsageModel::receiveReply);
#endif
  }
  return model;
}
void PeerUsageModel::setConnectedDevices(const QSet<QString> &devices)
{
  if (devices == m_connected)
    return;
  for (const auto &device : m_connected - devices) {
    // A reconnect needs a new authenticated binding, even at the same endpoint.
    m_bindings.remove(device);
    m_pending.remove(device);
    m_errors.remove(device);
  }
  m_connected = devices;
  Q_EMIT changed();
}
bool PeerUsageModel::connected(const QString &device) const
{
  return m_connected.contains(device);
}
void PeerUsageModel::bindAuthenticatedDevice(const QString &device, const QString &peerId)
{
  static const QRegularExpression identity("^[0-9a-f]{64}$");
  if (!connected(device) || !identity.match(peerId).hasMatch())
    return;
  if (m_bindings.value(device) == peerId)
    return;
  m_pending.remove(device);
  m_snapshots.remove(peerId);
  m_bindings.insert(device, peerId);
  m_errors.remove(device);
  send(device, "subscribePeerUsageEvents", {});
}
QJsonObject PeerUsageModel::snapshot(const QString &device) const
{
  return connected(device) ? m_snapshots.value(m_bindings.value(device)) : QJsonObject{};
}
void PeerUsageModel::receiveSnapshot(const QJsonObject &value)
{
  const auto peer = value.value("peerId").toString();
  if (peer.isEmpty())
    return;
  bool valid = false;
  const auto revision = value.value("revision").toString().toULongLong(&valid);
  if (!valid)
    return;
  const auto old = m_snapshots.value(peer);
  if (!old.isEmpty() && revision <= old.value("revision").toString().toULongLong())
    return;
  m_snapshots.insert(peer, value);
  Q_EMIT changed();
}
void PeerUsageModel::receiveReply(const QJsonObject &reply)
{
  const auto id = reply.value("id").toString();
  const auto device = m_pending.key(id);
  if (device.isEmpty())
    return; // Late response from a disconnected/changed peer.
  m_pending.remove(device);
  if (!reply.value("accepted").toBool())
    m_errors.insert(device, reasonText(reply.value("reasonCode").toString()));
  receiveSnapshot(reply.value("snapshot").toObject());
  Q_EMIT changed();
}
bool PeerUsageModel::displayLayoutBlocked() const
{
  for (const auto &device : m_connected) {
    const auto s = snapshot(device);
    if (s.value("effectiveMode") == "extended" || s.value("targetMode") == "extended" ||
        s.value("usageState") == "CleanupRequired")
      return true;
  }
  return false;
}
bool PeerUsageModel::pending(const QString &device) const
{
  return m_pending.contains(device);
}
QString PeerUsageModel::error(const QString &device) const
{
  return m_errors.value(device);
}
void PeerUsageModel::send(const QString &device, const QString &operation, QJsonObject args)
{
  if (!connected(device) || m_bindings.value(device).isEmpty() || pending(device))
    return;
  const auto id = QUuid::createUuid().toString(QUuid::Id128);
  args.insert("peerId", m_bindings.value(device));
  m_pending.insert(device, id);
  m_errors.remove(device);
  Q_EMIT changed();
  Q_EMIT commandRequested(QJsonObject{{"id", id}, {"operation", operation}, {"args", args}});
}
void PeerUsageModel::choose(const QString &device, const QString &mode)
{
  if (mode != "input" && mode != "extended")
    return;
  const auto s = snapshot(device);
  const auto actions = s.value("availableActions").toArray();
  if (s.isEmpty() || (!actions.contains("switch") && !actions.contains("start")))
    return;
  const auto effective = s.value("effectiveMode").toString();
  if (mode == effective)
    return;
  const auto caps = s.value("inputCapabilities").toObject();
  if (caps.value(mode == "extended" ? "extended" : "input") != "supported")
    return;
  const bool start = effective == "none";
  send(
      device, start ? "startPeerUsage" : "switchPeerUsageMode",
      {{start ? "mode" : "targetMode", mode}, {"operationId", QUuid::createUuid().toString(QUuid::Id128)}}
  );
}
void PeerUsageModel::cancel(const QString &device)
{
  const auto s = snapshot(device);
  if (!s.value("availableActions").toArray().contains("cancel"))
    return;
  send(
      device, "cancelPeerUsageOperation",
      {{"operationId", s.value("operationId")}, {"operationGeneration", s.value("operationGeneration")}}
  );
}
void PeerUsageModel::stop(const QString &device)
{
  if (!snapshot(device).value("availableActions").toArray().contains("stop"))
    return;
  send(device, "stopPeerUsage", {{"reason", "UserStopped"}});
}
QString PeerUsageModel::reasonText(const QString &reason)
{
  if (reason == "PeerRejected")
    return tr("The other computer declined the request.");
  if (reason == "PermissionDenied" || reason == "InputPermissionDenied")
    return tr("System permission is required.");
  if (reason == "PeerBusy")
    return tr("Another display session is in use.");
  if (reason == "NetworkUnavailable")
    return tr("A network connection is required for extended display.");
  if (reason == "CleanupFailed")
    return tr("Resources need cleanup before switching again.");
  if (reason == "CapabilityCheckRequired" || reason == "InputCapabilityIncomplete")
    return tr("Device capabilities have not been confirmed.");
  if (reason == "Cancelled")
    return tr("Switch cancelled.");
  if (reason == "RequestExpired")
    return tr("The request timed out.");
  if (reason == "UnsupportedPlatform" || reason == "UnsupportedPeerVersion")
    return tr("This device does not support this mode.");
  return tr("Could not switch mode. Check the connection and try again.");
}
