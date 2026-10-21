// SPDX-License-Identifier: MIT
#include "BluetoothDiscovery.h"
#include "RfcommBridge.h"
#include <QCoreApplication>

void requestBluetoothPermission(QObject *context, bool advertise, std::function<void(bool)> callback)
{
  QBluetoothPermission permission;
  permission.setCommunicationModes(advertise ? QBluetoothPermission::Default : QBluetoothPermission::Access);
  auto *app = QCoreApplication::instance();
  if (app->checkPermission(permission) == Qt::PermissionStatus::Granted)
    QTimer::singleShot(0, context, [callback] { callback(true); });
  else
    app->requestPermission(permission, context, [callback](const QPermission &result) {
      callback(result.status() == Qt::PermissionStatus::Granted);
    });
}

BluetoothDiscovery::BluetoothDiscovery(QObject *parent) : QObject(parent)
{
  m_deadline.setSingleShot(true);
  connect(&m_deadline, &QTimer::timeout, this, &BluetoothDiscovery::stop);
}

void BluetoothDiscovery::scan(int milliseconds)
{
  if (m_active) {
    Q_EMIT error("Discovery is already active");
    return;
  }
  // Fresh agents prevent a late cancellation from completing a later operation.
  m_devices = new QBluetoothDeviceDiscoveryAgent(this);
  m_active = true;
  connect(m_devices, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, this, [this](const auto &device) {
    Q_EMIT deviceFound(device.address().toString(), device.name());
  });
  connect(m_devices, &QBluetoothDeviceDiscoveryAgent::finished, this, &BluetoothDiscovery::complete);
  connect(m_devices, &QBluetoothDeviceDiscoveryAgent::canceled, this, &BluetoothDiscovery::complete);
  connect(m_devices, &QBluetoothDeviceDiscoveryAgent::errorOccurred, this, [this] {
    const auto errorText = m_devices->errorString();
    Q_EMIT error(errorText);
    complete();
  });
  m_deadline.start(qBound(1000, milliseconds, 60000));
  m_devices->start(QBluetoothDeviceDiscoveryAgent::ClassicMethod);
}

void BluetoothDiscovery::discoverServices(const QBluetoothAddress &peer, int milliseconds)
{
  if (m_active) {
    Q_EMIT error("Discovery is already active");
    return;
  }
  if (peer.isNull()) {
    Q_EMIT error("Invalid Bluetooth peer address");
    Q_EMIT finished();
    return;
  }
  m_services = new QBluetoothServiceDiscoveryAgent(this);
  m_active = true;
  connect(m_services, &QBluetoothServiceDiscoveryAgent::serviceDiscovered, this, [this](const auto &service) {
    Q_EMIT serviceFound(
        service.device().address().toString(), service.serviceName(), service.serviceUuid().toString(),
        service.serverChannel()
    );
  });
  connect(m_services, &QBluetoothServiceDiscoveryAgent::finished, this, &BluetoothDiscovery::complete);
  connect(m_services, &QBluetoothServiceDiscoveryAgent::canceled, this, &BluetoothDiscovery::complete);
  connect(m_services, &QBluetoothServiceDiscoveryAgent::errorOccurred, this, [this] {
    const auto errorText = m_services->errorString();
    Q_EMIT error(errorText);
    complete();
  });
  m_services->setRemoteAddress(peer);
  m_services->setUuidFilter(ServiceUuid);
  m_deadline.start(qBound(1000, milliseconds, 60000));
  m_services->start(QBluetoothServiceDiscoveryAgent::FullDiscovery);
}

void BluetoothDiscovery::stop()
{
  complete();
}

void BluetoothDiscovery::complete()
{
  if (!m_active)
    return;
  m_active = false;
  m_deadline.stop();
  if (m_devices) {
    m_devices->disconnect(this);
    m_devices->stop();
    m_devices->deleteLater();
    m_devices = nullptr;
  }
  if (m_services) {
    m_services->disconnect(this);
    m_services->stop();
    m_services->deleteLater();
    m_services = nullptr;
  }
  Q_EMIT finished();
}
