// SPDX-License-Identifier: MIT
#include "BackupConnectionPanel.h"
#include "common/Settings.h"
#include "gui/widgets/HomeIcons.h"
#include "gui/widgets/HomeToggleSwitch.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
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
#include <QBluetoothServiceDiscoveryAgent>
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
  m_network = new QPushButton(tr("Local network"), m_transport);
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
  connect(m_network, &QPushButton::clicked, this, [this] {
    Settings::setValue(Settings::Connection::InitialTransport, "network");
    Q_EMIT transportChanged();
  });
  connect(m_bluetoothButton, &QPushButton::clicked, this, [this] {
    Settings::setValue(Settings::Connection::InitialTransport, "bluetooth");
    Q_EMIT transportChanged();
  });
  m_bluetooth = new QComboBox(this);
  m_bluetooth->setObjectName("bluetoothTarget");
  m_bluetooth->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  m_bluetooth->setFixedHeight(44);
  m_bluetooth->setAccessibleName(tr("Bluetooth computer"));
  m_bluetooth->addItem(tr("Choose a nearby computer"), QString());
  const auto saved = Settings::value(Settings::Connection::BluetoothPeer).toString();
  if (!saved.isEmpty()) {
    m_bluetooth->addItem(tr("Saved Bluetooth computer"), saved);
    m_bluetooth->setCurrentIndex(1);
  }
  connect(m_bluetooth, &QComboBox::currentIndexChanged, this, [this] {
    if (!m_updating)
      Settings::setValue(Settings::Connection::BluetoothPeer, m_bluetooth->currentData());
  });
  auto *policyRow = new QHBoxLayout;
  m_automatic = new HomeToggleSwitch(tr("Automatically establish a backup connection"), this);
  m_automatic->setObjectName("checkAutoBackup");
  m_automatic->setChecked(Settings::value(Settings::Connection::AutoBackup).toBool());
  policyRow->addWidget(m_automatic);
  policyRow->addStretch();
  layout->addLayout(policyRow);
  auto *description = plainLabel(
      tr("After connecting, prepare the other transport automatically and recover if the current connection fails."),
      this, "lblBackupDescription"
  );
  description->setProperty("role", "muted");
  description->setFixedHeight(30);
  layout->addWidget(description);
  auto *statusRow = new QHBoxLayout;
  m_state = plainLabel({}, this, "lblBackupStatus");
  m_state->setProperty("role", "muted");
  m_state->setFixedHeight(38);
  statusRow->addWidget(m_state, 1);
  m_action = new QPushButton(tr("Connection details"), this);
  m_action->setObjectName("btnBackupDetails");
  m_action->setProperty("role", "link");
  statusRow->addWidget(m_action);
  layout->addLayout(statusRow);
  setFixedHeight(108);

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
  m_scan = new QPushButton(tr("Find Bluetooth computers"), m_details);
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
      if (bluetoothSelected())
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
  } else if (changed)
    Q_EMIT command({{"kind", "snapshot"}});
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
  m_retry->setEnabled(
      !m_peer.isEmpty() ||
      (m_running && bluetoothSelected() && !m_snapshot.value("bluetoothReason").toString().isEmpty())
  );
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
      m_actionTargets->addItem(m_bluetooth->itemText(i), m_bluetooth->itemData(i));
  }
  m_actionInput->setAccessibleName(action == "pairingPin" ? tr("Pairing PIN") : tr("Bluetooth address"));
  m_actionLabel->setText(
      action == "confirmPairing" ? tr("Confirm the code on both computers: %1").arg(m_peer.value("reason").toString())
      : action == "pairingPin"   ? tr("Enter the pairing PIN")
      : action == "selectBluetoothDevice"
          ? tr("Choose the computer with Find Bluetooth computers, or enter its Bluetooth address.")
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
void BackupConnectionPanel::finishDiscovery(quint64 generation, const QString &error)
{
  if (generation != m_scanGeneration || !m_scanRequest)
    return;
  m_scanRequest = false;
#ifdef DESKFLOW_BLUETOOTH
  if (m_discovery) {
    m_discovery->disconnect(this);
    m_discovery->stop();
    m_discovery->deleteLater();
    m_discovery = nullptr;
  }
#endif
  m_scan->setEnabled(!m_running || m_peer.value("action") == "selectBluetoothDevice");
  if (!error.isEmpty())
    m_state->setText(error);
  else
    m_state->setText(
        m_bluetooth->count() > 1 ? tr("Select a Bluetooth computer; its identity is verified when connecting.")
                                 : tr("No Bluetooth computers found. Start Bluetooth sharing on the other computer.")
    );
}
void BackupConnectionPanel::discover()
{
#ifdef DESKFLOW_BLUETOOTH
  if (m_scanRequest)
    return;
  // Startup discovery and an action's target selection are explicit user operations.
  const auto generation = ++m_scanGeneration;
  m_scanRequest = true;
  m_scan->setEnabled(false);
  m_state->setText(tr("Finding Bluetooth computers…"));
  requestBluetoothPermission(this, false, [this, generation](bool granted) {
    if (generation != m_scanGeneration || !m_scanRequest)
      return;
    if (!granted) {
      finishDiscovery(generation, tr("Bluetooth permission is required for discovery."));
      return;
    }
    m_discovery = new QBluetoothServiceDiscoveryAgent(this);
    m_discovery->setUuidFilter(QBluetoothUuid(QStringLiteral("28d88531-5adc-4852-bf1c-2bc9107c315e")));
    connect(
        m_discovery, &QBluetoothServiceDiscoveryAgent::serviceDiscovered, this,
        [this, generation](const auto &service) {
          if (generation != m_scanGeneration || !m_scanRequest)
            return;
          const auto address = service.device().address().toString();
          if (m_bluetooth->findData(address) >= 0)
            return;
          auto name = service.device().name();
          if (name.isEmpty())
            name = tr("Unnamed computer");
          m_bluetooth->addItem(name, address);
          m_bluetooth->setItemData(m_bluetooth->count() - 1, address, Qt::ToolTipRole);
          if (m_actionTargets->findData(address) < 0)
            m_actionTargets->addItem(name, address);
        }
    );
    connect(m_discovery, &QBluetoothServiceDiscoveryAgent::finished, this, [this, generation] {
      finishDiscovery(generation);
    });
    connect(m_discovery, &QBluetoothServiceDiscoveryAgent::canceled, this, [this, generation] {
      finishDiscovery(generation);
    });
    connect(m_discovery, &QBluetoothServiceDiscoveryAgent::errorOccurred, this, [this, generation] {
      finishDiscovery(generation, tr("Bluetooth discovery failed. Check system Bluetooth settings."));
    });
    QTimer::singleShot(20000, this, [this, generation] { finishDiscovery(generation); });
    m_discovery->start(QBluetoothServiceDiscoveryAgent::FullDiscovery);
  });
#else
  m_state->setText(tr("This build does not include Bluetooth support."));
#endif
}
