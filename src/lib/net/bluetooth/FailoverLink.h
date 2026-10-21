// SPDX-License-Identifier: MIT
#pragma once
#include "TlsChannel.h"
#include <QElapsedTimer>
#include <QTimer>

class FailoverLink : public QObject
{
  Q_OBJECT
public:
  enum Message : quint8
  {
    Hello = 1,
    Activate,
    Ready,
    Data,
    Close,
    Ping,
    Pong
  };
  static constexpr int MaxData = 16384;
  FailoverLink(
      TlsChannel *channel, bool server, const QByteArray &instanceId, QObject *parent, int heartbeatMs = 1000,
      int timeoutMs = 6000
  );
  void start();
  void stop(const QString &reason);
  bool send(Message type, quint64 epoch = 0, const QByteArray &data = {});
  bool ready() const
  {
    return m_ready && !m_stopped;
  }
  qint64 queued() const
  {
    return m_output.size() + m_channel->bytesToWrite();
  }
  QByteArray peerInstance() const
  {
    return m_peer;
  }
Q_SIGNALS:
  void established();
  void message(quint8 type, quint64 epoch, const QByteArray &data);
  void writable();
  void disconnected(const QString &reason);

private:
  void receive();
  void flush();
  TlsChannel *m_channel;
  bool m_server, m_ready = false, m_stopped = false;
  QByteArray m_id, m_peer, m_input, m_output;
  QTimer m_tick;
  QElapsedTimer m_clock;
  qint64 m_pingAt = 0;
  quint64 m_ping = 0, m_nextPing = 0;
  int m_timeout;
};
