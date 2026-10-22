// SPDX-License-Identifier: MIT
#include "FailoverSession.h"

FailoverSession::FailoverSession(bool server, QObject *parent) : QObject(parent), m_server(server)
{
  m_offlineDeadline.setSingleShot(true);
  m_offlineDeadline.setInterval(2000);
  connect(&m_offlineDeadline, &QTimer::timeout, this, [this] {
    if (!m_stopping && m_recovering)
      Q_EMIT transportState("offline");
  });
  m_activationDeadline.setSingleShot(true);
  m_activationDeadline.setInterval(8000);
  connect(&m_activationDeadline, &QTimer::timeout, this, [this] {
    const int lane = m_selected;
    dropCore(true);
    if (lane >= 0 && m_links[lane])
      m_links[lane]->stop("Activation timeout");
  });
}

bool FailoverSession::available(int lane) const
{
  return lane >= 0 && m_links[lane] && m_links[lane]->ready();
}

void FailoverSession::attach(Lane lane, FailoverLink *link, bool startLink)
{
  if (m_stopping || m_links[lane]) {
    link->stop("Duplicate or stopped channel");
    link->deleteLater();
    return;
  }
  link->setParent(this);
  m_links[lane] = link;
  connect(link, &FailoverLink::established, this, [this, lane, link] {
    if (m_links[lane] != link)
      return;
    if (!m_peerId.isEmpty() && link->peerInstance() != m_peerId) {
      link->stop("Channels belong to different peer instances");
      return;
    }
    m_peerId = link->peerInstance();
  });
  connect(link, &FailoverLink::message, this, [this, lane, link](quint8 type, quint64 epoch, const auto &data) {
    if (m_links[lane] == link)
      handle(lane, type, epoch, data);
  });
  connect(link, &FailoverLink::writable, this, &FailoverSession::pump);
  connect(link, &FailoverLink::disconnected, this, [this, lane, link] {
    if (m_links[lane] != link)
      return;
    m_links[lane] = nullptr;
    if (m_selected == lane) {
      dropCore(false);
      m_preferred = Network;
      m_selected = -1;
      interrupted();
    }
    if (!m_links[0] && !m_links[1]) {
      m_peerId.clear();
      if (m_server)
        m_lastEpoch = 0;
    }
    link->deleteLater();
  });
  if (link->ready()) {
    if (!m_peerId.isEmpty() && link->peerInstance() != m_peerId) {
      link->stop("Channels belong to different peer instances");
      return;
    }
    m_peerId = link->peerInstance();
  }
  if (startLink)
    link->start();
}

void FailoverSession::acceptLocal(QTcpSocket *socket)
{
  if (m_stopping || m_server || m_core) {
    socket->abort();
    socket->deleteLater();
    return;
  }
  const int preferred = !m_automatic && m_fixedLane >= 0 ? m_fixedLane : m_preferred;
  const int lane = available(preferred) ? preferred : (m_automatic && available(1 - preferred) ? 1 - preferred : -1);
  if (lane < 0) {
    socket->abort();
    socket->deleteLater();
    return;
  }
  m_selected = m_preferred = m_fixedLane = lane;
  ++m_epoch;
  attachCore(socket);
  m_activationDeadline.start();
  m_links[lane]->send(FailoverLink::Activate, m_epoch);
}

void FailoverSession::attachCore(QTcpSocket *socket)
{
  m_core = socket;
  m_dataReady = false;
  m_coreReported = false;
  socket->setParent(this);
  socket->setReadBufferSize(TlsChannel::Limit);
  socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
  connect(socket, &QTcpSocket::readyRead, this, &FailoverSession::pump);
  connect(socket, &QTcpSocket::bytesWritten, this, [this, socket] {
    if (m_core == socket && available(m_selected) && socket->bytesToWrite() < TlsChannel::Limit / 4)
      m_links[m_selected]->pauseRead(false);
  });
  connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
    if (m_core == socket)
      dropCore(true);
  });
  connect(socket, &QTcpSocket::errorOccurred, this, [this, socket] {
    if (m_core == socket)
      dropCore(true);
  });
}

