// SPDX-License-Identifier: MIT
#pragma once
#include "FailoverSession.h"
#include <QBluetoothAddress>
#include <QBluetoothLocalDevice>
#include <QBluetoothServer>
#include <QBluetoothServiceInfo>
#include <QTcpServer>
#include <array>

class AutoTransport : public QObject
{
  Q_OBJECT
public:
  struct Options
  {
    bool server = false;
    QString host, pem;
    QHostAddress listenAddress = QHostAddress::Any;
    QBluetoothAddress bluetoothPeer;
    QByteArray peerSha256;
    quint16 networkPort = 24800;
    quint16 corePort = 0; // Tests may supply an already-listening core. Production allocates an internal port.
  };
  explicit AutoTransport(const Options &options, QObject *parent = nullptr);
  void start();
  void enableBluetooth();
  void stop();
  void coreConnected(bool connected)
  {
    m_session.coreConnected(connected);
  }
Q_SIGNALS:
  void started(quint16 corePort);
  void stateChanged(const QString &state);
  void fatalError(const QString &error);
  void warning(const QString &error);

private:
  void listenBluetooth();
  void dial(int lane);
  void adopt(int lane, QIODevice *wire);
  void schedule(int lane);
  void abortWire(QIODevice *wire);
  Options m_options;
  FailoverSession m_session;
  QTcpServer m_networkServer, m_localServer;
  QBluetoothServer *m_bluetoothServer = nullptr;
  QBluetoothLocalDevice *m_adapter = nullptr;
  QBluetoothServiceInfo m_service;
  std::array<QPointer<FailoverLink>, 2> m_links;
  std::array<QPointer<QIODevice>, 2> m_pending;
  std::array<QTimer, 2> m_retry;
  std::array<int, 2> m_backoff{1000, 1000};
  QTimer m_listenerRetry;
  bool m_started = false, m_stopped = false, m_bluetoothEnabled = false;
};
