// SPDX-License-Identifier: MIT
#pragma once
#include "RfcommSession.h"
#include <QBluetoothLocalDevice>
#include <QBluetoothServer>
#include <QBluetoothServiceInfo>
#include <QBluetoothSocket>
#include <QDebug>
#include <QPointer>
#include <QTcpServer>
#include <algorithm>
inline const QBluetoothUuid ServiceUuid(QStringLiteral("7e349c68-6726-4b5c-a1d4-6c80f21cd980"));

class RfcommBridge : public QObject
{
  Q_OBJECT
public:
  RfcommBridge(bool server, bool probe, QBluetoothAddress peer, quint16 port, int dropAfter)
      : m_serverMode(server),
        m_probe(probe),
        m_peer(peer),
        m_port(port),
        m_dropAfter(dropAfter)
  {
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, [this] { dial(nullptr); });
    m_listenerRetry.setSingleShot(true);
    m_listenerRetry.setInterval(2000);
    connect(&m_listenerRetry, &QTimer::timeout, this, &RfcommBridge::listen);
  }

  void start()
  {
    if (m_peer.isNull() || (m_serverMode && !m_port) || m_dropAfter < 0 || m_dropAfter > 86400) {
      Q_EMIT fatalError("Invalid Bluetooth bridge configuration");
      return;
    }
    if (m_adapter)
      return;
    m_stopping = false;
    m_adapter = new QBluetoothLocalDevice(this);
    qInfo().noquote() << "adapter=" << m_adapter->address().toString() << "name=" << m_adapter->name();
    connect(m_adapter, &QBluetoothLocalDevice::hostModeStateChanged, this, [this](auto mode) {
      if (m_stopping)
        return;
      if (mode == QBluetoothLocalDevice::HostPoweredOff) {
        if (m_session)
          m_session->stop("adapter_powered_off");
        if (m_pendingSocket && m_dialing)
          m_pendingSocket->abort();
      }
      if (m_serverMode)
        m_listenerRetry.start();
    });
    if (m_serverMode) {
      listen();
    } else if (m_probe) {
      dial(nullptr);
    } else {
      m_tcpServer.disconnect(this);
      connect(&m_tcpServer, &QTcpServer::newConnection, this, [this] {
        while (m_tcpServer.hasPendingConnections()) {
          auto *local = m_tcpServer.nextPendingConnection();
          if (m_session || m_dialing) {
            local->abort();
            local->deleteLater();
          } else {
            dial(local);
          }
        }
      });
      if (!m_tcpServer.listen(QHostAddress::LocalHost, m_port)) {
        qCritical() << m_tcpServer.errorString();
        Q_EMIT fatalError(m_tcpServer.errorString());
        return;
      }
      m_port = m_tcpServer.serverPort();
      qInfo() << "local_listener=127.0.0.1 port=" << m_port << "waiting_for_deskflow_client";
      Q_EMIT started(m_port);
    }
  }

  quint16 localPort() const
  {
    return m_port;
  }
  void stop()
  {
    m_stopping = true;
    m_retry.stop();
    m_listenerRetry.stop();
    if (m_pendingSocket && m_dialing)
      m_pendingSocket->abort();
    if (m_session)
      m_session->stop("backend_stopped");
    m_tcpServer.close();
    if (m_adapter) {
      m_adapter->disconnect(this);
      m_adapter->deleteLater();
      m_adapter = nullptr;
    }
    m_service.unregisterService();
    if (m_server)
      m_server->close();
  }
Q_SIGNALS:
  void started(quint16 localPort);
  void fatalError(const QString &message);
  void sessionReady();
  void sessionClosed(const QString &reason);
  void sample(double milliseconds);

