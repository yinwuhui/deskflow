// SPDX-License-Identifier: MIT
#include "ConnectionManager.h"
#include "../../common/PeerConnectionRegistry.h"
#include <QBluetoothSocket>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QScopeGuard>
#include <QSysInfo>
#include <QTcpSocket>
#include <QUrl>
#include <cmath>
#include <memory>
#include <utility>

static const QBluetoothUuid ManagedService(QStringLiteral("28d88531-5adc-4852-bf1c-2bc9107c315e"));
static QString operationId()
{
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
ConnectionManager::ConnectionManager(Options options, QObject *parent)
    : QObject(parent),
      m_options(std::move(options)),
      m_backupPeer(m_options.backupPeer)
{
  // Authenticated device links and the local core bridge use direct sockets.
  // Application/system HTTP or SOCKS proxy settings must not redirect them.
  m_server.setProxy(QNetworkProxy::NoProxy);
  m_local.setProxy(QNetworkProxy::NoProxy);
  m_explicitTarget = !m_backupPeer.isEmpty();
  m_bootstrapRetry.setSingleShot(true);
  connect(&m_bootstrapRetry, &QTimer::timeout, this, [this] {
    if (m_stopped || !m_clientPeer.isEmpty() || (m_peers.contains({}) && m_peers.value({})->stopped))
      return;
    // A bootstrap peer is not a trusted identity; it never enters the peer map.
    auto *p = peer(QString());
    dial(p, m_options.initialLane);
  });
  m_networkTimer.setInterval(5000);
  connect(&m_networkTimer, &QTimer::timeout, this, [this] {
    validateTrust();
    for (auto *p : m_peers)
      if (!p->id.isEmpty() && !p->stopped) {
        if (p->connected)
          offer(p);
        prepare(p);
      }
    publish();
  });
}
ConnectionManager::Peer *ConnectionManager::peer(const QString &id)
{
  if (m_peers.contains(id))
    return m_peers[id];
  auto *p = new Peer;
  p->id = id;
  p->initial = m_options.initialLane;
  p->requested = m_options.automatic;
  if (m_options.loadPeer && !id.isEmpty()) {
    const auto saved = m_options.loadPeer(id);
    p->explicitPolicy =
        saved.contains("automaticExplicit") ? saved.value("automaticExplicit").toBool() : saved.contains("automatic");
    if (p->explicitPolicy)
      p->requested = saved.value("automatic").toBool();
    p->bt = saved.value("bluetooth").toString();
  }
  setupSession(p);
  for (int lane = 0; lane < 2; ++lane) {
    p->retry[lane] = new QTimer(this);
    p->retry[lane]->setSingleShot(true);
    connect(p->retry[lane], &QTimer::timeout, this, [this, p, lane] { dial(p, lane); });
  }
  m_peers.insert(id, p);
  return p;
}
void ConnectionManager::setupSession(Peer *p)
{
  p->session = new FailoverSession(m_options.server, this);
  p->session->setCorePort(m_options.corePort);
  QTimer::singleShot(3000, this, [this, p] {
    if (!m_stopped && !p->stopped && !p->offered && !p->id.isEmpty()) {
      p->reason = "capabilitiesTimeout";
      publish();
    }
  });
  p->session->setInitialLane(FailoverSession::Lane(p->initial));
  p->session->setAutomatic(false);
  connect(p->session, &FailoverSession::transportState, this, [this, p](const QString &state) {
    if (m_stopped || p->stopped)
      return;
    p->state = state;
    p->connected = state == "network" || state == "bluetooth";
    if (!m_options.server || p->id == m_backupPeer)
      Q_EMIT stateChanged(state);
    if (p->connected) {
      offer(p);
      prepare(p);
    }
    publish();
  });
}
void ConnectionManager::start()
{
  if (m_started || m_stopped)
    return;
  if (!TlsChannel::validIdentity(m_options.pem) || !m_options.port || !m_options.trusted ||
      (!m_options.server && m_options.initialLane == 0 && m_options.host.isEmpty()) ||
      (!m_options.server && m_options.initialLane == 1 && QBluetoothAddress(m_options.bluetoothAddress).isNull())) {
    Q_EMIT fatalError("Invalid first-channel configuration or TLS identity");
    return;
  }
  m_started = true;
  if (m_options.server) {
    if (m_options.serverTransports == 0) {
      Q_EMIT fatalError("Select at least one sharing transport");
      return;
    }
    if ((m_options.serverTransports & 1) && !m_server.listen(m_options.bind, m_options.port)) {
      Q_EMIT fatalError(m_server.errorString());
      return;
    }
    if (!m_options.corePort) {
      QTcpServer reservation;
      if (!reservation.listen(QHostAddress::LocalHost, 0)) {
        Q_EMIT fatalError(reservation.errorString());
        return;
      }
      m_options.corePort = reservation.serverPort();
    }
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
      while (m_server.hasPendingConnections())
        adopt(m_server.nextPendingConnection(), 0);
    });
    Q_EMIT started(m_options.corePort);
    // Explicit sharing selection prepares the advertised listener only after Start sharing.
    if (m_options.initialLane == 1 || m_options.advertiseBluetooth)
      requestBluetooth(nullptr);
  } else {
    if (!m_local.listen(QHostAddress::LocalHost, 0)) {
      Q_EMIT fatalError(m_local.errorString());
      return;
    }
    connect(&m_local, &QTcpServer::newConnection, this, [this] {
      while (m_local.hasPendingConnections()) {
        auto *socket = m_local.nextPendingConnection();
        if (m_peers.contains(m_clientPeer) && !m_clientPeer.isEmpty() && !m_peers[m_clientPeer]->stopped)
          m_peers[m_clientPeer]->session->acceptLocal(socket);
        else {
          socket->abort();
          socket->deleteLater();
        }
      }
    });
    Q_EMIT started(m_local.serverPort());
    if (m_options.initialLane == 1) {
      auto *initial = peer({});
      initial->bt = m_options.bluetoothAddress;
      requestBluetooth(initial);
    } else
      dial(peer({}), 0);
  }
  m_networkTimer.start();
  publish();
}
bool ConnectionManager::effective(const Peer *p) const
{
  return p->requested && p->remoteAllows && !p->stopped &&
         (!m_options.server || (m_options.serverTransports == 3 && m_backupPeer == p->id));
}
QJsonObject ConnectionManager::localOffer(Peer *p) const
{
  QJsonArray endpoints;
  if (m_options.server && m_server.isListening()) {
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
      if (!(iface.flags() & QNetworkInterface::IsUp) || iface.flags() & QNetworkInterface::IsLoopBack)
        continue;
      for (const auto &entry : iface.addressEntries()) {
        auto addr = entry.ip();
        if (addr.isNull() || addr.isLoopback() || addr.isMulticast())
          continue;
        // Link-local scope identifiers are local to the receiving host. Avoid advertising them in v2.
        if (addr.protocol() == QAbstractSocket::IPv6Protocol && addr.isLinkLocal())
          addr.setScopeId({});
        if (m_options.bind != QHostAddress::Any && m_options.bind != QHostAddress::AnyIPv4 &&
            m_options.bind != QHostAddress::AnyIPv6 && addr != m_options.bind)
          continue;
        endpoints.append(QJsonObject{{"host", addr.toString()}, {"port", int(m_server.serverPort())}});
        if (endpoints.size() == 8)
          break;
      }
      if (endpoints.size() == 8)
        break;
    }
  }
  const bool selected = !m_options.server || m_backupPeer == p->id;
  auto values = endpoints.toVariantList();
  std::stable_sort(values.begin(), values.end(), [](const QVariant &left, const QVariant &right) {
    const QHostAddress a(left.toMap().value("host").toString()), b(right.toMap().value("host").toString());
    const auto rank = [](const QHostAddress &address) {
      return address.protocol() == QAbstractSocket::IPv4Protocol ? 0 : address.isLinkLocal() ? 2 : 1;
    };
    return rank(a) < rank(b);
  });
  endpoints = QJsonArray::fromVariantList(values);
  return {
      {"version", 2},
      {"kind", "offer"},
      {"requestId", operationId()},
      {"instance", QString::fromLatin1(m_instance.toHex())},
      {"name", m_options.name},
      {"server", m_options.server},
      {"automatic", p->requested && selected && (!m_options.server || m_options.serverTransports == 3)},
      {"selected", selected},
      {"network", endpoints},
      {"revision", double(m_revision)},
      {"bluetooth", m_adapter ? m_adapter->address().toString() : QString()},
      {"service", ManagedService.toString()},
      {"clipboardKiB", effective(p) ? 64 : 0}
  };
}
void ConnectionManager::offer(Peer *p)
{
  if (p->stopped)
    return;
  const auto data = QJsonDocument(localOffer(p)).toJson(QJsonDocument::Compact);
  for (auto &link : p->links)
    if (link && link->ready())
      link->send(FailoverLink::Control, 0, data);
}
void ConnectionManager::control(Peer *p, int lane, const QByteArray &payload)
{
  const auto doc = QJsonDocument::fromJson(payload);
  if (!doc.isObject()) {
    p->links[lane]->stop("Invalid management payload");
    return;
  }
  const auto obj = doc.object();
  if (obj.value("version").toInt() != 2 || obj.value("instance").toString() != p->remoteSession) {
    p->links[lane]->stop("Invalid management session");
    return;
  }
  const auto kind = obj.value("kind").toString();
  if (kind == "stop") {
    closePeer(p);
    publish();
    return;
  }
  if (kind != "offer" || obj.value("server").toBool() == m_options.server ||
      obj.value("name").toString().size() > 256 || obj.value("network").toArray().size() > 8 ||
      obj.value("service").toString() != ManagedService.toString()) {
    p->links[lane]->stop("Invalid capabilities");
    return;
  }
  const double revisionValue = obj.value("revision").toDouble(-1);
  if (revisionValue < 0 || revisionValue > 9007199254740991.0 || std::floor(revisionValue) != revisionValue) {
    p->links[lane]->stop("Invalid control revision");
    return;
  }
  if (quint64(revisionValue) < p->endpointRevision)
    return;
  p->name = obj.value("name").toString();
  const bool policyChanged = p->remoteAllows != obj.value("automatic").toBool();
  p->remoteAllows = obj.value("automatic").toBool();
  p->offered = true;
  const auto revision = quint64(qMax(0.0, obj.value("revision").toDouble()));
  if (revision >= p->endpointRevision) {
    p->endpointRevision = revision;
    p->endpointsExpireAt = QDateTime::currentSecsSinceEpoch() + 30;
    p->endpoints.clear();
    for (const auto &candidate : obj.value("network").toArray()) {
      if (p->endpoints.size() >= 8)
        break;
      const auto endpoint = candidate.toObject();
      QHostAddress address(endpoint.value("host").toString());
      const int port = endpoint.value("port").toInt();
      if (address.isNull() || address.isLoopback() || address.isMulticast() || port < 1 || port > 65535)
        continue;
      if (address.protocol() == QAbstractSocket::IPv6Protocol && address.isLinkLocal()) {
        for (const auto &iface : QNetworkInterface::allInterfaces()) {
          if (!(iface.flags() & QNetworkInterface::IsUp) || iface.flags() & QNetworkInterface::IsLoopBack)
            continue;
          bool suitable = false;
          for (const auto &entry : iface.addressEntries())
            if (entry.ip().protocol() == QAbstractSocket::IPv6Protocol && entry.ip().isLinkLocal())
              suitable = true;
          if (!suitable)
            continue;
          address.setScopeId(iface.name());
          p->endpoints.append({address.toString(), quint16(port)});
          if (p->endpoints.size() >= 8)
            break;
        }
      } else if (p->endpoints.size() < 8)
        p->endpoints.append({address.toString(), quint16(port)});
    }
    const auto bt = QBluetoothAddress(obj.value("bluetooth").toString());
    if (!bt.isNull())
      p->bt = bt.toString();
  }
  if (m_options.savePeer)
    m_options.savePeer(
        p->id, {{"automatic", p->requested},
                {"automaticExplicit", p->explicitPolicy},
                {"bluetooth", p->bt},
                {"name", p->name},
                {"lastVerified", double(QDateTime::currentSecsSinceEpoch())},
                {"networkTarget", m_options.server ? QString() : m_options.host},
                {"network", obj.value("network")}}
    );
  p->session->setAutomatic(effective(p));
  Q_EMIT clipboardLimit(p->id, effective(p) ? 64 : 0);
  if (!effective(p)) {
    const int active = p->session->selectedLane();
    // Do not close a candidate initial lane before the first core activation.
    if (active >= 0) {
      const int standby = 1 - active;
      p->retry[standby]->stop();
      if (p->pending[standby])
        interruptWire(p->pending[standby]);
      p->session->removeLane(FailoverSession::Lane(standby));
    }
  } else if (p->connected)
    prepare(p);
  if (policyChanged)
    offer(p);
  publish();
}
void ConnectionManager::adopt(QIODevice *wire, int lane, const QString &expected)
{
  if (m_stopped || m_suspended || m_wires.size() >= 16 ||
      (m_options.server && !(m_options.serverTransports & (1 << lane)))) {
    interruptWire(wire);
    wire->deleteLater();
    return;
  }
  m_wires.insert(wire);
  auto *tls = new TlsChannel(wire, m_options.server, m_options.pem, {}, this);
  tls->setVerifier([this, expected](const QByteArray &pin) {
    if (m_stopped || m_suspended || (!expected.isEmpty() && pin.toHex() != expected.toLatin1()))
      return false;
    if (m_options.trusted(pin))
      return true;
    const auto id = QString::fromLatin1(pin.toHex());
    if (!m_trustReported.contains(id)) {
      m_trustReported.insert(id);
      Q_EMIT trustRequired(pin);
    }
    return false;
  });
  auto *link = new FailoverLink(tls, m_options.server, m_instance, this);
  connect(link, &FailoverLink::established, this, [this, link, lane, expected, wire] {
    if (auto *deadline = wire->findChild<QTimer *>("connectionDeadline", Qt::FindDirectChildrenOnly))
      deadline->stop();
    const auto id = QString::fromLatin1(link->peerFingerprint().toHex());
    if (m_stopped || m_suspended || (!expected.isEmpty() && expected != id) ||
        (!m_options.server && !m_clientPeer.isEmpty() && m_clientPeer != id)) {
      link->stop("Unexpected peer");
      return;
    }
    auto *p = peer(id);
    for (auto *candidate : m_peers)
      if (candidate->pending[lane] == wire)
        candidate->pending[lane] = nullptr;
    if (p->stopped || (p->suppressed && p->session->reconnectLane() >= 0 && lane != p->session->reconnectLane()) ||
        p->links[lane] ||
        (!p->remoteSession.isEmpty() && p->remoteSession != QString::fromLatin1(link->peerInstance().toHex()) &&
         (p->links[0] || p->links[1]))) {
      link->stop("Duplicate or different peer instance");
      return;
    }
    const auto remoteInstance = QString::fromLatin1(link->peerInstance().toHex());
    if (p->remoteSession != remoteInstance) {
      p->endpointRevision = 0;
      p->offered = false;
      p->remoteAllows = false;
    }
    p->remoteSession = remoteInstance;
    if (m_options.server && !m_explicitTarget) {
      int count = 0;
      QString only;
      for (auto *candidate : m_peers)
        if (!candidate->id.isEmpty() && !candidate->stopped) {
          ++count;
          only = candidate->id;
        }
      m_backupPeer = count == 1 ? only : QString();
      for (auto *candidate : m_peers)
        if (candidate != p && !candidate->id.isEmpty()) {
          candidate->session->setAutomatic(effective(candidate));
          if (!effective(candidate) && candidate->session->selectedLane() >= 0)
            candidate->session->removeLane(FailoverSession::Lane(1 - candidate->session->selectedLane()));
          Q_EMIT clipboardLimit(candidate->id, effective(candidate) ? 64 : 0);
          offer(candidate);
        }
    }
    if (!m_options.server)
      m_clientPeer = id;
    p->links[lane] = link;
    p->remoteAddresses[lane].clear();
    if(lane==0) {
      if(auto *socket=qobject_cast<QTcpSocket *>(wire)) {
        auto host=socket->peerAddress().toString();
        if(host.contains(':'))host='['+host+']';
        p->remoteAddresses[lane]=host+':'+QString::number(socket->peerPort());
      }
    } else if(auto *socket=qobject_cast<QBluetoothSocket *>(wire)) {
      p->remoteAddresses[lane]=socket->peerAddress().toString();
    }
    if (lane == 1) {
      if (auto *bluetooth = qobject_cast<QBluetoothSocket *>(wire))
        p->bt = bluetooth->peerAddress().toString();
      else if (p->bt.isEmpty())
        p->bt = m_options.bluetoothAddress;
    }
    if (lane == 1) {
      p->action.clear();
      p->actionId.clear();
      p->reason.clear();
    }
    connect(link, &FailoverLink::healthConfirmed, this, [this] { publish(); });
    connect(link, &FailoverLink::statisticsChanged, this, [this] { publish(); });
    p->failures[lane] = 0;
    p->session->setInitialLane(FailoverSession::Lane(p->initial));
    p->session->setAutomatic(effective(p));
    p->session->attach(FailoverSession::Lane(lane), link, false);
    connect(link, &FailoverLink::message, this, [this, p, lane](quint8 type, quint64, const QByteArray &data) {
      if (type == FailoverLink::Control && p->links[lane])
        control(p, lane, data);
    });
    connect(link, &FailoverLink::disconnected, this, [this, p, lane] {
      p->links[lane] = nullptr;
      if (!m_stopped && !p->stopped) {
        retry(p, lane);
        publish();
      }
    });
    offer(p);
    publish();
  });
  connect(link, &FailoverLink::disconnected, this, [this, wire, link, expected](const QString &reason) {
    qDebug() << "Managed channel closed" << expected.left(12) << reason;
    m_wires.remove(wire);
    for (auto *p : m_peers)
      for (int lane = 0; lane < 2; ++lane)
        if (p->pending[lane] == wire) {
          p->pending[lane] = nullptr;
          ++p->failures[lane];
          if (!p->id.isEmpty())
            retry(p, lane);
        }
    if (!m_options.server && !m_stopped && m_clientPeer.isEmpty()) {
      m_bootstrapRetry.start(m_bootstrapBackoff);
      m_bootstrapBackoff = qMin(8000, m_bootstrapBackoff * 2);
    }
    // Session-owned links are also deleteLater'd by the session; QObject deletion is idempotent.
    link->deleteLater();
  });
  link->start();
}
void ConnectionManager::interruptWire(QIODevice *wire)
{
  if (auto *tcp = qobject_cast<QTcpSocket *>(wire))
    tcp->abort();
  else if (auto *bt = qobject_cast<QBluetoothSocket *>(wire))
    bt->abort();
  else
    wire->close();
}
void ConnectionManager::dial(Peer *p, int lane)
{
  if (m_stopped || m_suspended || p->stopped || m_options.server || p->links[lane] || p->pending[lane])
    return;
  if (!p->id.isEmpty() && !effective(p) &&
      lane != (p->session->reconnectLane() >= 0 ? p->session->reconnectLane() : p->initial))
    return;
  if (lane == 1 && !m_btEnabled)
    return;
  QString host = m_options.host;
  quint16 port = m_options.port;
  if (lane == 0 && !p->endpoints.isEmpty() && p->endpointsExpireAt > QDateTime::currentSecsSinceEpoch()) {
    const auto candidate = p->endpoints[p->failures[0] % p->endpoints.size()];
    host = candidate.first;
    port = candidate.second;
  }
  if (lane == 0) {
    const auto hosts = host.split(',', Qt::SkipEmptyParts);
    if (!hosts.isEmpty())
      host = hosts[p->failures[0] % hosts.size()].trimmed();
    if (QHostAddress(host).isNull()) {
      const QUrl endpoint("tcp://" + host);
      if (!endpoint.host().isEmpty()) {
        host = endpoint.host();
        port = quint16(endpoint.port(port));
      }
    }
  }
  const QString bt = p->bt.isEmpty() ? m_options.bluetoothAddress : p->bt;
  if ((lane == 0 && host.isEmpty()) || (lane == 1 && QBluetoothAddress(bt).isNull())) {
    p->reason = lane == 0 ? "networkUnavailable" : "bluetoothTargetUnknown";
    publish();
    return;
  }
  QIODevice *wire = lane == 0
                        ? static_cast<QIODevice *>(new QTcpSocket(this))
                        : static_cast<QIODevice *>(new QBluetoothSocket(QBluetoothServiceInfo::RfcommProtocol, this));
  p->pending[lane] = wire;
  auto *timer = new QTimer(wire);
  timer->setObjectName("connectionDeadline");
  timer->setSingleShot(true);
  auto failed = [this, p, lane, wire] {
    if (p->pending[lane] != wire)
      return;
    p->pending[lane] = nullptr;
    wire->disconnect(this);
    interruptWire(wire);
    wire->deleteLater();
    p->reason = "addressUnreachable";
    ++p->failures[lane];
    if (p->id.isEmpty()) {
      if (!m_stopped)
        m_bootstrapRetry.start(qMin(8000, 1000 << qMin(3, p->failures[lane])));
    } else
      retry(p, lane);
    publish();
  };
  auto connected = [this, p, lane, wire] {
    if (p->pending[lane] != wire || m_stopped || p->stopped)
      return;
    wire->disconnect(this);
    adopt(wire, lane, p->id.isEmpty() ? m_options.initialPeer : p->id);
  };
  connect(timer, &QTimer::timeout, this, failed);
  timer->start(10000);
  if (auto *tcp = qobject_cast<QTcpSocket *>(wire)) {
    tcp->setProxy(QNetworkProxy::NoProxy);
    connect(tcp, &QTcpSocket::connected, this, connected);
    connect(tcp, &QTcpSocket::errorOccurred, this, failed);
    tcp->connectToHost(host, port);
  } else {
    auto *socket = static_cast<QBluetoothSocket *>(wire);
    connect(socket, &QBluetoothSocket::connected, this, connected);
    connect(socket, &QBluetoothSocket::errorOccurred, this, failed);
    socket->connectToService(QBluetoothAddress(bt), ManagedService);
  }
}
void ConnectionManager::retry(Peer *p, int lane)
{
  if (m_stopped || m_suspended || p->stopped || m_options.server || p->retry[lane]->isActive())
    return;
  const int active = p->session->reconnectLane() >= 0 ? p->session->reconnectLane() : p->initial;
  if (active != lane && !effective(p))
    return;
  p->retry[lane]->start(qMin(8000, 1000 << qMin(3, p->failures[lane])));
}
void ConnectionManager::prepare(Peer *p)
{
  if (m_stopped || m_suspended || p->stopped || p->suppressed || !p->connected || !effective(p))
    return;
  const int backup = 1 - p->session->selectedLane();
  if (backup < 0 || backup > 1 || p->links[backup] || p->pending[backup] || p->retry[backup]->isActive())
    return;
  if (backup == 1 && !m_btEnabled) {
    requestBluetooth(p);
    return;
  }
  if (m_options.server)
    return;
  if (backup == 1 && QBluetoothAddress(p->bt).isNull()) {
    requestBluetoothTarget(p);
    return;
  }
  if (backup == 1 && m_adapter &&
      m_adapter->pairingStatus(QBluetoothAddress(p->bt)) == QBluetoothLocalDevice::Unpaired) {
    if (p->action.isEmpty()) {
      p->action = "pairBluetooth";
      p->actionId = operationId();
      p->reason = "notPaired";
      publish();
    }
    return;
  }
  dial(p, backup);
}
void ConnectionManager::requestBluetooth(Peer *p)
{
  if (m_stopped || m_btRequesting || m_btEnabled || (m_options.server && !(m_options.serverTransports & 2)))
    return;
  // Only an explicit first Bluetooth connection or an authenticated peer can request this permission.
  if (!m_btReason.isEmpty()) {
    if (p) {
      p->reason = m_btReason;
      p->action = "bluetoothSettings";
      if (p->actionId.isEmpty())
        p->actionId = operationId();
    }
    publish();
    return;
  }
  m_btRequesting = true;
  const auto request = m_options.bluetoothPermission ? m_options.bluetoothPermission : requestBluetoothPermission;
  request(this, m_options.server, [this](bool granted) {
    m_btRequesting = false;
    if (m_stopped)
      return;
    if (!granted) {
      m_btReason = "bluetoothPermissionDenied";
      for (auto *p : m_peers)
        if (!p->stopped && (effective(p) || (p->id.isEmpty() && m_options.initialLane == 1))) {
          p->reason = m_btReason;
          p->action = "bluetoothSettings";
          if (p->actionId.isEmpty())
            p->actionId = operationId();
        }
      publish();
      return;
    }
    startBluetooth();
  });
}
void ConnectionManager::startBluetooth()
{
  if (m_stopped || m_btEnabled)
    return;
  m_adapter = new QBluetoothLocalDevice(this);
  if (!m_adapter->isValid() || m_adapter->hostMode() == QBluetoothLocalDevice::HostPoweredOff) {
    m_btReason = m_adapter->isValid() ? "bluetoothPoweredOff" : "bluetoothAdapterUnavailable";
    m_adapter->deleteLater();
    m_adapter = nullptr;
    for (auto *p : m_peers)
      if (!p->stopped && (effective(p) || (p->id.isEmpty() && m_options.initialLane == 1))) {
        p->reason = m_btReason;
        p->action = "bluetoothSettings";
        if (p->actionId.isEmpty())
          p->actionId = operationId();
      }
    publish();
    return;
  }
  m_btEnabled = true;
  m_btReason.clear();
  connect(m_adapter, &QBluetoothLocalDevice::hostModeStateChanged, this, [this](auto mode) {
    if (m_stopped)
      return;
    if (mode == QBluetoothLocalDevice::HostPoweredOff) {
      m_btReason = "bluetoothPoweredOff";
      for (auto *p : m_peers)
        if (p->links[1])
          p->links[1]->stop("Bluetooth powered off");
    } else {
      m_btReason.clear();
      if (m_options.server)
        listenBluetooth();
      for (auto *p : m_peers)
        if (!p->id.isEmpty())
          prepare(p);
    }
    publish();
  });
  if (m_options.server)
    listenBluetooth();
  else if (m_clientPeer.isEmpty() && m_options.initialLane == 1) {
    auto *initial = peer({});
    initial->bt = m_options.bluetoothAddress;
    if (initial->suppressed)
      return;
    if (m_adapter->pairingStatus(QBluetoothAddress(initial->bt)) == QBluetoothLocalDevice::Unpaired) {
      initial->action = "pairBluetooth";
      initial->actionId = operationId();
      initial->reason = "notPaired";
    } else
      dial(initial, 1);
  }
  for (auto *p : m_peers)
    if (!p->id.isEmpty()) {
      if (p->action == "bluetoothSettings") {
        p->action.clear();
        p->actionId.clear();
      }
      offer(p);
      prepare(p);
    }
  publish();
}
void ConnectionManager::listenBluetooth()
{
  if (m_stopped || !m_btEnabled)
    return;
  // Unregister while Qt can still resolve the old listener by its port.
  m_service.unregisterService();
  if (m_btServer) {
    m_btServer->disconnect(this);
    m_btServer->close();
    m_btServer->deleteLater();
  }
  m_btServer = new QBluetoothServer(QBluetoothServiceInfo::RfcommProtocol, this);
#ifndef Q_OS_MACOS
  m_btServer->setSecurityFlags(QBluetooth::Security::Encryption | QBluetooth::Security::Authentication);
#endif
  auto *server = m_btServer;
  connect(server, &QBluetoothServer::newConnection, this, [this, server] {
    while (server->hasPendingConnections())
      if (auto *wire = server->nextPendingConnection())
        adopt(wire, 1);
  });
  m_service = m_btServer->listen(ManagedService, "BMouse:" + QString::fromLatin1(m_instance.toHex()));
  if (!m_service.isRegistered())
    m_btReason = "bluetoothListenerUnavailable";
}
void ConnectionManager::requestBluetoothTarget(Peer *p)
{
  if (m_stopped || p->stopped || p->suppressed || !effective(p))
    return;
  // Authenticated capability exchange normally supplies the peer's address.
  // If it does not, ask the user instead of querying every nearby device's services.
  p->reason = "bluetoothTargetUnknown";
  if (p->action != "selectBluetoothDevice") {
    p->action = "selectBluetoothDevice";
    p->actionId = operationId();
  }
  publish();
}

