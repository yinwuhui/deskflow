// SPDX-License-Identifier: MIT
#pragma once
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothPermission>
#include <QBluetoothServiceDiscoveryAgent>
#include <QObject>
#include <QTimer>
#include <functional>

// All callbacks and operations run on the Qt application thread.
void requestBluetoothPermission(QObject *context, bool advertise, std::function<void(bool)> callback);

class BluetoothDiscovery : public QObject
{
  Q_OBJECT
public:
  explicit BluetoothDiscovery(QObject *parent = nullptr);
  void scan(int milliseconds = 20000);
  void discoverServices(const QBluetoothAddress &peer, int milliseconds = 20000);
  void stop();
Q_SIGNALS:
  void deviceFound(const QString &address, const QString &name);
  void serviceFound(const QString &address, const QString &name, const QString &uuid, int channel);
  void finished();
  void error(const QString &message);

private:
  QBluetoothDeviceDiscoveryAgent *m_devices = nullptr;
  QBluetoothServiceDiscoveryAgent *m_services = nullptr;
  QTimer m_deadline;
  bool m_active = false;
  void complete();
};