private:
  void listen()
  {
    if (m_stopping)
      return;
    // Re-register after adapter/listener errors; never resume a stale data session.
    m_service.unregisterService();
    if (m_server) {
      m_server->disconnect(this);
      m_server->close();
      m_server->deleteLater();
    }
    m_server = new QBluetoothServer(QBluetoothServiceInfo::RfcommProtocol, this);
#ifndef Q_OS_MACOS
    m_server->setSecurityFlags(QBluetooth::Security::Authentication | QBluetooth::Security::Encryption);
#endif
    connect(m_server, &QBluetoothServer::errorOccurred, this, [this] {
      if (m_stopping)
        return;
      qWarning() << "RFCOMM listener error:" << m_server->error() << "retrying_in_ms=2000";
      if (m_session)
        m_session->stop("listener_error");
      m_listenerRetry.start();
    });
    connect(m_server, &QBluetoothServer::newConnection, this, [this] {
      while (m_server->hasPendingConnections()) {
        auto *socket = m_server->nextPendingConnection();
        if (m_listenerRetry.isActive() || m_session || socket->peerAddress() != m_peer) {
          qWarning() << "reject_peer=" << socket->peerAddress().toString();
          socket->close();
          socket->deleteLater();
          continue;
        }
        qInfo() << "accepted_peer=" << socket->peerAddress().toString();
        launch(socket, nullptr);
      }
    });
    m_service = m_server->listen(ServiceUuid, "Deskflow RFCOMM prototype v1");
    if (!m_service.isRegistered()) {
      qWarning() << "Cannot register RFCOMM service; retrying in 2s. Check Bluetooth power and permission.";
      m_listenerRetry.start();
      return;
    }
    qInfo() << "listening_rfcomm channel=" << m_server->serverPort() << "uuid=" << ServiceUuid.toString();
    Q_EMIT started(m_port);
  }

  void dial(QTcpSocket *local)
  {
    if (m_stopping)
      return;
    m_dialing = true;
    auto *socket = new QBluetoothSocket(QBluetoothServiceInfo::RfcommProtocol, this);
    m_pendingSocket = socket;
#ifndef Q_OS_MACOS
    socket->setPreferredSecurityFlags(QBluetooth::Security::Authentication | QBluetooth::Security::Encryption);
#endif
    auto *deadline = new QTimer(socket);
    deadline->setSingleShot(true);
    if (local) {
      local->setParent(socket);
      local->setReadBufferSize(RfcommSession::MaxQueue);
    }
    // A successful dial transfers ownership to RfcommSession and removes the dial-only callbacks.
    auto failed = [this, socket, local](const QString &reason) {
      if (!m_dialing)
        return;
      m_dialing = false;
      socket->disconnect(this);
      if (local) {
        local->abort();
      }
      socket->abort();
      socket->deleteLater();
      qWarning().noquote() << "dial_failed=" << reason;
      retryProbe();
    };
    connect(deadline, &QTimer::timeout, this, [failed] { failed("10s connect deadline"); });
    connect(socket, &QBluetoothSocket::errorOccurred, this, [failed, socket] { failed(socket->errorString()); });
    if (local)
      connect(local, &QTcpSocket::disconnected, socket, [failed] { failed("local closed during dial"); });
    connect(socket, &QBluetoothSocket::connected, this, [this, socket, local, deadline] {
      deadline->stop();
      socket->disconnect(this);
      if (local)
        local->disconnect(socket);
      m_dialing = false;
      launch(socket, local);
    });
    deadline->start(10000);
    qInfo() << "dial_peer=" << m_peer.toString();
    socket->connectToService(m_peer, ServiceUuid);
  }

  void launch(QBluetoothSocket *wire, QTcpSocket *local)
  {
    auto *session = new RfcommSession(wire, !m_serverMode, m_probe, m_port, local);
    session->setParent(this);
    m_session = session;
    connect(session, &RfcommSession::ready, this, [this, session] {
      m_retryMs = 1000;
      if (m_dropAfter && !m_dropped) {
        m_dropped = true;
        QTimer::singleShot(m_dropAfter * 1000, session, [session] { session->stop("simulated_drop"); });
      }
    });
    connect(session, &RfcommSession::finished, this, [this, session] {
      m_session.clear();
      session->deleteLater();
      retryProbe();
    });
    connect(session, &RfcommSession::ready, this, &RfcommBridge::sessionReady);
    connect(session, &RfcommSession::sample, this, &RfcommBridge::sample);
    connect(session, &RfcommSession::closed, this, &RfcommBridge::sessionClosed);
    session->start();
  }

  void retryProbe()
  {
    if (!m_stopping && !m_serverMode && m_probe) {
      qInfo() << "retry_in_ms=" << m_retryMs;
      m_retry.start(m_retryMs);
      m_retryMs = std::min(8000, m_retryMs * 2);
    }
    // In bridge mode the Deskflow client's next TCP connection starts a fresh RFCOMM session.
  }

  bool m_serverMode, m_probe, m_dialing = false, m_dropped = false, m_stopping = false;
  QBluetoothAddress m_peer;
  quint16 m_port;
  int m_dropAfter, m_retryMs = 1000;
  QBluetoothServer *m_server = nullptr;
  QBluetoothServiceInfo m_service;
  QTcpServer m_tcpServer;
  QPointer<RfcommSession> m_session;
  QPointer<QBluetoothSocket> m_pendingSocket;
  QTimer m_retry, m_listenerRetry;
  QBluetoothLocalDevice *m_adapter = nullptr;
};
