// SPDX-License-Identifier: MIT
#include "AutoTransport.h"
#include <QBluetoothSocket>

// Separate service UUID: an older single-channel peer must never interpret the new protocol.
static const QBluetoothUuid DualService(QStringLiteral("28d88531-5adc-4852-bf1c-2bc9107c315e"));

AutoTransport::AutoTransport(const Options &options, QObject *parent)
    : QObject(parent),
      m_options(options),
      m_session(options.server, this)
{
  connect(&m_session, &FailoverSession::transportState, this, &AutoTransport::stateChanged);
  for (int i = 0; i < 2; ++i) {
    m_retry[i].setSingleShot(true);
    connect(&m_retry[i], &QTimer::timeout, this, [this, i] { dial(i); });
  }
  m_listenerRetry.setSingleShot(true);
  m_listenerRetry.setInterval(2000);
  connect(&m_listenerRetry, &QTimer::timeout, this, &AutoTransport::listenBluetooth);
}

void AutoTransport::start()
{
  if (m_started || m_stopped)
    return;
  if (!m_options.networkPort || m_options.peerSha256.size() != 32 || m_options.bluetoothPeer.isNull() ||
      !TlsChannel::validIdentity(m_options.pem) || (!m_options.server && m_options.host.isEmpty())) {
    Q_EMIT fatalError("Invalid automatic transport configuration");
    return;
  }
  m_started = true;
  if (m_options.server) {
    if (!m_networkServer.listen(m_options.listenAddress, m_options.networkPort)) {
      Q_EMIT fatalError(m_networkServer.errorString());
      return;
    }
    if (!m_options.corePort) {
      QTcpServer reservation;
      if (!reservation.listen(QHostAddress::LocalHost, 0)) {
        Q_EMIT fatalError(reservation.errorString());
        return;
      }
      m_options.corePort = reservation.serverPort();
      // Existing core owns its listener. Release immediately before the core starts binding.
      reservation.close();
    }
    m_session.setCorePort(m_options.corePort);
    connect(&m_networkServer, &QTcpServer::newConnection, this, [this] {
      while (m_networkServer.hasPendingConnections())
        adopt(FailoverSession::Network, m_networkServer.nextPendingConnection());
    });
    Q_EMIT started(m_options.corePort);
  } else {
    if (!m_localServer.listen(QHostAddress::LocalHost, 0)) {
      Q_EMIT fatalError(m_localServer.errorString());
      return;
    }
    connect(&m_localServer, &QTcpServer::newConnection, this, [this] {
      while (m_localServer.hasPendingConnections())
        m_session.acceptLocal(m_localServer.nextPendingConnection());
    });
    dial(FailoverSession::Network);
    Q_EMIT started(m_localServer.serverPort());
  }
}

void AutoTransport::enableBluetooth()
{
  if (m_stopped || m_bluetoothEnabled)
    return;
  m_bluetoothEnabled = true;
  m_adapter = new QBluetoothLocalDevice(this);
  connect(m_adapter, &QBluetoothLocalDevice::hostModeStateChanged, this, [this](auto mode) {
    if (m_stopped)
      return;
    if (mode == QBluetoothLocalDevice::HostPoweredOff) {
      if (m_links[1])
        m_links[1]->stop("Bluetooth powered off");
      if (m_pending[1])
        abortWire(m_pending[1]);
    }
    if (m_options.server)
      m_listenerRetry.start();
    else
      schedule(1);
  });
  if (m_options.server)
    listenBluetooth();
  else
    dial(1);
}

void AutoTransport::listenBluetooth()
{
  if (m_stopped || !m_bluetoothEnabled)
    return;
  m_service.unregisterService();
  if (m_bluetoothServer) {
    m_bluetoothServer->disconnect(this);
    m_bluetoothServer->close();
    m_bluetoothServer->deleteLater();
  }
  m_bluetoothServer = new QBluetoothServer(QBluetoothServiceInfo::RfcommProtocol, this);
#ifndef Q_OS_MACOS
  m_bluetoothServer->setSecurityFlags(QBluetooth::Security::Encryption | QBluetooth::Security::Authentication);
#endif
  connect(m_bluetoothServer, &QBluetoothServer::errorOccurred, this, [this] {
    if (!m_stopped)
      m_listenerRetry.start();
  });
  connect(m_bluetoothServer, &QBluetoothServer::newConnection, this, [this] {
    while (m_bluetoothServer->hasPendingConnections()) {
      auto *wire = m_bluetoothServer->nextPendingConnection();
      if (wire->peerAddress() != m_options.bluetoothPeer) {
        wire->abort();
        wire->deleteLater();
      } else {
        adopt(1, wire);
      }
    }
  });
  m_service = m_bluetoothServer->listen(DualService, "Deskflow automatic transport v1");
  if (!m_service.isRegistered())
    m_listenerRetry.start();
}

