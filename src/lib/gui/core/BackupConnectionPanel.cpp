// SPDX-License-Identifier: MIT
#include "BackupConnectionPanel.h"
#include "NearbyBluetoothModel.h"
#include "common/Settings.h"
#include "gui/widgets/DiscoveryComboBox.h"
#include "gui/widgets/HomeIcons.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#ifdef DESKFLOW_BLUETOOTH
#include "BluetoothDiscovery.h"
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#endif
using namespace deskflow::gui;
namespace {
QLabel *plainLabel(const QString &text, QWidget *parent, const QString &name = {})
{
  auto *label = new QLabel(text, parent);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setObjectName(name);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  return label;
}
} // namespace
BackupConnectionPanel::BackupConnectionPanel(QWidget *parent) : QWidget(parent)
{
  setObjectName("backupConnectionPanel");
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 6, 0, 0);
  layout->setSpacing(4);
  m_transport = new QWidget(this);
  m_transport->setObjectName("initialTransportSelector");
  auto *transportLayout = new QHBoxLayout(m_transport);
  transportLayout->setContentsMargins(3, 3, 3, 3);
  transportLayout->setSpacing(3);
  auto *group = new QButtonGroup(this);
  m_network = new QPushButton(tr("LAN"), m_transport);
  m_bluetoothButton = new QPushButton(tr("Bluetooth"), m_transport);
  m_network->setObjectName("btnInitialNetwork");
  m_bluetoothButton->setObjectName("btnInitialBluetooth");
  for (auto *button : {m_network, m_bluetoothButton}) {
    button->setCheckable(true);
    button->setAutoDefault(false);
    button->setFixedHeight(30);
    button->setStyleSheet(
        "QPushButton { padding: 2px 8px; border: none; } QPushButton:checked { background: #e7f0fc; color: #087fff; }"
    );
    group->addButton(button);
    transportLayout->addWidget(button);
  }
  m_network->setIcon(homeIcon(HomeIcon::Wifi));
  m_bluetoothButton->setIcon(homeIcon(HomeIcon::Bluetooth));
  m_transport->setStyleSheet(
      "QWidget#initialTransportSelector { background: #ffffff; border: 1px solid #e3e8ef; border-radius: 7px; }"
  );
  (bluetoothSelected() ? m_bluetoothButton : m_network)->setChecked(true);
#ifndef DESKFLOW_BLUETOOTH
  m_bluetoothButton->setEnabled(false);
  m_bluetoothButton->setToolTip(tr("This build does not include Bluetooth support."));