void ConnectionManager::pairBluetooth(Peer *p)
{
  if (m_pairing || m_stopped || p->stopped || (!p->id.isEmpty() && !effective(p)))
    return;
  m_pairing = new BluetoothPairing(this);
  m_pairPeer = p->id;
  m_pairAction = p->actionId;
  auto *pairing = m_pairing;
  const auto current = [this, p] {
    return !m_stopped && !p->stopped && (p->id.isEmpty() || effective(p)) && m_pairPeer == p->id &&
           m_pairAction == p->actionId;
  };
  connect(pairing, &BluetoothPairing::confirmationRequired, this, [this, p, current](const QString &code) {
    if (current()) {
      p->action = "confirmPairing";
      p->reason = code;
      publish();
    }
  });
  connect(pairing, &BluetoothPairing::pinRequired, this, [this, p, current] {
    if (current()) {
      p->action = "pairingPin";
      publish();
    }
  });
  connect(pairing, &BluetoothPairing::passkeyDisplayed, this, [this, p, current](const QString &code) {
    if (current()) {
      p->action = "displayPairingCode";
      p->reason = code;
      publish();
    }
  });
  connect(pairing, &BluetoothPairing::finished, this, [this, p, pairing, current](bool ok, const QString &) {
    if (current()) {
      p->action.clear();
      p->actionId.clear();
      p->reason = ok ? QString() : "pairingFailed";
      if (ok)
        dial(p, 1);
    }
    m_pairPeer.clear();
    m_pairAction.clear();
    m_pairing = nullptr;
    pairing->deleteLater();
    publish();
  });
  pairing->pair(QBluetoothAddress(p->bt));
}
QJsonObject ConnectionManager::snapshot() const
{
  QJsonArray peers;
  for (auto *p : m_peers)
    if (!p->id.isEmpty() || (m_clientPeer.isEmpty() && !p->action.isEmpty())) {
      QJsonArray lanes;
      for (int lane = 0; lane < 2; ++lane) {
        QJsonObject channel{
                {"type", lane == 0 ? "network" : "bluetooth"},
                {"state", p->links[lane] && p->links[lane]->ready()
                              ? (p->session->selectedLane() == lane && p->connected ? "active"
                                 : p->links[lane]->healthy() && p->offered          ? "ready"
                                                                                    : "authenticating")
                          : p->pending[lane]           ? "connecting"
                          : p->retry[lane]->isActive() ? "retrying"
                                                       : "unavailable"},
                {"retryInMs", p->retry[lane]->remainingTime()},
                {"remoteAddress", p->links[lane] && p->links[lane]->ready() ? p->remoteAddresses[lane] : QString()}
        };
        if (p->links[lane]) {
          const auto metrics = p->links[lane]->statistics();
          for (auto it = metrics.begin(); it != metrics.end(); ++it) channel.insert(it.key(), it.value());
        }
        lanes.append(channel);
      }
      peers.append(
          QJsonObject{
              {"peerId", p->id.isEmpty() ? QString("bootstrap") : p->id},
              {"name", p->name.isEmpty() ? m_options.bluetoothAddress : p->name},
              {"sessionId", p->remoteSession},
              {"state", p->stopped ? "stopped" : p->state},
              {"requested", p->requested},
              {"policySource", p->explicitPolicy ? "peer" : "default"},
              {"effective", effective(p)},
              {"reason", p->reason},
              {"action", p->action},
              {"actionId", p->actionId},
              {"channels", lanes},
              {"clipboardKiB", effective(p) ? 64 : 0},
              {"backup", p->stopped                                  ? "disabled"
                         : !p->requested                             ? "disabled"
                         : m_options.server && m_backupPeer != p->id ? "needsSelection"
                         : !p->offered                               ? "negotiating"
                         : !p->remoteAllows                          ? "blockedByPeer"
                         : (p->links[0] && p->links[1] && p->links[0]->healthy() && p->links[1]->healthy()) ? "ready"
                         : !p->action.isEmpty()                                     ? "needsAction"
                         : p->session->selectedLane() == 0 && !m_btReason.isEmpty() ? "unavailable"
                                                                                    : "preparing"}
          }
      );
    }
  return {
      {"version", 2},
      {"revision", double(m_revision)},
      {"peers", peers},
      {"bluetoothReason", m_btReason},
      {"networkListening", m_options.server && m_server.isListening()},
      {"bluetoothListening", m_options.server && m_service.isRegistered() && m_btReason.isEmpty()},
      {"backupPeer", m_backupPeer},
      {"capabilities", QJsonObject{
                           {"protocol", 2},
                           {"platform", QSysInfo::productType()},
                           {"server", m_options.server},
                           {"transports", QJsonArray{"network", "bluetooth"}},
                           {"bluetoothState", m_btEnabled ? (m_btReason.isEmpty() ? QString("available") : m_btReason)
                                                          : (m_btReason.isEmpty() ? QString("unknown") : m_btReason)},
#ifdef Q_OS_LINUX
                           {"pairingAgent", "systemAgentRequired"}
#else
                           {"pairingAgent", "native"}
#endif
                       }}
  };
}
void ConnectionManager::publish()
{
  for(auto* peer:m_peers)if(peer->id.size()==64&&peer->offered){
    QJsonArray keys{QString("peer:")+peer->id};
    auto append=[&](QString host,int port){
      if(host.isEmpty()||port<1||port>65535)return;if(host.contains(':')&&!host.startsWith('['))host='['+host+']';
      const auto key="server:"+host.toLower()+":"+QString::number(port);if(!keys.contains(key))keys.append(key);
    };
    // Each alias is supplied by this authenticated peer's current control lane.
    for(const auto& endpoint:peer->endpoints)append(endpoint.first,endpoint.second);
    const auto targets=m_options.host.split(',',Qt::SkipEmptyParts);
    if(peer->initial==0&&targets.size()==1){QUrl target("tcp://"+targets.first().trimmed());if(target.isValid())append(target.host(),target.port(m_options.port));}
    const auto name=peer->name.isEmpty()?QString("Computer ")+peer->id.left(12):peer->name.left(80);
    // Initial Bluetooth plus an enabled network backup is not Bluetooth-only.
    const bool networkAllowed=m_options.server?(m_options.serverTransports&1)!=0:(peer->initial==0||peer->requested);
    PeerConnectionRegistry::uiContext(peer->id,{{"peerName",name},{"deviceKeys",keys},
      {"displayNetworkPolicy",networkAllowed?"networkAllowed":"bluetoothOnly"}});
  }
  ++m_revision;
  Q_EMIT changed(snapshot());
}
void ConnectionManager::coreConnected(const QString &id, bool connected)
{
  const auto key = m_options.server ? id : m_clientPeer;
  if (m_peers.contains(key) && !key.isEmpty())
    m_peers[key]->session->coreConnected(connected);
}
void ConnectionManager::closePeer(Peer *p)
{
  if (p->id.isEmpty())
    m_bootstrapRetry.stop();
  p->stopped = true;
  p->connected = false;
  p->action.clear();
  p->actionId.clear();
  for (int lane = 0; lane < 2; ++lane) {
    p->retry[lane]->stop();
    if (p->pending[lane])
      interruptWire(p->pending[lane]);
  }
  p->session->stop();
  Q_EMIT clipboardLimit(p->id, 0);
  if (m_pairPeer == p->id && m_pairing) {
    m_pairing->cancel();
    m_pairing->deleteLater();
    m_pairing = nullptr;
    m_pairPeer.clear();
  }
  if (m_discovery) {
    m_discovery->stop();
  }
}
void ConnectionManager::stop()
{
  if (m_stopped)
    return;
  m_stopped = true;
  m_networkTimer.stop();
  m_bootstrapRetry.stop();
  for (auto *p : m_peers)
    closePeer(p);
  const auto wires = m_wires;
  for (auto *wire : wires)
    interruptWire(wire);
  m_server.close();
  m_local.close();
  m_service.unregisterService();
  if (m_btServer)
    m_btServer->close();
  publish();
}
void ConnectionManager::command(const QJsonObject &request)
{
  QString error;
  const auto requestId = request.value("operationId").toString(operationId());
  const auto completion = qScopeGuard([&] {
    Q_EMIT operationFinished(
        {{"operationId", requestId},
         {"kind", request.value("kind")},
         {"peerId", request.value("peerId")},
         {"accepted", error.isEmpty()},
         {"error", error}}
    );
  });
  const auto requestedKind = request.value("kind").toString();
  const QStringList kinds{"snapshot",      "capabilities", "defaultPolicy", "disconnectName", "stopPeer",
                          "reconnectPeer", "policy",       "retryBackup",   "retryBluetooth", "action"};
  if (!kinds.contains(requestedKind)) {
    error = "unknownCommand";
    return;
  }
  if ((requestedKind == "policy" || requestedKind == "defaultPolicy") && !request.value("enabled").isBool()) {
    error = "invalidPolicy";
    return;
  }
  if (m_stopped) {
    error = "stopped";
    return;
  }
  if (requestedKind == "retryBluetooth") {
    if (m_options.initialLane != 1 && !(m_options.server && (m_options.serverTransports & 2))) {
      error = "actionUnavailable";
      return;
    }
    m_btReason.clear();
    if (m_peers.contains({}) && !m_peers.value({})->stopped)
      m_peers.value({})->suppressed = false;
    requestBluetooth(nullptr);
    publish();
    return;
  }
  if (request.value("kind") == "defaultPolicy" && request.value("enabled").isBool()) {
    m_options.automatic = request.value("enabled").toBool();
    for (auto *candidate : m_peers)
      if (!candidate->id.isEmpty() && !candidate->stopped && !candidate->explicitPolicy)
        command({{"kind", "policy"}, {"peerId", candidate->id}, {"enabled", m_options.automatic}, {"inherited", true}});
    publish();
    return;
  }
  auto id = request.value("peerId").toString();
  if (request.value("kind") == "disconnectName" && !m_options.server)
    id = m_clientPeer;
  else if (request.value("kind") == "disconnectName") {
    const auto name = request.value("name").toString();
    for (auto *candidate : m_peers)
      if (candidate->name == name) {
        id = candidate->id;
        break;
      }
  }
  auto *p = m_peers.value(id == "bootstrap" ? QString() : id, nullptr);
  if (request.value("kind") == "snapshot" || request.value("kind") == "capabilities") {
    publish();
    return;
  }
  if (!p) {
    error = "unknownPeer";
    return;
  }
  id = p->id;
  const auto kind = request.value("kind").toString();
  if (kind == "reconnectPeer" && p->stopped && !p->id.isEmpty()) {
    if (!m_options.trusted(QByteArray::fromHex(p->id.toLatin1()))) {
      error = "identityUntrusted";
      return;
    }
    delete p->session;
    p->session = nullptr;
    p->stopped = false;
    p->suppressed = false;
    p->connected = false;
    p->offered = false;
    p->remoteAllows = false;
    p->state = "connecting";
    p->remoteSession.clear();
    p->reason.clear();
    p->links[0] = nullptr;
    p->links[1] = nullptr;
    p->pending[0] = nullptr;
    p->pending[1] = nullptr;
    setupSession(p);
    if (!m_options.server)
      dial(p, p->initial);
    publish();
    return;
  }
  if (p->stopped) {
    error = "stopped";
    return;
  }
  if (kind == "action" && (request.value("actionId") != p->actionId || p->actionId.isEmpty())) {
    error = "actionExpired";
    return;
  }
  if (kind == "action" && !request.value("cancel").toBool()) {
    const auto pin = request.value("pin").toString().toUtf8();
    if (p->action == "pairingPin" && (pin.isEmpty() || pin.size() > 16)) {
      error = "invalidPin";
      return;
    }
    if (p->action == "confirmPairing" && !request.value("accepted").isBool()) {
      error = "invalidResponse";
      return;
    }
  }
  if (kind == "stopPeer" || kind == "disconnectName") {
    const auto payload =
        QJsonDocument(
            QJsonObject{{"version", 2}, {"kind", "stop"}, {"instance", QString::fromLatin1(m_instance.toHex())}}
        ).toJson(QJsonDocument::Compact);
    for (auto &link : p->links)
      if (link)
        link->send(FailoverLink::Control, 0, payload);
    closePeer(p);
  } else if (kind == "policy") {
    if (p->id.isEmpty())
      return;
    p->explicitPolicy = !request.value("inherited").toBool();
    p->requested = request.value("enabled").toBool();
    p->suppressed = false;
    if (request.value("select").toBool() && m_options.server) {
      m_backupPeer = id;
      m_explicitTarget = true;
    }
    p->session->setAutomatic(effective(p));
    if (m_options.savePeer)
      m_options.savePeer(
          id, {{"automatic", p->requested},
               {"automaticExplicit", p->explicitPolicy},
               {"bluetooth", p->bt},
               {"name", p->name}}
      );
    offer(p);
    // Apply the new intersection locally without waiting for the remote acknowledgement.
    if (!effective(p) && p->session->selectedLane() >= 0) {
      const int backup = 1 - p->session->selectedLane();
      p->retry[backup]->stop();
      if (p->pending[backup])
        interruptWire(p->pending[backup]);
      p->session->removeLane(FailoverSession::Lane(backup));
    }
    Q_EMIT clipboardLimit(p->id, effective(p) ? 64 : 0);
    prepare(p);
    for (auto *other : m_peers)
      if (other != p && !other->id.isEmpty() && !other->stopped) {
        other->session->setAutomatic(effective(other));
        if (!effective(other) && other->session->selectedLane() >= 0) {
          const int standby = 1 - other->session->selectedLane();
          other->retry[standby]->stop();
          if (other->pending[standby]) {
            auto *wire = other->pending[standby].data();
            other->pending[standby] = nullptr;
            wire->disconnect(this);
            interruptWire(wire);
            wire->deleteLater();
          }
          other->session->removeLane(FailoverSession::Lane(standby));
        }
        Q_EMIT clipboardLimit(other->id, effective(other) ? 64 : 0);
        offer(other);
      }
  } else if (kind == "retryBackup") {
    p->suppressed = false;
    m_btReason.clear();
    if (p->id.isEmpty() && m_options.initialLane == 1) {
      p->action = "pairBluetooth";
      p->actionId = operationId();
      if (!m_btEnabled)
        requestBluetooth(p);
      else
        pairBluetooth(p);
    } else
      prepare(p);
  } else if (kind == "action" && request.value("actionId") == p->actionId && !p->actionId.isEmpty()) {
    if (request.value("cancel").toBool()) {
      if (m_pairing && m_pairPeer == id) {
        m_pairing->cancel();
        m_pairing->deleteLater();
        m_pairing = nullptr;
        m_pairPeer.clear();
      }
      p->suppressed = true;
      p->action.clear();
      p->actionId.clear();
      p->reason = "cancelled";
      const int standby = 1 - p->session->selectedLane();
      if (standby >= 0 && standby < 2) {
        p->retry[standby]->stop();
        if (p->pending[standby]) {
          auto *wire = p->pending[standby].data();
          p->pending[standby] = nullptr;
          wire->disconnect(this);
          interruptWire(wire);
          wire->deleteLater();
        }
      }
    } else if (p->action == "pairBluetooth") {
      pairBluetooth(p);
    } else if (p->action == "confirmPairing" && m_pairing && m_pairPeer == id) {
      m_pairing->confirm(request.value("accepted").toBool());
    } else if (p->action == "pairingPin" && m_pairing && m_pairPeer == id) {
      m_pairing->providePin(request.value("pin").toString());
    } else if (p->action == "selectBluetoothDevice") {
      const QBluetoothAddress addr(request.value("address").toString());
      if (!addr.isNull()) {
        p->bt = addr.toString();
        p->action = "pairBluetooth";
        p->actionId = operationId();
        pairBluetooth(p);
      }
    } else if (p->action == "bluetoothSettings") {
      m_btReason.clear();
      requestBluetooth(p);
    }
  }
  publish();
}

