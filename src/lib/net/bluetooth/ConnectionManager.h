// SPDX-License-Identifier: MIT
#pragma once
#include "BluetoothDiscovery.h"
#include "BluetoothPairing.h"
#include "FailoverSession.h"
#include <QBluetoothServer>
#include <QBluetoothServiceInfo>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QTcpServer>
#include <functional>

// One process owns the input core; each authenticated peer owns an independent logical session.
class ConnectionManager : public QObject
{
  Q_OBJECT
public:
  struct Options
  {
    bool server = false, automatic = true;
    int initialLane = 0;
    QString host, pem, name, bluetoothAddress, initialPeer;
    quint16 port = 24800, corePort = 0;
    QHostAddress bind = QHostAddress::Any;
    QString backupPeer;
    std::function<void(QObject *, bool, std::function<void(bool)>)> bluetoothPermission;
    std::function<bool(const QByteArray &)> trusted;
    std::function<QJsonObject(const QString &)> loadPeer;
    std::function<void(const QString &, const QJsonObject &)> savePeer;
  };
  explicit ConnectionManager(Options options, QObject *parent = nullptr);
  ~ConnectionManager() override;
  void start();
  void stop();
  QJsonObject snapshot() const;
  void command(const QJsonObject &request);
  // Connectors may supply an already-connected reliable byte stream; authentication remains mandatory.
  void acceptTransport(QIODevice *wire, int lane, const QString &expected = {})
  {
    if (lane >= 0 && lane < 2)
      adopt(wire, lane, expected);
  }
  void suspend(bool sleeping);
  void trustChanged()
  {
    validateTrust();
    publish();
  }
  void coreConnected(const QString &peer, bool connected);
Q_SIGNALS:
  void started(quint16 port);
  void changed(const QJsonObject &snapshot);
  void operationFinished(const QJsonObject &result);
  void stateChanged(const QString &state);
  void trustRequired(const QByteArray &fingerprint);
  void fatalError(const QString &error);
  void clipboardLimit(const QString &peer, int kib);

private:
  struct Peer
  {
    QString id, name, state = "connecting", reason, action, actionId, bt;
    FailoverSession *session = nullptr;
    QPointer<FailoverLink> links[2];
    QList<QPair<QString, quint16>> endpoints;
    bool suppressed = false, explicitPolicy = false;
    bool requested = true, remoteAllows = false, offered = false, stopped = false;
    int initial = 0, failures[2] = {0, 0};
    quint64 endpointRevision = 0;
    qint64 endpointsExpireAt = 0;
    QTimer *retry[2] = {nullptr, nullptr};
    QPointer<QIODevice> pending[2];
    QString remoteSession;
    bool connected = false;
  };
  Peer *peer(const QString &id);
  bool effective(const Peer *p) const;
  void publish();
  void offer(Peer *p);
  void control(Peer *p, int lane, const QByteArray &payload);
  void prepare(Peer *p);
  void requestBluetooth(Peer *p);
  void startBluetooth();
  void listenBluetooth();
  void findBluetooth(Peer *p);
  void pairBluetooth(Peer *p);
  void dial(Peer *p, int lane);
  void retry(Peer *p, int lane);
  void adopt(QIODevice *wire, int lane, const QString &expected = {});
  void interruptWire(QIODevice *wire);
  void closePeer(Peer *p);
  void setupSession(Peer *p);
  void validateTrust();
  QJsonObject localOffer(Peer *p) const;
  Options m_options;
  QTcpServer m_server, m_local;
  QBluetoothServer *m_btServer = nullptr;
  QBluetoothServiceInfo m_service;
  QBluetoothLocalDevice *m_adapter = nullptr;
  QMap<QString, Peer *> m_peers;
  QSet<QIODevice *> m_wires;
  QSet<QString> m_trustReported;
  QByteArray m_instance = QUuid::createUuid().toRfc4122();
  QString m_clientPeer, m_backupPeer, m_btReason;
  QTimer m_networkTimer, m_bootstrapRetry;
  QBluetoothServiceDiscoveryAgent *m_serviceDiscovery = nullptr;
  QTimer m_scanDeadline;
  BluetoothDiscovery *m_discovery = nullptr;
  BluetoothPairing *m_pairing = nullptr;
  QString m_pairPeer, m_pairAction;
  quint64 m_revision = 0;
  bool m_stopped = false, m_started = false, m_btEnabled = false, m_btRequesting = false;
  bool m_explicitTarget = false, m_suspended = false;
  int m_bootstrapBackoff = 1000;
  qint64 m_lastScan = 0;
};
