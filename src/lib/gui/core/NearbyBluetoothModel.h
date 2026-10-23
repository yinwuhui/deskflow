// SPDX-License-Identifier: MIT
#pragma once
#include <QCoreApplication>
#include <QStandardItemModel>

// Enumeration only: selection never queries services, pairs, or opens a connection.
class NearbyBluetoothModel : public QStandardItemModel
{
  Q_DECLARE_TR_FUNCTIONS(NearbyBluetoothModel)
public:
  enum Role
  {
    Address = Qt::UserRole,
    Identity,
    Name,
    SortKey,
    Seen,
    ClassicCapable
  };
  explicit NearbyBluetoothModel(QObject *parent = nullptr);
  void resetDevices(const QString &savedAddress = {});
  void beginScan();
  void addDevice(const QString &identity, const QString &address, const QString &name, bool classic);
  void finishScan();
  void setScanningText(bool scanning);
  int rowForIdentity(const QString &identity) const;
  QString connectableAddress(int row) const;
  int foundCount() const;

private:
  void refreshItem(QStandardItem *item);
};