void FailoverSession::dropCore(bool tellPeer)
{
  m_activationDeadline.stop();
  if (available(m_selected))
    m_links[m_selected]->pauseRead(false);
  m_dataReady = false;
  if (m_core) {
    auto *socket = m_core.data();
    m_core = nullptr;
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    if (tellPeer && available(m_selected))
      m_links[m_selected]->send(FailoverLink::Close, m_epoch);
  }
}

void FailoverSession::handle(int lane, quint8 type, quint64 epoch, const QByteArray &data)
{
  if (m_stopping || type == FailoverLink::Control)
    return;
  if (type == FailoverLink::Activate && m_server) {
    if ((!m_automatic && m_fixedLane >= 0 && lane != m_fixedLane) || epoch <= m_lastEpoch)
      return; // A delayed activation from an old lane must never replace the new session.
    m_lastEpoch = epoch;
    const bool replaced = m_core != nullptr;
    dropCore(false);
    m_selected = m_fixedLane = lane;
    m_epoch = epoch;
    if (replaced)
      interrupted();
    auto *socket = new QTcpSocket;
    attachCore(socket);
    connect(socket, &QTcpSocket::connected, this, [this, socket, lane, epoch] {
      if (m_core != socket || m_epoch != epoch || m_selected != lane)
        return;
      m_dataReady = true;
      m_activationDeadline.stop();
      m_links[lane]->send(FailoverLink::Ready, epoch);
      pump();
    });
    m_activationDeadline.start();
    socket->connectToHost(QHostAddress::LocalHost, m_corePort);
    return;
  }
  if (lane != m_selected || epoch != m_epoch)
    return; // Old close/data callbacks cannot affect the active generation.
  if (type == FailoverLink::Close) {
    dropCore(false);
  } else if (type == FailoverLink::Ready && !m_server && m_core) {
    m_dataReady = true;
    m_activationDeadline.stop();
    pump();
  } else if (type == FailoverLink::Data && m_dataReady && m_core && !data.isEmpty()) {
    if (m_core->bytesToWrite() + data.size() > TlsChannel::Limit || m_core->write(data) != data.size())
      dropCore(true);
    else if (m_core->bytesToWrite() >= TlsChannel::Limit / 2)
      m_links[lane]->pauseRead(true);
  } else if (type == FailoverLink::Data && !m_core) {
    return; // A retired generation never forwards its remaining input into another core.
  } else {
    m_links[lane]->stop("Unexpected session message");
  }
}

void FailoverSession::pump()
{
  if (!m_dataReady || !m_core || !available(m_selected))
    return;
  auto *link = m_links[m_selected].data();
  while (m_core && m_core->bytesAvailable() && link->queued() < 2 * FailoverLink::MaxData) {
    const auto bytes = m_core->read(FailoverLink::MaxData);
    if (bytes.isEmpty() || !link->send(FailoverLink::Data, m_epoch, bytes))
      break;
  }
}

void FailoverSession::interrupted()
{
  if (m_stopping || !m_hadCore || m_recovering)
    return;
  m_recovering = true;
  m_offlineDeadline.start();
  Q_EMIT transportState("switching");
}

void FailoverSession::coreConnected(bool connected)
{
  if (m_stopping)
    return;
  m_coreReported = connected;
  if (!connected && m_hadCore && !m_recovering)
    Q_EMIT transportState("connecting");
  if (connected && m_dataReady && m_core) {
    const auto epoch = m_epoch;
    QTimer::singleShot(500, this, [this, epoch] {
      if (m_stopping || !m_coreReported || !m_dataReady || !m_core || m_epoch != epoch)
        return;
      m_hadCore = true;
      m_recovering = false;
      m_offlineDeadline.stop();
      Q_EMIT transportState(m_selected == Network ? "network" : "bluetooth");
    });
  }
}

void FailoverSession::stop()
{
  if (m_stopping)
    return;
  m_stopping = true;
  m_offlineDeadline.stop();
  dropCore(false);
  for (auto &link : m_links) {
    if (link)
      link->stop("Transport stopped");
  }
}

void FailoverSession::removeLane(Lane lane)
{
  if (m_links[lane] && m_selected != lane)
    m_links[lane]->stop("Backup disabled");
}
