// SPDX-License-Identifier: MIT
#include "RfcommSession.h"
#include <QBluetoothSocket>
#include <QDateTime>
#include <QDebug>
#include <QtEndian>

QByteArray RfcommSession::frame(Type type, const QByteArray &payload)
{
  QByteArray result(5, '\0');
  result[0] = char(type);
  qToBigEndian<quint32>(quint32(payload.size()), result.data() + 1);
  result += payload;
  return result;
}

RfcommSession::RfcommSession(QIODevice *wire, bool initiator, bool probe, quint16 corePort, QTcpSocket *local)
    : m_wire(wire),
      m_initiator(initiator),
      m_probe(probe),
      m_corePort(corePort)
{
  wire->setParent(this);
  connect(wire, &QIODevice::readyRead, this, &RfcommSession::receive);
  connect(wire, &QIODevice::bytesWritten, this, [this] { pumpLocal(); });
  if (auto *bt = qobject_cast<QBluetoothSocket *>(wire)) {
    connect(bt, &QBluetoothSocket::disconnected, this, [this] { stop("bluetooth_disconnected"); });
    connect(bt, &QBluetoothSocket::errorOccurred, this, [this, bt] { stop(bt->errorString()); });
  } else if (auto *tcp = qobject_cast<QTcpSocket *>(wire)) {
    connect(tcp, &QTcpSocket::disconnected, this, [this] { stop("wire_disconnected"); });
    connect(tcp, &QTcpSocket::errorOccurred, this, [this, tcp] { stop(tcp->errorString()); });
  }
  if (local)
    attachLocal(local);
  m_tick.setInterval(1000);
  connect(&m_tick, &QTimer::timeout, this, [this] {
    if (m_clock.elapsed() - m_lastReceive >= 10000 ||
        (!m_pendingPing.isEmpty() && m_clock.nsecsElapsed() - m_pingTime >= 10000000000LL)) {
      stop("receive_timeout");
      return;
    }
    if (m_ready && m_pendingPing.isEmpty()) {
      m_pendingPing = QByteArray::number(++m_sequence);
      m_pingTime = m_clock.nsecsElapsed();
      send(Ping, m_pendingPing);
    }
  });
}

void RfcommSession::start()
{
  m_clock.start();
  m_tick.start();
  if (m_initiator)
    send(Hello, m_probe ? "DFBT1 probe" : "DFBT1 bridge");
  receive();
}

void RfcommSession::stop(const QString &reason)
{
  if (m_stopped)
    return;
  m_stopped = true;
  m_tick.stop();
  if (m_local)
    m_local->abort(); // Discard old events; Deskflow must establish a fresh TLS session.
  if (auto *bt = qobject_cast<QBluetoothSocket *>(m_wire))
    bt->abort();
  else if (auto *tcp = qobject_cast<QTcpSocket *>(m_wire))
    tcp->abort();
  else
    m_wire->close();
  qInfo().noquote() << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) << "session_closed reason=" << reason
                    << "sent=" << m_sent << "received=" << m_received;
  Q_EMIT closed(reason);
  Q_EMIT finished();
}

bool RfcommSession::send(Type type, const QByteArray &payload)
{
  if (m_stopped)
    return false;
  const auto packet = frame(type, payload);
  if (m_wire->bytesToWrite() + packet.size() > MaxQueue || m_wire->write(packet) != packet.size()) {
    stop("wire_write_failed_or_queue_limit");
    return false;
  }
  return true;
}

void RfcommSession::receive()
{
  if (m_stopped)
    return;
  // Read only one bounded frame at a time, including fragmented headers/payloads.
  while (!m_stopped && m_wire->bytesAvailable()) {
    const auto target = m_input.size() < 5 ? 5 : 5 + qFromBigEndian<quint32>(m_input.constData() + 1);
    m_input += m_wire->read(target - m_input.size());
    if (m_input.size() < 5)
      return;
    const auto size = qFromBigEndian<quint32>(m_input.constData() + 1);
    if (size > MaxPayload) {
      stop("oversized_frame");
      return;
    }
    if (m_input.size() < 5 + size)
      continue;
    const auto type = Type(quint8(m_input[0]));
    const auto payload = m_input.mid(5);
    m_input.clear();
    m_lastReceive = m_clock.elapsed();
    handle(type, payload);
  }
}

void RfcommSession::handle(Type type, const QByteArray &payload)
{
  if (!m_hello && !m_initiator) {
    if (type != Hello || (payload != "DFBT1 probe" && payload != "DFBT1 bridge")) {
      stop("invalid_hello");
      return;
    }
    m_hello = true;
    m_probe = payload == "DFBT1 probe";
    if (m_probe) {
      send(Ready);
      becomeReady();
    } else {
      auto *local = new QTcpSocket;
      attachLocal(local);
      connect(local, &QTcpSocket::connected, this, [this] {
        send(Ready);
        becomeReady();
      });
      local->connectToHost(QHostAddress::LocalHost, m_corePort);
    }
    return;
  }
  if (m_initiator && !m_ready && type == Ready && payload.isEmpty()) {
    becomeReady();
    return;
  }
  if (!m_ready) {
    stop("frame_before_ready");
    return;
  }
  switch (type) {
  case Data:
    if (m_probe || !m_local || payload.isEmpty() || m_local->bytesToWrite() + payload.size() > MaxQueue ||
        m_local->write(payload) != payload.size()) {
      stop("local_write_failed_or_invalid_data");
      return;
    }
    m_received += payload.size();
    break;
  case Ping:
    if (payload.isEmpty() || payload.size() > 32) {
      stop("invalid_ping");
      return;
    }
    send(Pong, payload);
    break;
  case Pong:
    if (m_pendingPing.isEmpty() || payload != m_pendingPing) {
      stop("unexpected_pong");
      return;
    }
    {
      const double ms = double(m_clock.nsecsElapsed() - m_pingTime) / 1000000.0;
      qInfo().noquote() << "rtt_ms=" << QString::number(ms, 'f', 3);
      m_pendingPing.clear();
      Q_EMIT sample(ms);
    }
    break;
  default:
    stop("unexpected_frame_type");
  }
}

void RfcommSession::attachLocal(QTcpSocket *socket)
{
  m_local = socket;
  socket->setParent(this);
  socket->setReadBufferSize(MaxQueue);
  socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
  connect(socket, &QTcpSocket::readyRead, this, &RfcommSession::pumpLocal);
  connect(socket, &QTcpSocket::disconnected, this, [this] { stop("local_disconnected"); });
  connect(socket, &QTcpSocket::errorOccurred, this, [this, socket] { stop(socket->errorString()); });
}

void RfcommSession::pumpLocal()
{
  if (!m_ready || m_stopped || !m_local)
    return;
  // Backpressure keeps clipboard bursts from growing the Bluetooth write queue indefinitely.
  while (m_local->bytesAvailable() && m_wire->bytesToWrite() < 2 * MaxPayload) {
    const auto bytes = m_local->read(MaxPayload);
    if (bytes.isEmpty() || !send(Data, bytes))
      return;
    m_sent += bytes.size();
  }
}

void RfcommSession::becomeReady()
{
  if (m_stopped)
    return;
  m_ready = true;
  qInfo().noquote() << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
                    << "session_ready mode=" << (m_probe ? "probe" : "bridge");
  Q_EMIT ready();
  pumpLocal();
}
