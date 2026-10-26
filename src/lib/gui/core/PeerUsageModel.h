// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QHash>
#include <QJsonObject>
#include <QJsonArray>
#include <QObject>
#include <QSet>

// Shared presentation state for devices and arrangement. UI names/IPs are never
// used as authenticated identities. The transport adapter must supply bindings.
class PeerUsageModel : public QObject
{
  Q_OBJECT
public:
  explicit PeerUsageModel(QObject *parent = nullptr);
  static PeerUsageModel *applicationModel();
  void setConnectedDevices(const QSet<QString> &devices);
  bool connected(const QString &device) const;
  void bindAuthenticatedDevice(const QString &device, const QString &peerId);
  QJsonObject snapshot(const QString &device) const;
  void receiveSnapshot(const QJsonObject &snapshot);
  void receiveReply(const QJsonObject &reply);
  void choose(const QString &device, const QString &mode);
  void cancel(const QString &device);
  void stop(const QString &device);
  void retryCleanup(const QString &device);
  void receiveConnection(const QJsonObject& connection);
  QJsonArray authenticatedConnections() const;
  bool pending(const QString &device) const;
  bool displayLayoutBlocked() const;
  QString error(const QString &device) const;
  static QString reasonText(const QString &reason);
Q_SIGNALS:
  void changed();
  void connectionsChanged();
  void commandRequested(const QJsonObject &command);

private:
  void send(const QString &device, const QString &operation, QJsonObject args);
  QSet<QString> m_connected;
  QHash<QString, QString> m_bindings, m_pending, m_errors;
  QHash<QString, QJsonObject> m_snapshots;
  QHash<QString,QJsonObject> m_connections;
};
