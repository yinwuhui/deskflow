// SPDX-License-Identifier: MIT
#include "BackupConnectionPanel.h"
#include "common/Settings.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <memory>
#ifdef DESKFLOW_BLUETOOTH
#include "BluetoothDiscovery.h"
#include <QBluetoothServiceDiscoveryAgent>
#endif
BackupConnectionPanel::BackupConnectionPanel(QWidget *parent) : QWidget(parent)
{
  setObjectName("backupConnectionPanel");
  auto *layout = new QVBoxLayout(this);
  auto *row = new QHBoxLayout;
  m_transport = new QComboBox(this);
  m_transport->addItem(tr("Local network"), "network");
  m_transport->addItem(tr("Bluetooth"), "bluetooth");
  m_transport->setCurrentIndex(Settings::value(Settings::Connection::InitialTransport) == "bluetooth" ? 1 : 0);
  m_bluetooth = new QLineEdit(Settings::value(Settings::Connection::BluetoothPeer).toString(), this);
  m_bluetooth->setMinimumWidth(0);
  m_bluetooth->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  m_bluetooth->setPlaceholderText(tr("Bluetooth address of the other computer"));
  m_scan = new QPushButton(tr("Find Bluetooth computers"), this);
  row->addWidget(new QLabel(tr("First connection"), this));
  row->addWidget(m_transport);
  row->addWidget(m_bluetooth, 1);
  row->addWidget(m_scan);
  layout->addLayout(row);
  m_automatic = new QCheckBox(tr("Automatically establish a backup connection"), this);
  m_automatic->setChecked(Settings::value(Settings::Connection::AutoBackup).toBool());
  layout->addWidget(m_automatic);
  row = new QHBoxLayout;
  m_peers = new QComboBox(this);
  m_peers->setMinimumWidth(130);
  m_state = new QLabel(tr("The first connection works independently of backup preparation."), this);
  m_state->setWordWrap(true);
  m_state->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  row->addWidget(m_peers);
  auto *protect = new QPushButton(tr("Use backup for this computer"), this);
  row->addWidget(protect);
  connect(protect, &QPushButton::clicked, this, [this] {
    if (!selectedPeer().isEmpty() && selectedPeer() != "bootstrap")
      Q_EMIT command({{"kind", "policy"}, {"peerId", selectedPeer()}, {"enabled", true}, {"select", true}});
  });
  row->addWidget(m_state, 1);
  layout->addLayout(row);
  row = new QHBoxLayout;
  m_action = new QPushButton(tr("Complete setup"), this);
  m_later = new QPushButton(tr("Later"), this);
  m_retry = new QPushButton(tr("Retry backup"), this);
  auto *disconnect = new QPushButton(tr("Disconnect selected computer"), this);
  row->addWidget(m_action);
  row->addWidget(m_later);
  row->addWidget(m_retry);
  row->addStretch();
  row->addWidget(disconnect);
  layout->addLayout(row);
  connect(m_transport, &QComboBox::currentIndexChanged, this, [this] {
    Settings::setValue(Settings::Connection::InitialTransport, m_transport->currentData());
    m_bluetooth->setVisible(m_transport->currentIndex() == 1);
    m_scan->setVisible(m_transport->currentIndex() == 1);
  });
  m_bluetooth->setVisible(m_transport->currentIndex() == 1);
  m_scan->setVisible(m_transport->currentIndex() == 1);
  connect(m_bluetooth, &QLineEdit::editingFinished, this, [this] {
    Settings::setValue(Settings::Connection::BluetoothPeer, m_bluetooth->text().trimmed());
  });
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
  connect(m_action, &QPushButton::clicked, this, &BackupConnectionPanel::performAction);
  connect(m_scan, &QPushButton::clicked, this, &BackupConnectionPanel::discover);
  connect(m_retry, &QPushButton::clicked, this, [this] {
    Q_EMIT command(
        {{"kind", m_peer.value("state") == "stopped" ? "reconnectPeer" : "retryBackup"}, {"peerId", selectedPeer()}}
    );
  });
  connect(m_later, &QPushButton::clicked, this, [this] {
    Q_EMIT command(
        {{"kind", "action"}, {"peerId", selectedPeer()}, {"actionId", m_peer.value("actionId")}, {"cancel", true}}
    );
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
void BackupConnectionPanel::setRunning(bool running)
{
  m_transport->setEnabled(!running);
  m_bluetooth->setEnabled(!running);
  m_scan->setEnabled(!running);
  if (!running) {
    m_snapshot = {};
    updateSnapshot({});
  }
}
void BackupConnectionPanel::updateSnapshot(const QJsonObject &snapshot)
{
  if (!m_snapshot.isEmpty() && snapshot.value("revision").toDouble() < m_snapshot.value("revision").toDouble())
    return;
  const auto selected = selectedPeer();
  m_snapshot = snapshot;
  m_updating = true;
  m_peers->clear();
  for (const auto &value : snapshot.value("peers").toArray()) {
    auto peer = value.toObject();
    m_peers->addItem(
        peer.value("name").toString().isEmpty() ? peer.value("peerId").toString().left(12)
                                                : peer.value("name").toString(),
        peer.value("peerId")
    );
  }
  const int index = m_peers->findData(selected);
  if (index >= 0)
    m_peers->setCurrentIndex(index);
  m_updating = false;
  showPeer();
}
void BackupConnectionPanel::showPeer()
{
  m_peer = {};
  for (const auto &value : m_snapshot.value("peers").toArray())
    if (value.toObject().value("peerId") == selectedPeer())
      m_peer = value.toObject();
  m_updating = true;
  m_automatic->setChecked(
      m_peer.isEmpty() ? Settings::value(Settings::Connection::AutoBackup).toBool() : m_peer.value("requested").toBool()
  );
  m_updating = false;
  m_automatic->setEnabled(m_peer.value("peerId") != "bootstrap");
  if (!Settings::value(Settings::Security::TlsEnabled).toBool() ||
      !Settings::value(Settings::Security::CheckPeers).toBool())
    m_state->setText(tr("Enable encryption and certificate verification to prepare backup connections."));
  else if (m_peer.isEmpty())
    m_state->setText(tr("The first connection works independently of backup preparation."));
  else {
    QString active = tr("Not connected");
    QString standby = tr("Preparing backup");
    for (const auto &lane : m_peer.value("channels").toArray())
      if (lane.toObject().value("state") == "active")
        active = lane.toObject().value("type") == "network" ? tr("Using local network") : tr("Using Bluetooth");
    const auto backup = m_peer.value("backup").toString();
    if (backup == "ready")
      standby = tr("Backup ready");
    else if (backup == "disabled")
      standby = tr("Backup disabled");
    else if (backup == "blockedByPeer")
      standby = tr("Backup is disabled by the other computer");
    else if (backup == "unavailable")
      standby = tr("Backup unavailable; current connection remains usable");
    else if (backup == "needsSelection")
      standby = tr("Select a computer to protect with a backup");
    else if (backup == "needsAction")
      standby = tr("Complete Bluetooth setup");
    if (m_peer.value("state") == "stopped")
      active = tr("Disconnected");
    m_state->setText(
        active + " · " + standby +
        (m_peer.value("effective").toBool() ? tr(" · Clipboard cap: up to 64 KiB (lower settings retained)") : QString()
        )
    );
    m_state->setToolTip(m_peer.value("reason").toString() + " " + m_snapshot.value("bluetoothReason").toString());
  }
  m_action->setEnabled(
      !m_peer.value("actionId").toString().isEmpty() ||
      (m_peer.isEmpty() && !m_snapshot.value("bluetoothReason").toString().isEmpty())
  );
  m_later->setEnabled(m_action->isEnabled());
  m_retry->setText(m_peer.value("state") == "stopped" ? tr("Reconnect") : tr("Retry backup"));
  m_retry->setEnabled(!m_peer.isEmpty());
}
void BackupConnectionPanel::operationFinished(const QJsonObject &result)
{
  if (result.value("accepted").toBool() || result.value("peerId").toString() != selectedPeer())
    return;
  m_state->setText(
      result.value("error") == "identityUntrusted"
          ? tr("Review this computer's certificate trust before reconnecting.")
          : tr("This action is no longer available. Refresh the connection and try again.")
  );
}
void BackupConnectionPanel::performAction()
{
  if (m_peer.isEmpty()) {
#ifdef Q_OS_MACOS
    QDesktopServices::openUrl(QUrl("x-apple.systempreferences:com.apple.preference.security?Privacy_Bluetooth"));
#endif
    Q_EMIT command({{"kind", "retryBluetooth"}});
    return;
  }
  QJsonObject request{{"kind", "action"}, {"peerId", selectedPeer()}, {"actionId", m_peer.value("actionId")}};
  const auto action = m_peer.value("action").toString();
  bool ok = false;
  if (action == "pairingPin") {
    const auto pin =
        QInputDialog::getText(this, tr("Bluetooth pairing"), tr("Enter the pairing PIN"), QLineEdit::Password, {}, &ok);
    if (!ok)
      return;
    request.insert("pin", pin);
  } else if (action == "confirmPairing") {
    const auto answer = QInputDialog::getItem(
        this, tr("Bluetooth pairing"),
        tr("Confirm the code on both computers: %1").arg(m_peer.value("reason").toString()),
        {tr("Confirm"), tr("Reject")}, 0, false, &ok
    );
    if (!ok)
      return;
    request.insert("accepted", answer == tr("Confirm"));
  } else if (action == "selectBluetoothDevice") {
    const auto address = QInputDialog::getText(
        this, tr("Bluetooth computer"),
        tr("Automatic discovery could not locate this computer. Enter its Bluetooth address."), QLineEdit::Normal, {},
        &ok
    );
    if (!ok)
      return;
    request.insert("address", address);
  } else if (action == "displayPairingCode") {
    m_state->setText(tr("Enter this code on the other computer: %1").arg(m_peer.value("reason").toString()));
    return;
  } else if (action == "bluetoothSettings") {
#ifdef Q_OS_MACOS
    QDesktopServices::openUrl(QUrl("x-apple.systempreferences:com.apple.preference.security?Privacy_Bluetooth"));
#endif
  }
  Q_EMIT command(request);
}
void BackupConnectionPanel::discover()
{
#ifdef DESKFLOW_BLUETOOTH
  m_scan->setEnabled(false);
  m_state->setText(tr("Finding Bluetooth computers…"));
  requestBluetoothPermission(this, false, [this](bool granted) {
    if (!granted) {
      m_scan->setEnabled(true);
      m_state->setText(tr("Bluetooth permission is required for discovery."));
      return;
    }
    auto *agent = new QBluetoothServiceDiscoveryAgent(this);
    agent->setUuidFilter(QBluetoothUuid(QStringLiteral("28d88531-5adc-4852-bf1c-2bc9107c315e")));
    auto addresses = std::make_shared<QStringList>();
    connect(agent, &QBluetoothServiceDiscoveryAgent::serviceDiscovered, this, [addresses](const auto &service) {
      const auto address = service.device().address().toString();
      if (!addresses->contains(address))
        addresses->append(address);
    });
    auto done = [this, agent, addresses] {
      agent->disconnect(this);
      agent->stop();
      agent->deleteLater();
      m_scan->setEnabled(true);
      if (addresses->isEmpty()) {
        m_state->setText(tr("No Bluetooth computers found. Start Bluetooth sharing on the other computer."));
        return;
      }
      bool ok = false;
      const auto address = QInputDialog::getItem(
          this, tr("Bluetooth computers"), tr("Select a computer; its identity will be verified when connecting."),
          *addresses, 0, false, &ok
      );
      if (ok) {
        m_bluetooth->setText(address);
        Settings::setValue(Settings::Connection::BluetoothPeer, address);
      }
    };
    connect(agent, &QBluetoothServiceDiscoveryAgent::finished, this, done);
    connect(agent, &QBluetoothServiceDiscoveryAgent::canceled, this, done);
    connect(agent, &QBluetoothServiceDiscoveryAgent::errorOccurred, this, done);
    QTimer::singleShot(20000, agent, &QBluetoothServiceDiscoveryAgent::stop);
    agent->start(QBluetoothServiceDiscoveryAgent::FullDiscovery);
  });
#else
  m_state->setText(tr("This build does not include Bluetooth support."));
#endif
}
