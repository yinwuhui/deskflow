// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QJsonValue>
#include <QWidget>

class PeerUsageModel;
class QLabel;
class DeviceConnectionInfo : public QWidget
{
  Q_OBJECT
public:
  explicit DeviceConnectionInfo(QWidget *parent = nullptr, PeerUsageModel *model = nullptr);
  void setDevice(const QString &key, const QString &savedAddress = {});

private:
  void refresh();
  QString metric(const QJsonValue &value, bool rate) const;
  PeerUsageModel *m_model;
  QString m_device, m_savedAddress;
  QLabel *m_input, *m_display;
  QLabel *m_states[2], *m_latency[2], *m_addresses[2], *m_rates[2];
};
