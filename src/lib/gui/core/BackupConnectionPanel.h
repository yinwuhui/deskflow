// SPDX-License-Identifier: MIT
#pragma once
#include <QJsonObject>
#include <QWidget>
#include <functional>
class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QLineEdit;
class QBluetoothDeviceDiscoveryAgent;
class NearbyBluetoothModel;

// Presentation adapter for ConnectionManager snapshots. It never invents connection state.
class BackupConnectionPanel : public QWidget
{
  Q_OBJECT
public:
  explicit BackupConnectionPanel(QWidget *parent = nullptr);
  void updateSnapshot(const QJsonObject &snapshot);
  void setRunning(bool running);
  void operationFinished(const QJsonObject &result);
  QString selectedPeer() const;
  bool bluetoothSelected() const;
  bool bluetoothTargetReady() const;
  QWidget *transportSelector() const
  {
    return m_transport;
  }
  QWidget *bluetoothSelector() const;
  QWidget *detailsWidget() const
  {
    return m_details;
  }
  QWidget *sharingSelector() const
  {
    return m_sharing;
  }
  QWidget *statusWidget() const
  {
    return m_statusRow;
  }
  void setServerMode(bool server);
  void discover();
  void showDetails();
Q_SIGNALS:
  void command(const QJsonObject &command);
  void transportChanged();
  void statusVisibilityChanged(bool visible);
  void snapshotChanged(const QJsonObject &snapshot);

private:
  void showPeer();
  void performAction();
  void submitAction(bool accepted);
  void openBluetoothSettings();
  void finishDiscovery(quint64 generation, const QString &error = {});
  void applyDiscoveryUpdate(const std::function<void()> &update);
  void updateBluetoothChoice();
  QJsonObject m_snapshot, m_peer, m_pendingAction;
  QWidget *m_transport, *m_details, *m_actionForm, *m_sharing, *m_statusRow;
  QCheckBox *m_shareNetwork, *m_shareBluetooth;
  QPushButton *m_network, *m_bluetoothButton;
  QComboBox *m_peers, *m_bluetooth, *m_actionTargets;
  QCheckBox *m_automatic;
  QLineEdit *m_actionInput;
  QLabel *m_state, *m_detailState, *m_actionLabel;
  QPushButton *m_action, *m_retry, *m_later, *m_scan, *m_confirm, *m_reject, *m_protect;
  QPushButton *m_disconnect;
  NearbyBluetoothModel *m_devicesModel;
  QBluetoothDeviceDiscoveryAgent *m_deviceDiscovery = nullptr;
  qint64 m_lastScanMs = 0;
  quint64 m_scanGeneration = 0;
  bool m_updating = false, m_running = false, m_scanRequest = false;
};
