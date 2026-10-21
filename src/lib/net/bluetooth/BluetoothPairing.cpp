// SPDX-License-Identifier: MIT
#include "BluetoothPairing.h"

BluetoothPairing::BluetoothPairing(QObject *parent) : QObject(parent)
{
  m_deadline.setSingleShot(true);
  m_deadline.setInterval(60000);
  connect(&m_deadline, &QTimer::timeout, this, [this] {
    cancel();
    Q_EMIT finished(false, "Bluetooth pairing timed out");
  });
#ifndef Q_OS_MACOS
  connect(&m_adapter, &QBluetoothLocalDevice::pairingFinished, this, [this](const auto &peer, auto status) {
    if (m_active && peer == m_peer)
      complete(status != QBluetoothLocalDevice::Unpaired, "Pairing was not accepted");
  });
  connect(&m_adapter, &QBluetoothLocalDevice::errorOccurred, this, [this] {
    complete(false, "System Bluetooth pairing failed");
  });
#endif
}

BluetoothPairing::~BluetoothPairing()
{
  cancel();
}

void BluetoothPairing::complete(bool success, const QString &error)
{
  if (!m_active)
    return;
  m_active = false;
  m_deadline.stop();
  Q_EMIT finished(success, success ? QString() : error);
}

#ifndef Q_OS_MACOS
void BluetoothPairing::pair(const QBluetoothAddress &peer)
{
  cancel();
  if (peer.isNull() || !m_adapter.isValid()) {
    Q_EMIT finished(false, "Invalid peer or no Bluetooth adapter");
    return;
  }
  m_peer = peer;
  m_active = true;
  m_deadline.start();
  // BlueZ's system agent handles passkeys and numeric confirmation on Linux.
  m_adapter.requestPairing(peer, QBluetoothLocalDevice::Paired);
}
void BluetoothPairing::confirm(bool)
{
}
void BluetoothPairing::providePin(const QString &)
{
}
void BluetoothPairing::cancel()
{
  m_active = false;
  m_deadline.stop();
  // Qt exposes no portable cancel API for the system agent; late results are ignored.
}
#endif