void AutoTransport::schedule(int lane)
{
  if (m_stopped || m_options.server || (lane == 1 && !m_bluetoothEnabled) || m_retry[lane].isActive())
    return;
  m_retry[lane].start(m_backoff[lane]);
  m_backoff[lane] = qMin(8000, m_backoff[lane] * 2);
}

void AutoTransport::dial(int lane)
{
  if (m_stopped || m_options.server || m_links[lane] || m_pending[lane])
    return;
  QIODevice *wire = lane == 0
                        ? static_cast<QIODevice *>(new QTcpSocket(this))
                        : static_cast<QIODevice *>(new QBluetoothSocket(QBluetoothServiceInfo::RfcommProtocol, this));
  m_pending[lane] = wire;
  auto *deadline = new QTimer(wire);
  deadline->setSingleShot(true);
  auto failed = [this, lane, wire] {
    if (m_pending[lane] != wire)
      return;
    m_pending[lane] = nullptr;
    wire->disconnect(this);
    abortWire(wire);
    wire->deleteLater();
    schedule(lane);
  };
  auto connected = [this, lane, wire, deadline] {
    if (m_pending[lane] != wire || m_stopped)
      return;
    deadline->stop();
    wire->disconnect(this);
    m_pending[lane] = nullptr;
    adopt(lane, wire);
  };
  connect(deadline, &QTimer::timeout, this, failed);
  deadline->start(8000);
  if (auto *tcp = qobject_cast<QTcpSocket *>(wire)) {
    connect(tcp, &QTcpSocket::connected, this, connected);
    connect(tcp, &QTcpSocket::errorOccurred, this, failed);
    tcp->connectToHost(m_options.host, m_options.networkPort);
  } else {
    auto *bt = static_cast<QBluetoothSocket *>(wire);
    connect(bt, &QBluetoothSocket::connected, this, connected);
    connect(bt, &QBluetoothSocket::errorOccurred, this, failed);
    bt->connectToService(m_options.bluetoothPeer, DualService);
  }
}

void AutoTransport::adopt(int lane, QIODevice *wire)
{
  if (m_stopped || m_links[lane]) {
    abortWire(wire);
    wire->deleteLater();
    return;
  }
  auto *tls = new TlsChannel(wire, m_options.server, m_options.pem, m_options.peerSha256, this);
  auto *link = new FailoverLink(tls, m_options.server, m_session.instanceId(), this);
  m_links[lane] = link;
  connect(link, &FailoverLink::established, this, [this, lane] { m_backoff[lane] = 1000; });
  connect(link, &FailoverLink::disconnected, this, [this, lane, link](const QString &reason) {
    if (m_links[lane] != link)
      return;
    m_links[lane] = nullptr;
    Q_EMIT warning(QStringLiteral("%1 channel unavailable: %2").arg(lane == 0 ? "Network" : "Bluetooth", reason));
    schedule(lane);
  });
  m_session.attach(FailoverSession::Lane(lane), link);
}

void AutoTransport::abortWire(QIODevice *wire)
{
  if (auto *tcp = qobject_cast<QTcpSocket *>(wire))
    tcp->abort();
  else if (auto *bt = qobject_cast<QBluetoothSocket *>(wire))
    bt->abort();
  else
    wire->close();
}

void AutoTransport::stop()
{
  if (m_stopped)
    return;
  m_stopped = true;
  m_listenerRetry.stop();
  for (auto &timer : m_retry)
    timer.stop();
  for (auto &wire : m_pending) {
    if (wire) {
      wire->disconnect(this);
      abortWire(wire);
      wire->deleteLater();
      wire = nullptr;
    }
  }
  m_session.stop();
  m_networkServer.close();
  m_localServer.close();
  m_service.unregisterService();
  if (m_bluetoothServer) {
    m_bluetoothServer->disconnect(this);
    m_bluetoothServer->close();
  }
}
