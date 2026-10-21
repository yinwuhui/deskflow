// SPDX-License-Identifier: MIT
#include "TransportNotifications.h"

TransportNotifications::TransportNotifications(QObject *parent, int delayMs) : QObject(parent)
{
  m_delay.setSingleShot(true);
  m_delay.setInterval(delayMs);
  connect(&m_delay, &QTimer::timeout, this, [this] {
    if (m_recovering && !m_offlineShown)
      Q_EMIT notification(
          tr("Connection interrupted"),
          tr("Remote control has stopped. Deskflow is automatically trying another connection.")
      );
  });
}

void TransportNotifications::reset()
{
  m_delay.stop();
  m_online = m_recovering = m_offlineShown = false;
  m_last.clear();
  Q_EMIT statusChanged({});
}

void TransportNotifications::update(const QString &state)
{
  if (state == m_last)
    return;
  if (state == "switching") {
    if (!m_online || m_recovering)
      return;
    m_last = state;
    m_recovering = true;
    m_delay.start();
    Q_EMIT statusChanged(tr("Switching connection; control remains local."));
  } else if (state == "offline") {
    if (m_offlineShown)
      return;
    m_last = state;
    m_delay.stop();
    m_offlineShown = true;
    const auto body =
        tr("Unable to connect to the other computer. Control remains local; Deskflow will keep retrying automatically."
        );
    Q_EMIT statusChanged(tr("Offline; retrying automatically."));
    if (m_recovering)
      Q_EMIT notification(tr("Waiting for a connection"), body);
  } else if (state == "connecting") {
    m_last = state;
    Q_EMIT statusChanged(tr("Reconnecting to the other computer automatically..."));
  } else if (state == "network" || state == "bluetooth") {
    m_last = state;
    const auto transport = state == "network" ? tr("network") : tr("Bluetooth");
    const auto body =
        tr("Connected via %1. Move the mouse to the screen edge to control the other computer.").arg(transport);
    m_delay.stop();
    Q_EMIT statusChanged(tr("Using %1; move to the screen edge to resume.").arg(transport));
    if (m_recovering)
      Q_EMIT notification(tr("Connection restored"), body);
    m_online = true;
    m_recovering = m_offlineShown = false;
  }
}
