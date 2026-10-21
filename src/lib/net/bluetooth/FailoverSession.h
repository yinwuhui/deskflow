// SPDX-License-Identifier: MIT
#pragma once
#include "FailoverLink.h"
#include <QPointer>
#include <QTcpSocket>
#include <QUuid>
#include <array>

// One controller, two authenticated lanes, at most one input session. No data replay.
// Server receives activations; client is the sole authority for monotonically increasing epochs.
class FailoverSession : public QObject
{
  Q_OBJECT
public:
  enum Lane
  {
    Network = 0,
    Bluetooth = 1
  };
  FailoverSession(bool server, QObject *parent = nullptr);
  QByteArray instanceId() const
  {
    return m_id;
  }
  void attach(Lane lane, FailoverLink *link);
  void acceptLocal(QTcpSocket *socket);
  void setCorePort(quint16 port)
  {
    m_corePort = port;
  }
  void coreConnected(bool connected);
  void stop();
  int selectedLane() const
  {
    return m_selected;
  }
Q_SIGNALS:
  void transportState(const QString &state);

private:
  bool available(int lane) const;
  void handle(int lane, quint8 type, quint64 epoch, const QByteArray &data);
  void attachCore(QTcpSocket *socket);
  void dropCore(bool tellPeer);
  void pump();
  void interrupted();
  bool m_server, m_stopping = false, m_dataReady = false, m_hadCore = false, m_recovering = false;
  QByteArray m_id = QUuid::createUuid().toRfc4122(), m_peerId;
  std::array<QPointer<FailoverLink>, 2> m_links;
  QPointer<QTcpSocket> m_core;
  quint16 m_corePort = 0;
  quint64 m_epoch = 0, m_lastEpoch = 0;
  int m_selected = -1, m_preferred = Network;
  bool m_coreReported = false;
  QTimer m_activationDeadline, m_offlineDeadline;
};
