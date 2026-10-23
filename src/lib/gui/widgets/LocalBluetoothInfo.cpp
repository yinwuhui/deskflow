// SPDX-License-Identifier: MIT
#include "LocalBluetoothInfo.h"
#include "HomeIcons.h"
#include "HomeIdentityLabel.h"
#include <QApplication>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#ifdef DESKFLOW_BLUETOOTH
#include "BluetoothDiscovery.h"
#include <QBluetoothLocalDevice>
#include <QBluetoothPermission>
#endif
using namespace deskflow::gui;
LocalBluetoothInfo::LocalBluetoothInfo(QWidget *parent) : QWidget(parent)
{
  setObjectName("localBluetoothRow");
  setFixedHeight(28);
  auto *row = new QHBoxLayout(this);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(6);
  auto *icon = new QLabel(this);
  icon->setPixmap(homeIcon(HomeIcon::Bluetooth, QColor("#087fff")).pixmap(16, 16));
  icon->setFixedSize(18, 20);
  m_name = new HomeIdentityLabel(this);
  m_name->setObjectName("lblLocalBluetoothName");
  m_name->setTextFormat(Qt::PlainText);
  m_name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  m_name->setStyleSheet("font-size: 13px;");
  m_state = new QLabel(this);
  m_state->setObjectName("lblLocalBluetoothState");
  m_state->setProperty("role", "muted");
  m_action = new QPushButton(this);
  m_action->setObjectName("btnLocalBluetoothAction");
  m_action->setProperty("role", "link");
  m_action->setFixedSize(42, 26);
  m_action->setStyleSheet("padding: 0; font-size: 12px;");
  row->addWidget(icon);
  row->addWidget(m_name, 1);
  row->addWidget(m_state);
  row->addWidget(m_action);
  connect(m_action, &QPushButton::clicked, this, &LocalBluetoothInfo::requestAccess);
  connect(qApp, &QGuiApplication::applicationStateChanged, this, [this] { refresh(); });
  auto *timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this, &LocalBluetoothInfo::refresh);
  timer->start(5000);
  refresh();
}
void LocalBluetoothInfo::refresh()
{
#ifdef DESKFLOW_BLUETOOTH
  const auto permission = qApp->checkPermission(QBluetoothPermission());
  if (permission != Qt::PermissionStatus::Granted) {
    if (m_adapter) {
      delete m_adapter;
      m_adapter = nullptr;
    }
    m_name->setIdentity(tr("Bluetooth"));
    m_state->setText(tr("Not authorized"));
    m_action->setText(tr("Allow"));
    m_action->setEnabled(!m_requesting);
    m_name->setToolTip(tr("Bluetooth information is available after authorization."));
    return;
  }
  if (!m_adapter) {
    m_adapter = new QBluetoothLocalDevice(this);
    connect(m_adapter, &QBluetoothLocalDevice::hostModeStateChanged, this, [this] { refresh(); });
  }
  const auto name = m_adapter->name();
  m_name->setIdentity(name.isEmpty() ? tr("Local Bluetooth") : name);
  m_name->setToolTip(name + (m_adapter->address().isNull() ? QString() : "\n" + m_adapter->address().toString()));
  m_name->setAccessibleName(tr("Local Bluetooth: %1").arg(name.isEmpty() ? tr("Name unavailable") : name));
  m_state->setText(
      !m_adapter->isValid()                                            ? tr("Unavailable")
      : m_adapter->hostMode() == QBluetoothLocalDevice::HostPoweredOff ? tr("Off")
                                                                       : tr("On")
  );
  m_action->setText(tr("Details"));
  m_action->setEnabled(true);
#else
  m_name->setIdentity(tr("Bluetooth"));
  m_state->setText(tr("Not supported"));
  m_action->setText(tr("Details"));
  m_action->setEnabled(false);
#endif
}
void LocalBluetoothInfo::requestAccess()
{
#ifdef DESKFLOW_BLUETOOTH
  if (m_requesting)
    return;
  const auto permission = qApp->checkPermission(QBluetoothPermission());
  if (permission == Qt::PermissionStatus::Granted) {
    const auto information =
        tr("Local Bluetooth: %1").arg(m_adapter ? m_adapter->name() : tr("Name unavailable")) + "\n" + m_state->text() +
        (m_adapter && !m_adapter->address().isNull() ? "\n" + m_adapter->address().toString() : QString());
    QToolTip::showText(
        m_action->mapToGlobal(QPoint(0, m_action->height())),
        "<qt>" + information.toHtmlEscaped().replace('\n', "<br>") + "</qt>", m_action
    );
    return;
  }
  if (permission == Qt::PermissionStatus::Undetermined) {
    m_requesting = true;
    m_action->setEnabled(false);
    requestBluetoothPermission(this, false, [this](bool) {
      m_requesting = false;
      refresh();
    });
    return;
  }
#ifdef Q_OS_MACOS
  QDesktopServices::openUrl(QUrl(
      permission == Qt::PermissionStatus::Denied
          ? "x-apple.systempreferences:com.apple.preference.security?Privacy_Bluetooth"
          : "x-apple.systempreferences:com.apple.BluetoothSettings"
  ));
#else
  m_name->setToolTip(tr("Open your system Bluetooth settings to inspect the adapter and permissions."));
  m_name->setIdentity(tr("System Bluetooth settings"));
#endif
#endif
}
