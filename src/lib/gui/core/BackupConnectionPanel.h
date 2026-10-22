// SPDX-License-Identifier: MIT
#pragma once
#include <QJsonObject>
#include <QWidget>
class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QLineEdit;
class BackupConnectionPanel : public QWidget
{
  Q_OBJECT
public:
  explicit BackupConnectionPanel(QWidget *parent = nullptr);
  void updateSnapshot(const QJsonObject &snapshot);
  void setRunning(bool running);
  void operationFinished(const QJsonObject &result);
  QString selectedPeer() const;
Q_SIGNALS:
  void command(const QJsonObject &command);

private:
  void showPeer();
  void performAction();
  void discover();
  QJsonObject m_snapshot, m_peer;
  QComboBox *m_transport, *m_peers;
  QCheckBox *m_automatic;
  QLineEdit *m_bluetooth;
  QLabel *m_state;
  QPushButton *m_action, *m_retry, *m_later, *m_scan;
  bool m_updating = false;
};
