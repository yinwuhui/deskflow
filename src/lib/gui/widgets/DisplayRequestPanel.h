// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QFrame>
#include <QJsonObject>
class QLabel;
class QPushButton;
// Device-scoped inline consent card. No display permission is inferred from
// an existing keyboard/mouse connection.
class DisplayRequestPanel : public QFrame
{
  Q_OBJECT
public:
  explicit DisplayRequestPanel(QWidget *parent);
  void setDevice(const QString &device);
  static QString peerForDevice(const QString &device);
  static bool needsAttention(const QJsonObject &snapshot);

private:
  void updateRequest(const QJsonObject &snapshot);
  void respond(bool allow);
  QLabel *m_title, *m_peer, *m_status;
  QPushButton *m_allow, *m_reject;
  QJsonObject m_snapshot;
  QString m_pending, m_device, m_errorText;
};
