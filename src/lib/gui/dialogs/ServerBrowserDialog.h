/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 *
 * ServerBrowserDialog: shows Deskflow servers discovered on the local network
 * via mDNS and lets the user pick one to connect to.
 */

#pragma once

#include "net/MdnsBrowser.h"

#include <QCheckBox>
#include <QDialog>
#include <QListWidget>

class QPushButton;
class QLabel;

namespace deskflow::gui {

class ServerBrowserDialog : public QDialog
{
  Q_OBJECT
public:
  explicit ServerBrowserDialog(deskflow::mdns::MdnsBrowser *browser, QWidget *parent = nullptr);

  // The discovered IPv4 address and port to connect to if accepted.
  QString selectedAddress() const
  {
    return m_selectedAddress;
  }

  // Whether the user asked to auto-connect to this server on future launches.
  bool autoConnectRequested() const
  {
    return m_autoConnect;
  }

private Q_SLOTS:
  void refreshList();
  void onItemActivated(QListWidgetItem *item);

private:
  deskflow::mdns::MdnsBrowser *m_browser;
  QListWidget *m_list;
  QLabel *m_status;
  QPushButton *m_btnConnect;
  QPushButton *m_btnRescan;
  QCheckBox *m_autoConnectCheck;
  QString m_selectedAddress;
  bool m_autoConnect = false;
};

} // namespace deskflow::gui
