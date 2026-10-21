// SPDX-License-Identifier: MIT
#pragma once

#include <QElapsedTimer>
#include <QIODevice>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>

// Only the outer stream is RFCOMM. Data payloads preserve Deskflow's TLS byte stream.
// QIODevice permits tests to exercise the same session over a loopback stream.
class RfcommSession : public QObject
{
  Q_OBJECT
public:
  static constexpr quint32 MaxPayload = 16384;
  static constexpr qint64 MaxQueue = 1024 * 1024;
  enum Type : quint8
  {
    Hello = 1,
    Ready,
    Data,
    Ping,
    Pong
  };
  RfcommSession(QIODevice *wire, bool initiator, bool probe, quint16 corePort, QTcpSocket *local = nullptr);
  void start();
  void stop(const QString &reason);
  static QByteArray frame(Type type, const QByteArray &payload);
Q_SIGNALS:
  void finished();
  void closed(const QString &reason);
  void ready();
  void sample(double milliseconds);

private:
  bool send(Type type, const QByteArray &payload = {});
  void receive();
  void handle(Type type, const QByteArray &payload);
  void attachLocal(QTcpSocket *socket);
  void pumpLocal();
  void becomeReady();
  QIODevice *m_wire;
  QTcpSocket *m_local = nullptr;
  bool m_initiator, m_probe, m_ready = false, m_stopped = false, m_hello = false;
  quint16 m_corePort;
  QByteArray m_input, m_pendingPing;
  QTimer m_tick;
  QElapsedTimer m_clock;
  qint64 m_lastReceive = 0, m_pingTime = 0;
  quint64 m_sequence = 0, m_sent = 0, m_received = 0;
};
