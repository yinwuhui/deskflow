// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QJsonObject>
#include <QWidget>
class QLabel;
class QPushButton;
// Non-modal in-window consent card. No display permission is inferred from
// an existing keyboard/mouse connection.
class DisplayRequestPanel : public QWidget
{
  Q_OBJECT
public:
  explicit DisplayRequestPanel(QWidget *parent);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  void updateRequest(const QJsonObject &snapshot);
  void respond(bool allow);
  void place();
  QLabel *m_peer, *m_status;
  QPushButton *m_allow, *m_reject;
  QJsonObject m_snapshot;
  QString m_pending;
};
