// SPDX-License-Identifier: MIT
#include "NearbyBluetoothModel.h"

NearbyBluetoothModel::NearbyBluetoothModel(QObject *parent) : QStandardItemModel(parent)
{
  setObjectName("nearbyBluetoothModel");
  setSortRole(SortKey);
  resetDevices();
}
void NearbyBluetoothModel::resetDevices(const QString &savedAddress)
{
  clear();
  auto *placeholder = new QStandardItem(tr("Choose a nearby device"));
  placeholder->setData(QString(), Address);
  placeholder->setData(QStringLiteral("0"), SortKey);
  appendRow(placeholder);
  if (!savedAddress.isEmpty()) {
    addDevice(savedAddress, savedAddress, tr("Saved Bluetooth computer"), true);
    item(1)->setData(false, Seen);
    refreshItem(item(1));
  }
}
int NearbyBluetoothModel::rowForIdentity(const QString &identity) const
{
  for (int row = 1; row < rowCount(); ++row)
    if (item(row)->data(Identity).toString() == identity)
      return row;
  return -1;
}
void NearbyBluetoothModel::beginScan()
{
  for (int row = 1; row < rowCount(); ++row) {
    item(row)->setData(false, Seen);
    refreshItem(item(row));
  }
  setScanningText(true);
}
void NearbyBluetoothModel::addDevice(const QString &identity, const QString &address, const QString &name, bool classic)
{
  if (identity.isEmpty())
    return;
  int row = rowForIdentity(identity);
  QStandardItem *entry = row < 0 ? new QStandardItem : item(row);
  classic = classic || entry->data(ClassicCapable).toBool();
  entry->setData(classic, ClassicCapable);
  entry->setData(identity, Identity);
  entry->setData(address, Address);
  if (!name.isEmpty() || entry->data(Name).toString().isEmpty())
    entry->setData(name.isEmpty() ? tr("Unnamed device") : name, Name);
  entry->setData(true, Seen);
  refreshItem(entry);
  if (row < 0)
    appendRow(entry);
}
void NearbyBluetoothModel::refreshItem(QStandardItem *entry)
{
  const auto name = entry->data(Name).toString();
  const auto address = entry->data(Address).toString();
  QString hint = name;
  if (!address.isEmpty())
    hint += "\n" + address;
  if (!entry->data(ClassicCapable).toBool() || address.isEmpty())
    hint += "\n" + tr("This low energy device cannot use the current keyboard and mouse transport.");
  else if (!entry->data(Seen).toBool())
    hint += "\n" + tr("Not seen nearby");
  entry->setText(name);
  entry->setToolTip(hint);
  entry->setData("1" + name.toCaseFolded(), SortKey);
}
void NearbyBluetoothModel::finishScan()
{
  for (int row = rowCount() - 1; row > 0; --row) {
    if (!item(row)->data(Seen).toBool()) {
      removeRow(row);
      continue;
    }
  }
  setScanningText(false);
}
void NearbyBluetoothModel::setScanningText(bool scanning)
{
  item(0)->setText(scanning ? tr("Finding nearby devices…") : tr("Choose a nearby device"));
}
QString NearbyBluetoothModel::connectableAddress(int row) const
{
  if (row <= 0 || row >= rowCount() || !item(row)->data(ClassicCapable).toBool() || !item(row)->data(Seen).toBool())
    return {};
  return item(row)->data(Address).toString();
}
int NearbyBluetoothModel::foundCount() const
{
  int count = 0;
  for (int row = 1; row < rowCount(); ++row)
    count += item(row)->data(Seen).toBool();
  return count;
}