#endif
  connect(m_network, &QPushButton::toggled, this, [this](bool checked) {
    if (!checked)
      return;
    Settings::setValue(Settings::Connection::InitialTransport, "network");
    Q_EMIT transportChanged();
  });
  connect(m_bluetoothButton, &QPushButton::toggled, this, [this](bool checked) {
    if (!checked)
      return;
    Settings::setValue(Settings::Connection::InitialTransport, "bluetooth");
    Q_EMIT transportChanged();
  });
  auto *devicePicker = new DiscoveryComboBox(this);
  m_bluetooth = devicePicker;
  m_bluetooth->setObjectName("bluetoothTarget");
  m_bluetooth->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  m_bluetooth->setFixedHeight(44);
  m_bluetooth->setAccessibleName(tr("Nearby Bluetooth devices"));
  m_bluetooth->setStyleSheet(
      "QComboBox { background: #ffffff; color: #555e69; border: 1px solid #d9dde3; border-radius: 9px;"
      " padding: 0 13px; } QComboBox:focus { border-color: #087fff; }"
      "QComboBox::drop-down { border: none; width: 28px; }"
      "QComboBox::down-arrow { image: none; }"
  );
  m_devicesModel = new NearbyBluetoothModel(this);
  const auto saved = Settings::value(Settings::Connection::BluetoothPeer).toString();
  m_devicesModel->resetDevices(saved);
  devicePicker->setModel(m_devicesModel);
  m_bluetooth->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  m_bluetooth->setMinimumContentsLength(1);
  m_bluetooth->setMaxVisibleItems(8);
  m_bluetooth->view()->setTextElideMode(Qt::ElideRight);
  if (!saved.isEmpty())
    m_bluetooth->setCurrentIndex(1);
  devicePicker->refreshRequested = [this] {
    if (!m_running && !m_scanRequest && (m_lastScanMs == 0 || m_devicesModel->foundCount() == 0) &&
        QDateTime::currentMSecsSinceEpoch() - m_lastScanMs > 15000)
      discover();
  };
  connect(m_bluetooth, &QComboBox::currentIndexChanged, this, [this] {
    if (!m_updating)
      updateBluetoothChoice();
  });
  m_sharing = new QWidget(this);
  m_sharing->setObjectName("sharingTransportSelector");
  auto *sharingLayout = new QHBoxLayout(m_sharing);
  sharingLayout->setContentsMargins(0, 0, 0, 0);
  sharingLayout->setSpacing(14);
  auto *sharingLabel = new QLabel(tr("Connection method"), m_sharing);
  sharingLabel->setProperty("role", "muted");
  sharingLayout->addWidget(sharingLabel);
  m_shareNetwork = new QCheckBox(tr("LAN"), m_sharing);
  m_shareBluetooth = new QCheckBox(tr("Bluetooth"), m_sharing);
  m_shareNetwork->setObjectName("checkShareNetwork");
  m_shareBluetooth->setObjectName("checkShareBluetooth");
  m_shareNetwork->setStyleSheet("font-size: 12px;");
  m_shareBluetooth->setStyleSheet("font-size: 12px;");
  m_shareNetwork->setChecked(Settings::value(Settings::Connection::ShareNetwork).toBool());
  m_shareBluetooth->setChecked(Settings::value(Settings::Connection::ShareBluetooth).toBool());
#ifndef DESKFLOW_BLUETOOTH
  m_shareBluetooth->setChecked(false);
  Settings::setValue(Settings::Connection::ShareBluetooth, false);
  m_shareBluetooth->setEnabled(false);
  m_shareBluetooth->setToolTip(tr("This build does not include Bluetooth support."));
