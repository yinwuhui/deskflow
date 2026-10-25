// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "InputRouteAdapter.h"
#include <QLocalSocket>
namespace deskflow::display {
// Same-user local core IPC. No TCP endpoint, executable path or unauthenticated
// peer identifier can be supplied by the GUI through this transport.
class CoreInputChannel : public QObject {
  Q_OBJECT
public:
  explicit CoreInputChannel(InputRouteAdapter* adapter,QObject* parent=nullptr);
private:
  void send(const QJsonObject& command);
  void read();
  void failed();
  QLocalSocket m_socket;
  QByteArray m_buffer;
  QList<QByteArray> m_pending;
  bool m_authenticated=false;
  InputRouteAdapter* m_adapter;
};
}
