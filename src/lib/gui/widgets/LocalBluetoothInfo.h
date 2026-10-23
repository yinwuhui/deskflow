// SPDX-License-Identifier: MIT
#pragma once
#include <QWidget>
class QLabel;
class QPushButton;
class QBluetoothLocalDevice;
class HomeIdentityLabel;

// Reads local adapter information only after permission is granted; startup never prompts.
class LocalBluetoothInfo : public QWidget
{
  Q_OBJECT
public:
  explicit LocalBluetoothInfo(QWidget *parent = nullptr);
  void refresh();

private:
  void requestAccess();
  HomeIdentityLabel *m_name;
  QLabel *m_state;
  QPushButton *m_action;
  QBluetoothLocalDevice *m_adapter = nullptr;
  bool m_requesting = false;
};