#endif
  if (!m_shareNetwork->isChecked() && !m_shareBluetooth->isChecked()) {
    m_shareNetwork->setChecked(true);
    Settings::setValue(Settings::Connection::ShareNetwork, true);
  }
  sharingLayout->addWidget(m_shareNetwork);
  sharingLayout->addWidget(m_shareBluetooth);
  auto *shareInfo = new QPushButton(QStringLiteral("ⓘ"), m_sharing);
  shareInfo->setAccessibleName(tr("Connection method help"));
  shareInfo->setToolTip(tr("Enable both methods to allow automatic recovery over the other transport."));
  shareInfo->setFixedSize(24, 26);
  shareInfo->setProperty("role", "link");
  sharingLayout->addWidget(shareInfo);
  sharingLayout->addStretch();
  for (auto *check : {m_shareNetwork, m_shareBluetooth})
    connect(check, &QCheckBox::toggled, this, [this, check] {
      if (!m_shareNetwork->isChecked() && !m_shareBluetooth->isChecked()) {
        const QSignalBlocker blocker(check);
        check->setChecked(true);
      }
      Settings::setValue(Settings::Connection::ShareNetwork, m_shareNetwork->isChecked());
      Settings::setValue(Settings::Connection::ShareBluetooth, m_shareBluetooth->isChecked());
      Q_EMIT transportChanged();
    });
  auto *policyRow = new QHBoxLayout;
  layout->setContentsMargins(0, 0, 0, 0);
  m_automatic = new QCheckBox(tr("Automatically establish a backup connection"), this);
  m_automatic->setObjectName("checkAutoBackup");
  m_automatic->setChecked(Settings::value(Settings::Connection::AutoBackup).toBool());
  m_automatic->setStyleSheet("font-size: 12px;");
  policyRow->setSpacing(4);
  policyRow->addWidget(m_automatic);
  auto *backupInfo = new QPushButton(QStringLiteral("ⓘ"), this);
  backupInfo->setAccessibleName(tr("Backup connection help"));
  backupInfo->setToolTip(tr("After connecting, automatically prepare the other transport."));
  backupInfo->setFixedSize(24, 26);
  backupInfo->setProperty("role", "link");
  policyRow->addWidget(backupInfo);
  policyRow->addStretch();
  layout->addLayout(policyRow);
  setFixedHeight(28);
  m_statusRow = new QWidget(this);
  m_statusRow->setObjectName("backupStatusRow");
  auto *statusRow = new QHBoxLayout(m_statusRow);
  statusRow->setContentsMargins(0, 0, 0, 0);
  m_state = plainLabel({}, m_statusRow, "lblBackupStatus");
  m_state->setProperty("role", "muted");
  statusRow->addWidget(m_state, 1);
  m_action = new QPushButton(tr("Connection details"), m_statusRow);
  m_action->setObjectName("btnBackupDetails");
  m_action->setProperty("role", "link");
  statusRow->addWidget(m_action);
  m_statusRow->hide();

  m_details = new QWidget(this);
  m_details->setObjectName("backupConnectionDetails");
  m_details->setStyleSheet(
      "QWidget#backupConnectionDetails { background: #f7f8fa; border: 1px solid #e3e8ef; border-radius: 10px; }"
  );
  auto *details = new QVBoxLayout(m_details);
  details->setContentsMargins(14, 10, 14, 10);
  auto *head = new QHBoxLayout;
  head->addWidget(plainLabel(tr("Connection details"), m_details));
  head->addStretch();
  auto *close = new QPushButton(tr("Close"), m_details);
  close->setObjectName("btnCloseBackupDetails");
  head->addWidget(close);
  details->addLayout(head);
  connect(close, &QPushButton::clicked, m_details, &QWidget::hide);
  auto *peerRow = new QHBoxLayout;
  m_peers = new QComboBox(m_details);
  m_peers->setObjectName("backupPeerSelector");
  m_peers->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  m_peers->setAccessibleName(tr("Computer protected by backup"));
  peerRow->addWidget(m_peers, 1);
  m_protect = new QPushButton(tr("Use backup for this computer"), m_details);
  peerRow->addWidget(m_protect);
  details->addLayout(peerRow);
  m_detailState = plainLabel({}, m_details, "lblBackupDetailStatus");
  details->addWidget(m_detailState);
  m_actionForm = new QWidget(m_details);
  auto *form = new QVBoxLayout(m_actionForm);
  form->setContentsMargins(0, 0, 0, 0);
  m_actionLabel = plainLabel({}, m_actionForm, "lblBackupUserAction");
  m_actionInput = new QLineEdit(m_actionForm);
  m_actionInput->setObjectName("backupActionInput");
  form->addWidget(m_actionLabel);
  m_actionTargets = new QComboBox(m_actionForm);
  m_actionTargets->setObjectName("backupActionTarget");
  m_actionTargets->setAccessibleName(tr("Bluetooth computer"));
  m_actionTargets->addItem(tr("Choose a nearby computer"), QString());
  form->addWidget(m_actionTargets);
  form->addWidget(m_actionInput);
  auto *actionRow = new QHBoxLayout;
  m_confirm = new QPushButton(tr("Confirm"), m_actionForm);
  m_confirm->setObjectName("btnConfirmBackupAction");
  m_reject = new QPushButton(tr("Reject"), m_actionForm);
  m_later = new QPushButton(tr("Later"), m_actionForm);
  actionRow->addWidget(m_confirm);
  actionRow->addWidget(m_reject);
  actionRow->addWidget(m_later);
  actionRow->addStretch();
  form->addLayout(actionRow);
  details->addWidget(m_actionForm);
  auto *foot = new QVBoxLayout;
  auto *secondaryActions = new QHBoxLayout;
  m_retry = new QPushButton(tr("Retry backup"), m_details);
  m_scan = new QPushButton(tr("Refresh Bluetooth devices"), m_details);
  auto *disconnect = new QPushButton(tr("Disconnect selected computer"), m_details);
  m_disconnect = disconnect;
  secondaryActions->addWidget(m_retry);
  secondaryActions->addWidget(m_scan);
  secondaryActions->addStretch();
  foot->addLayout(secondaryActions);
  foot->addWidget(disconnect, 0, Qt::AlignRight);
  details->addLayout(foot);
  m_details->hide();
  connect(m_automatic, &QCheckBox::toggled, this, [this](bool enabled) {
    if (m_updating)
      return;
    if (selectedPeer().isEmpty()) {
      Settings::setValue(Settings::Connection::AutoBackup, enabled);
      Q_EMIT command({{"kind", "defaultPolicy"}, {"enabled", enabled}});
    } else
      Q_EMIT command({{"kind", "policy"}, {"peerId", selectedPeer()}, {"enabled", enabled}, {"select", true}});
  });
  connect(m_peers, &QComboBox::currentIndexChanged, this, [this] {
    if (!m_updating)
      showPeer();
  });
  connect(m_action, &QPushButton::clicked, this, [this] {
    if (!m_peer.value("actionId").toString().isEmpty())
      performAction();
    else
      showDetails();
  });
  connect(m_confirm, &QPushButton::clicked, this, [this] { submitAction(true); });
  connect(m_reject, &QPushButton::clicked, this, [this] { submitAction(false); });
  connect(m_later, &QPushButton::clicked, this, [this] {
    if (!m_pendingAction.isEmpty())
      Q_EMIT command(
          {{"kind", "action"},
           {"peerId", m_pendingAction.value("peerId")},
           {"actionId", m_pendingAction.value("actionId")},
           {"cancel", true}}
      );
    m_pendingAction = {};
    m_details->hide();
  });
  connect(m_scan, &QPushButton::clicked, this, &BackupConnectionPanel::discover);
  connect(m_retry, &QPushButton::clicked, this, [this] {
    if (selectedPeer().isEmpty()) {
      if (bluetoothSelected() || Settings::value(Settings::Core::CoreMode) == Settings::CoreMode::Server)
        Q_EMIT command({{"kind", "retryBluetooth"}});
      return;
    }
    Q_EMIT command(
        {{"kind", m_peer.value("state") == "stopped" ? "reconnectPeer" : "retryBackup"}, {"peerId", selectedPeer()}}
    );
  });
  connect(m_protect, &QPushButton::clicked, this, [this] {
    if (!selectedPeer().isEmpty() && selectedPeer() != "bootstrap")
      Q_EMIT command({{"kind", "policy"}, {"peerId", selectedPeer()}, {"enabled", true}, {"select", true}});
  });
  connect(disconnect, &QPushButton::clicked, this, [this] {
    if (!selectedPeer().isEmpty() && selectedPeer() != "bootstrap")
      Q_EMIT command({{"kind", "stopPeer"}, {"peerId", selectedPeer()}});
  });
  showPeer();
}
void BackupConnectionPanel::setServerMode(bool server)
{
  m_sharing->setVisible(server);
  m_transport->setVisible(!server);
  setVisible(!server);
  if (!m_running && (server || !bluetoothSelected()) && m_scanRequest) {
    finishDiscovery(m_scanGeneration);
    m_statusRow->hide();
    Q_EMIT statusVisibilityChanged(false);
  }
}
QString BackupConnectionPanel::selectedPeer() const
{
  return m_peers->currentData().toString();
}
bool BackupConnectionPanel::bluetoothSelected() const
{
  return Settings::value(Settings::Connection::InitialTransport).toString() == "bluetooth";
}
QWidget *BackupConnectionPanel::bluetoothSelector() const
{
  return m_bluetooth;
}
void BackupConnectionPanel::setRunning(bool running)
{
  const bool changed = m_running != running;
  m_running = running;
  m_transport->setEnabled(!running);
  m_sharing->setEnabled(!running);
#ifndef DESKFLOW_BLUETOOTH
  m_shareBluetooth->setEnabled(false);
#endif
  m_statusRow->setVisible(running);
  Q_EMIT statusVisibilityChanged(running);
  m_bluetooth->setEnabled(!running);
  m_scan->setEnabled(!running && !m_scanRequest);
  if ((running || changed) && m_scanRequest)
    finishDiscovery(m_scanGeneration);
  if (!running) {
    m_snapshot = {};
    m_peer = {};
    m_pendingAction = {};
    m_updating = true;
    m_peers->clear();
    m_updating = false;
    m_details->hide();
    showPeer();
  } else if (changed) {
    showPeer();
    Q_EMIT command({{"kind", "snapshot"}});
  }
}
void BackupConnectionPanel::updateSnapshot(const QJsonObject &snapshot)
{
  if (!m_running)
    return;
  if (!m_snapshot.isEmpty() && snapshot.value("revision").toDouble() < m_snapshot.value("revision").toDouble())
    return;
  const auto selected = selectedPeer();
  m_snapshot = snapshot;
  m_updating = true;
  m_peers->clear();
  for (const auto &value : snapshot.value("peers").toArray()) {
    const auto peer = value.toObject();
    const auto name = peer.value("name").toString();
    m_peers->addItem(name.isEmpty() ? tr("Connected computer") : name, peer.value("peerId"));
  }
  int index = m_peers->findData(selected);
  if (index < 0)
    index = m_peers->findData(snapshot.value("backupPeer").toString());
  if (index >= 0)
    m_peers->setCurrentIndex(index);
  m_updating = false;
  showPeer();
  Q_EMIT snapshotChanged(snapshot);
}
void BackupConnectionPanel::showPeer()
{
  m_peer = {};
  for (const auto &value : m_snapshot.value("peers").toArray())
    if (value.toObject().value("peerId").toString() == selectedPeer())
      m_peer = value.toObject();
  m_updating = true;
  m_automatic->setChecked(
      m_peer.isEmpty() ? Settings::value(Settings::Connection::AutoBackup).toBool() : m_peer.value("requested").toBool()
  );
  m_updating = false;
  m_automatic->setEnabled(selectedPeer() != "bootstrap");
  QString status;
  QString active;
  for (const auto &lane : m_peer.value("channels").toArray())
    if (lane.toObject().value("state") == "active")
      active =
          lane.toObject().value("type").toString() == "network" ? tr("Using local network") : tr("Using Bluetooth");
  const auto state = m_peer.value("state").toString();
  const auto backup = m_peer.value("backup").toString();
  if (m_peer.isEmpty())
    status = !m_running ? tr("The backup transport will be prepared after connecting.")
             : Settings::value(Settings::Core::CoreMode) == Settings::CoreMode::Server
                 ? tr("Sharing started; waiting for a computer to connect.")
                 : tr("Connecting to the other computer…");
  else if (state == "stopped")
    status = tr("Disconnected. Automatic reconnection for this computer is stopped.");
  else if (state == "switching" || state == "recovering" || state == "offline")
    status = tr("Connection interrupted; restoring control. Your input remains local.");
  else {
    if (active.isEmpty())
      active = tr("Connecting to the other computer…");
    if (backup == "ready")
      status = tr("Backup ready");
    else if (backup == "disabled")
      status = tr("Backup disabled");
    else if (backup == "blockedByPeer")
      status = tr("Backup is disabled by the other computer");
    else if (backup == "unavailable")
      status = tr("Backup unavailable; current connection remains usable");
    else if (backup == "needsSelection")
      status = tr("Select a computer to protect with a backup");
    else if (backup == "needsAction")
      status = tr("Complete Bluetooth setup");
    else
      status = tr("Preparing backup");
    status = active + QStringLiteral(" · ") + status;
  }
  if (m_automatic->isChecked() && (!Settings::value(Settings::Security::TlsEnabled).toBool() ||
                                   !Settings::value(Settings::Security::CheckPeers).toBool()))
    status = tr("Enable encryption and certificate verification to prepare backup connections.");
  if (m_peer.isEmpty() && !m_snapshot.value("bluetoothReason").toString().isEmpty())
    status = tr("Bluetooth unavailable; check permissions and adapter settings.");
  m_state->setText(status);
  m_state->setToolTip(m_peer.value("reason").toString() + " " + m_snapshot.value("bluetoothReason").toString());
  m_detailState->setText(
      status + (m_peer.value("effective").toBool()
                    ? tr("\nThis computer's clipboard cap is up to 64 KiB; lower limits are retained.")
                    : QString())
  );
  m_action->setText(m_peer.value("actionId").toString().isEmpty() ? tr("Connection details") : tr("Complete setup"));
  m_action->setEnabled(true);
  m_retry->setText(state == "stopped" ? tr("Reconnect") : tr("Retry backup"));
  m_retry->setEnabled(!m_peer.isEmpty() || (m_running && !m_snapshot.value("bluetoothReason").toString().isEmpty()));
  m_protect->setEnabled(!selectedPeer().isEmpty() && selectedPeer() != "bootstrap");
  m_protect->setVisible(backup == "needsSelection");
  m_disconnect->setEnabled(!selectedPeer().isEmpty() && selectedPeer() != "bootstrap");
  m_peers->setVisible(m_peers->count() > 1);
  if (!m_pendingAction.isEmpty() && (m_pendingAction.value("actionId") != m_peer.value("actionId") ||
                                     m_pendingAction.value("peerId").toString() != selectedPeer())) {
    m_pendingAction = {};
    m_actionInput->clear();
  }
  m_actionForm->setVisible(!m_pendingAction.isEmpty());
  m_scan->setEnabled(!m_scanRequest && (!m_running || m_peer.value("action") == "selectBluetoothDevice"));
}
void BackupConnectionPanel::showDetails()
{
  m_details->show();
}
void BackupConnectionPanel::operationFinished(const QJsonObject &result)
{
  if (result.value("accepted").toBool() || result.value("peerId").toString() != selectedPeer())
    return;
  showPeer();
  m_state->setText(
      result.value("error") == "identityUntrusted"
          ? tr("Review this computer's certificate trust before reconnecting.")
          : tr("This action is no longer available. Refresh the connection and try again.")
  );
}
void BackupConnectionPanel::openBluetoothSettings()
{
#ifdef Q_OS_MACOS
  QDesktopServices::openUrl(QUrl("x-apple.systempreferences:com.apple.preference.security?Privacy_Bluetooth"));
#else
  m_detailState->setText(tr("Open your system Bluetooth settings and complete the requested action."));
#endif
}
void BackupConnectionPanel::performAction()
{
  const auto action = m_peer.value("action").toString();
  showDetails();
  if (action == "bluetoothSettings") {
    openBluetoothSettings();
    return;
  }
  if (action == "displayPairingCode") {
    m_detailState->setText(tr("Enter this code on the other computer: %1").arg(m_peer.value("reason").toString()));
    return;
  }
  m_pendingAction = {{"peerId", selectedPeer()}, {"actionId", m_peer.value("actionId")}, {"action", action}};
  m_actionInput->clear();
  m_actionInput->setEchoMode(action == "pairingPin" ? QLineEdit::Password : QLineEdit::Normal);
  m_actionInput->setVisible(action == "pairingPin" || action == "selectBluetoothDevice");
  m_actionTargets->setVisible(action == "selectBluetoothDevice");
  if (action == "selectBluetoothDevice") {
    m_actionTargets->clear();
    m_actionTargets->addItem(tr("Choose a nearby computer"), QString());
    for (int i = 1; i < m_bluetooth->count(); ++i)
      if (!m_devicesModel->connectableAddress(i).isEmpty())
        m_actionTargets->addItem(m_bluetooth->itemText(i), m_devicesModel->connectableAddress(i));
  }
  m_actionInput->setAccessibleName(action == "pairingPin" ? tr("Pairing PIN") : tr("Bluetooth address"));
  m_actionLabel->setText(
      action == "confirmPairing" ? tr("Confirm the code on both computers: %1").arg(m_peer.value("reason").toString())
      : action == "pairingPin"   ? tr("Enter the pairing PIN")
      : action == "selectBluetoothDevice" ? tr("Select the other computer, or enter its Bluetooth address.")
                                          : tr("Continue the Bluetooth setup requested by the connection service.")
  );
  m_reject->setVisible(action == "confirmPairing");
  m_actionForm->show();
}
void BackupConnectionPanel::submitAction(bool accepted)
{
  if (m_pendingAction.isEmpty() || m_pendingAction.value("actionId") != m_peer.value("actionId") ||
      m_pendingAction.value("peerId").toString() != selectedPeer())
    return;
  QJsonObject request{{"kind", "action"}, {"peerId", selectedPeer()}, {"actionId", m_pendingAction.value("actionId")}};
  const auto action = m_pendingAction.value("action").toString();
  if (action == "confirmPairing")
    request.insert("accepted", accepted);
  if (action == "pairingPin") {
    if (m_actionInput->text().isEmpty())
      return;
    request.insert("pin", m_actionInput->text());
  }
  if (action == "selectBluetoothDevice") {
    const auto address =
        m_actionInput->text().isEmpty() ? m_actionTargets->currentData().toString() : m_actionInput->text().trimmed();
    if (address.isEmpty())
      return;
    request.insert("address", address);
  }
  m_pendingAction = {};
  m_actionInput->clear();
  m_actionForm->hide();
  Q_EMIT command(request);
}
bool BackupConnectionPanel::bluetoothTargetReady() const
{
  return !m_devicesModel->connectableAddress(m_bluetooth->currentIndex()).isEmpty();
}
void BackupConnectionPanel::updateBluetoothChoice()
{
  const auto address = m_devicesModel->connectableAddress(m_bluetooth->currentIndex());
  if (!m_running)
    Settings::setValue(Settings::Connection::BluetoothPeer, address);
  m_bluetooth->setToolTip(m_bluetooth->currentData(Qt::ToolTipRole).toString());
  Q_EMIT transportChanged();
}
void BackupConnectionPanel::applyDiscoveryUpdate(const std::function<void()> &update)
{
  const auto identity = m_bluetooth->currentData(NearbyBluetoothModel::Identity).toString();
  {
    const QSignalBlocker blocker(m_bluetooth);
    update();
    m_devicesModel->sort(0);
    const int row = m_devicesModel->rowForIdentity(identity);
    m_bluetooth->setCurrentIndex(row < 0 ? 0 : row);
  }
  const auto selectedTarget = m_actionTargets->currentData().toString();
  m_actionTargets->clear();
  m_actionTargets->addItem(tr("Choose a nearby computer"), QString());
  for (int row = 1; row < m_devicesModel->rowCount(); ++row) {
    const auto address = m_devicesModel->connectableAddress(row);
    if (!address.isEmpty())
      m_actionTargets->addItem(m_devicesModel->item(row)->data(NearbyBluetoothModel::Name).toString(), address);
  }
  const auto targetIndex = m_actionTargets->findData(selectedTarget);
  m_actionTargets->setCurrentIndex(targetIndex < 0 ? 0 : targetIndex);
  updateBluetoothChoice();
}
void BackupConnectionPanel::finishDiscovery(quint64 generation, const QString &error)
{
  if (generation != m_scanGeneration || !m_scanRequest)
    return;
  qInfo() << "Bluetooth device discovery finished after" << QDateTime::currentMSecsSinceEpoch() - m_lastScanMs
          << "ms; devices:" << m_devicesModel->foundCount() << "; error:" << error;
  m_scanRequest = false;
#ifdef DESKFLOW_BLUETOOTH
  if (m_deviceDiscovery) {
    m_deviceDiscovery->disconnect(this);
    m_deviceDiscovery->stop();
    m_deviceDiscovery->deleteLater();
    m_deviceDiscovery = nullptr;
  }
#endif
  applyDiscoveryUpdate([this] { m_devicesModel->finishScan(); });
  m_scan->setEnabled(!m_running || m_peer.value("action") == "selectBluetoothDevice");
  m_statusRow->show();
  Q_EMIT statusVisibilityChanged(true);
  if (!error.isEmpty())
    m_state->setText(error);
  else if (!m_devicesModel->foundCount())
    m_state->setText(tr("No nearby devices found. Make the other device discoverable and refresh."));
  else
    m_state->setText(tr("Found %1 devices").arg(m_devicesModel->foundCount()));
}

