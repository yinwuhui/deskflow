/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerBrowserDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QListWidget>

namespace deskflow::gui {

ServerBrowserDialog::ServerBrowserDialog(deskflow::mdns::MdnsBrowser *browser, QWidget *parent)
    : QDialog(parent),
      m_browser(browser),
      m_list(new QListWidget(this)),
      m_status(new QLabel(this)),
      m_btnConnect(new QPushButton(tr("Connect"), this)),
      m_btnRescan(new QPushButton(tr("Rescan"), this)),
      m_autoConnectCheck(new QCheckBox(tr("Connect to this server automatically next time"), this))
{
  setWindowTitle(tr("Deskflow servers on local network"));
  setMinimumWidth(420);

  auto *layout = new QVBoxLayout(this);

  m_list->setAlternatingRowColors(true);
  m_list->setSelectionMode(QAbstractItemView::SingleSelection);
  layout->addWidget(m_list);

  m_status->setText(tr("Scanning…"));
  layout->addWidget(m_status);

  layout->addWidget(m_autoConnectCheck);

  auto *buttons = new QDialogButtonBox;
  buttons->addButton(m_btnRescan, QDialogButtonBox::ActionRole);
  buttons->addButton(m_btnConnect, QDialogButtonBox::AcceptRole);
  buttons->addButton(QDialogButtonBox::Cancel);
  layout->addWidget(buttons);

  m_btnConnect->setEnabled(false);

  connect(m_list, &QListWidget::itemDoubleClicked, this, &ServerBrowserDialog::onItemActivated);
  connect(m_list, &QListWidget::itemSelectionChanged, this,
          [this] { m_btnConnect->setEnabled(m_list->currentRow() >= 0); });
  connect(m_btnConnect, &QPushButton::clicked, this,
          [this] { if (auto *item = m_list->currentItem()) onItemActivated(item); });
  connect(m_btnRescan, &QPushButton::clicked, this, [this] {
    m_status->setText(tr("Scanning…"));
    refreshList();
  });

  if (m_browser)
    connect(m_browser, &deskflow::mdns::MdnsBrowser::serversChanged, this,
            &ServerBrowserDialog::refreshList);

  refreshList();
}

void ServerBrowserDialog::refreshList()
{
  if (!m_browser) {
    m_status->setText(tr("Discovery not available"));
    return;
  }
  const auto servers = m_browser->servers();
  m_list->clear();
  for (const auto &s : servers) {
    auto *item = new QListWidgetItem(m_list);
    QString label = s.name;
    if (!s.hostName.isEmpty())
      label += QStringLiteral("  (%1)").arg(s.hostName);
    if (!s.ipAddress.isEmpty())
      label += QStringLiteral("  %1:%2").arg(s.ipAddress).arg(s.port);
    item->setText(label);
    item->setData(Qt::UserRole, s.hostName);
  }
  if (servers.isEmpty())
    m_status->setText(tr("No servers found yet — scanning…"));
  else
    m_status->setText(tr("%n server(s) found", nullptr, servers.size()));
}

void ServerBrowserDialog::onItemActivated(QListWidgetItem *item)
{
  if (!item)
    return;
  m_selectedHostName = item->data(Qt::UserRole).toString();
  m_autoConnect = m_autoConnectCheck->isChecked();
  accept();
}

} // namespace deskflow::gui
