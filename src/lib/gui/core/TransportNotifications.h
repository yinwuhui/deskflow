// SPDX-License-Identifier: MIT
#pragma once
#include <QObject>
#include <QTimer>

// Presentation policy: coalesce quick recovery, report each outage once, never steal focus.
class TransportNotifications : public QObject
{
  Q_OBJECT
public:
  explicit TransportNotifications(QObject *parent = nullptr, int delayMs = 1200);
  void update(const QString &state);
  void reset();
Q_SIGNALS:
  void notification(const QString &title, const QString &body);
  void statusChanged(const QString &text);

private:
  QTimer m_delay;
  bool m_online = false, m_recovering = false, m_offlineShown = false;
  QString m_last;
};