void BackupConnectionPanel::discover()
{
#ifdef DESKFLOW_BLUETOOTH
  if (m_scanRequest || (m_running && m_peer.value("action") != "selectBluetoothDevice"))
    return;
  const auto generation = ++m_scanGeneration;
  m_scanRequest = true;
  m_lastScanMs = QDateTime::currentMSecsSinceEpoch();
  m_actionTargets->clear();
  m_actionTargets->addItem(tr("Choose a nearby computer"), QString());
  m_scan->setEnabled(false);
  m_statusRow->show();
  Q_EMIT statusVisibilityChanged(true);
  m_state->setText(tr("Finding nearby Bluetooth devices…"));
  applyDiscoveryUpdate([this] { m_devicesModel->beginScan(); });
  requestBluetoothPermission(this, false, [this, generation](bool granted) {
    if (generation != m_scanGeneration || !m_scanRequest)
      return;
    if (!granted) {
      finishDiscovery(generation, tr("Bluetooth permission is required for discovery."));
      return;
    }
    m_deviceDiscovery = new QBluetoothDeviceDiscoveryAgent(this);
    // Classic inquiry can run before LE discovery on macOS. Allow both phases
    // to complete instead of truncating the entire operation after 12 seconds.
    m_deviceDiscovery->setLowEnergyDiscoveryTimeout(40000);
    const auto record = [this, generation](const QBluetoothDeviceInfo &device) {
      if (generation != m_scanGeneration || !m_scanRequest || !m_deviceDiscovery)
        return;
      const auto address = device.address().isNull() ? QString() : device.address().toString();
      const auto identity = address.isEmpty() ? device.deviceUuid().toString() : address;
      if (address.isEmpty() && device.deviceUuid().isNull())
        return;
      const bool classic = device.coreConfigurations() != QBluetoothDeviceInfo::LowEnergyCoreConfiguration;
      applyDiscoveryUpdate([&] { m_devicesModel->addDevice(identity, address, device.name(), classic); });
    };
    connect(m_deviceDiscovery, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, this, record);
    connect(
        m_deviceDiscovery, &QBluetoothDeviceDiscoveryAgent::deviceUpdated, this,
        [record](const QBluetoothDeviceInfo &device, QBluetoothDeviceInfo::Fields) { record(device); }
    );
    const auto devicesFinished = [this, generation] {
      if (generation != m_scanGeneration || !m_scanRequest || !m_deviceDiscovery)
        return;
      m_deviceDiscovery->disconnect(this);
      m_deviceDiscovery->stop();
      m_deviceDiscovery->deleteLater();
      m_deviceDiscovery = nullptr;
      finishDiscovery(generation);
    };
    connect(m_deviceDiscovery, &QBluetoothDeviceDiscoveryAgent::finished, this, devicesFinished);
    connect(m_deviceDiscovery, &QBluetoothDeviceDiscoveryAgent::canceled, this, devicesFinished);
    connect(m_deviceDiscovery, &QBluetoothDeviceDiscoveryAgent::errorOccurred, this, [this, generation] {
      qWarning() << "Bluetooth device discovery error:" << m_deviceDiscovery->error()
                 << m_deviceDiscovery->errorString();
      finishDiscovery(generation, tr("Bluetooth discovery failed. Check system Bluetooth settings."));
    });
    // Watchdog only: use the platform's finished signal for normal completion.
    QTimer::singleShot(90000, this, devicesFinished);
    qInfo() << "Starting Bluetooth device discovery; methods:"
            << QBluetoothDeviceDiscoveryAgent::supportedDiscoveryMethods();
    m_deviceDiscovery->start(QBluetoothDeviceDiscoveryAgent::supportedDiscoveryMethods());
  });
#else
  m_statusRow->show();
  Q_EMIT statusVisibilityChanged(true);
  m_state->setText(tr("This build does not include Bluetooth support."));
#endif
}
