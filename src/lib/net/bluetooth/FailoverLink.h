// SPDX-License-Identifier: MIT
#pragma once
#include "TlsChannel.h"
#include <QElapsedTimer>
#include <QJsonObject>
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
    Pong,
    Control
  };
  static constexpr int MaxData = 16384;
  FailoverLink(
      TlsChannel *channel, bool server, const QByteArray &instanceId, QObject *parent, int heartbeatMs = 1000,
      int timeoutMs = 6000
  );
  QJsonObject statistics() const;
  void start();
  void pauseRead(bool paused);
  void stop(const QString &reason);
  bool send(Message type, quint64 epoch = 0, const QByteArray &data = {});
  bool healthy() const
  {
    return m_healthy && ready();
  }
  bool ready() const
  {
    return m_ready && !m_stopped;
  }
  qint64 queued() const
  {
    return m_output.size() + m_channel->bytesToWrite();
  }
  QByteArray peerFingerprint() const
  {
    return m_channel->peerFingerprint();
  }
  QByteArray peerInstance() const
  {
    return m_peer;
  }
Q_SIGNALS:
  void established();
  void healthConfirmed();
  void statisticsChanged();
  void message(quint8 type, quint64 epoch, const QByteArray &data);
  void writable();
  void disconnected(const QString &reason);

private:
  void sampleRates();
  void receive();
  void flush();
  TlsChannel *m_channel;
  bool m_server, m_ready = false, m_stopped = false, m_healthy = false, m_readPaused = false;
  QByteArray m_id, m_peer, m_input, m_output;
  QTimer m_tick;
  QElapsedTimer m_clock;
  qint64 m_pingAt = 0;
  quint64 m_ping = 0, m_nextPing = 0;
  int m_timeout;
  quint64 m_txBytes = 0, m_rxBytes = 0, m_previousTx = 0, m_previousRx = 0;
  qint64 m_sampleNs = 0, m_sampleAtMs = -1, m_pingNs = 0, m_rttAtMs = -1;
  QString m_sampleTimestamp;
  double m_txRate = 0, m_rxRate = 0, m_rttMs = 0;
};
