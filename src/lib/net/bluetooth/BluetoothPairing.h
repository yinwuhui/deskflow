// SPDX-License-Identifier: MIT
#pragma once
#include <QBluetoothAddress>
#include <QBluetoothLocalDevice>
#include <QObject>
#include <QTimer>

// No confirmation is accepted automatically. A future UI (or CLI) must reply explicitly.
class BluetoothPairing : public QObject
{
  Q_OBJECT
public:
  explicit BluetoothPairing(QObject *parent = nullptr);
  ~BluetoothPairing() override;
  void pair(const QBluetoothAddress &peer);
  void confirm(bool accepted);
  void providePin(const QString &pin);
  void cancel();
  void complete(bool success, const QString &error = {});
Q_SIGNALS:
  void confirmationRequired(const QString &sixDigitCode);
  void pinRequired();
  void passkeyDisplayed(const QString &sixDigitCode);
  void finished(bool success, const QString &error);

private:
  QTimer m_deadline;
  bool m_active = false;
#ifdef Q_OS_MACOS
  void *m_native = nullptr;
#else
  QBluetoothLocalDevice m_adapter;
  QBluetoothAddress m_peer;
#endif
};