ConnectionManager::~ConnectionManager()
{
  stop();
  for (auto *p : m_peers) {
    delete p->session;
    delete p;
  }
}

void ConnectionManager::validateTrust()
{
  for (auto *p : m_peers)
    if (!p->id.isEmpty() && !p->stopped && !m_options.trusted(QByteArray::fromHex(p->id.toLatin1()))) {
      closePeer(p);
      p->reason = "trustRevoked";
      if (m_options.savePeer)
        m_options.savePeer(p->id, {{"revoked", true}});
    }
}

void ConnectionManager::suspend(bool sleeping)
{
  if (m_stopped || m_suspended == sleeping)
    return;
  m_suspended = sleeping;
  if (sleeping) {
    m_networkTimer.stop();
    m_bootstrapRetry.stop();
    if (m_pairing) {
      m_pairing->cancel();
      m_pairing->deleteLater();
      m_pairing = nullptr;
      m_pairPeer.clear();
    }
    for (auto *p : m_peers) {
      p->action.clear();
      p->actionId.clear();
      for (int lane = 0; lane < 2; ++lane) {
        p->retry[lane]->stop();
        if (p->pending[lane])
          interruptWire(p->pending[lane]);
        if (p->links[lane])
          p->links[lane]->stop("System sleep");
      }
    }
  } else {
    m_networkTimer.start();
    if (m_options.server && m_btEnabled)
      listenBluetooth();
    if (!m_options.server) {
      if (m_clientPeer.isEmpty())
        dial(peer({}), m_options.initialLane);
      else
        for (auto *p : m_peers)
          if (!p->id.isEmpty() && !p->stopped) {
            dial(p, effective(p) ? 0 : (p->session->reconnectLane() >= 0 ? p->session->reconnectLane() : p->initial));
            if (effective(p))
              dial(p, 1);
          }
    }
  }
  publish();
}
