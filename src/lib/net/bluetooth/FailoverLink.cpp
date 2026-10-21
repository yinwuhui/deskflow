// SPDX-License-Identifier: MIT
#include "FailoverLink.h"
#include <QtEndian>

FailoverLink::FailoverLink(
    TlsChannel *channel, bool server, const QByteArray &id, QObject *parent, int heartbeatMs, int timeoutMs
)
    : QObject(parent),
      m_channel(channel),
      m_server(server),
      m_id(id),
      m_timeout(timeoutMs)
{
  channel->setParent(this);
  m_tick.setInterval(heartbeatMs);
  connect(channel, &TlsChannel::authenticated, this, [this] {
    send(Hello, 0, QByteArray("DFF1") + char(m_server ? 1 : 0) + m_id);
  });
  connect(channel, &TlsChannel::readyRead, this, &FailoverLink::receive);
  connect(channel, &TlsChannel::bytesWritten, this, [this] {
    flush();
    Q_EMIT writable();
  });
  connect(channel, &TlsChannel::disconnected, this, [this](const QString &reason) { stop(reason); });
  connect(&m_tick, &QTimer::timeout, this, [this] {
    if ((!m_ready && m_clock.elapsed() >= m_timeout) || (m_ping && m_clock.elapsed() - m_pingAt >= m_timeout)) {
      stop("Channel heartbeat timeout");
      return;
    }
    if (m_ready && !m_ping) {
      m_ping = ++m_nextPing;
      m_pingAt = m_clock.elapsed();
      send(Ping, m_ping);
    }
  });
}

void FailoverLink::start()
{
  m_clock.start();
  m_tick.start();
  m_channel->start();
}

void FailoverLink::stop(const QString &reason)
{
  if (m_stopped)
    return;
  m_stopped = true;
  m_tick.stop();
  m_input.clear();
  m_output.clear();
  m_channel->abort(reason);
  Q_EMIT disconnected(reason);
}

bool FailoverLink::send(Message type, quint64 epoch, const QByteArray &data)
{
  if (m_stopped || data.size() > MaxData || queued() + data.size() + 13 > TlsChannel::Limit) {
    stop("Channel send queue limit");
    return false;
  }
  QByteArray frame(13, '\0');
  frame[0] = char(type);
  qToBigEndian<quint32>(quint32(data.size()), frame.data() + 1);
  qToBigEndian<quint64>(epoch, frame.data() + 5);
  m_output += frame;
  m_output += data;
  flush();
  return !m_stopped;
}

void FailoverLink::flush()
{
  while (!m_stopped && !m_output.isEmpty() && m_channel->encrypted()) {
    const auto n = m_channel->write(m_output);
    if (n < 0) {
      stop("Channel write failed");
      return;
    }
    if (!n)
      break;
    m_output.remove(0, n);
  }
}

void FailoverLink::receive()
{
  while (!m_stopped && m_channel->bytesAvailable()) {
    const qint64 target = m_input.size() < 13 ? 13 : 13 + qFromBigEndian<quint32>(m_input.constData() + 1);
    const auto part = m_channel->read(target - m_input.size());
    if (part.isEmpty())
      return;
    m_input += part;
    if (m_input.size() < 13)
      return;
    const auto size = qFromBigEndian<quint32>(m_input.constData() + 1);
    if (size > MaxData) {
      stop("Oversized channel frame");
      return;
    }
    if (m_input.size() != 13 + size)
      continue;
    const auto type = Message(quint8(m_input[0]));
    const auto epoch = qFromBigEndian<quint64>(m_input.constData() + 5);
    const auto data = m_input.mid(13);
    m_input.clear();
    if (!m_ready) {
      if (type != Hello || epoch || data.size() != 21 || data.left(4) != "DFF1" || data[4] != char(m_server ? 0 : 1)) {
        stop("Invalid channel handshake");
        return;
      }
      m_peer = data.mid(5);
      m_ready = true;
      Q_EMIT established();
    } else if (type == Ping && epoch && data.isEmpty()) {
      send(Pong, epoch);
    } else if (type == Pong && m_ping && epoch == m_ping && data.isEmpty()) {
      m_ping = 0;
    } else if (type >= Activate && type <= Close && epoch && (type == Data || data.isEmpty())) {
      Q_EMIT message(type, epoch, data);
    } else {
      stop("Invalid channel message");
      return;
    }
  }
}
