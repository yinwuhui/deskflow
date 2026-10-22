/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerBrowserDialog.h"

#include "common/Settings.h"

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
  setWindowTitle(tr("busymouse servers on local network"));
  setMinimumWidth(420);

  auto *layout = new QVBoxLayout(this);

  m_list->setAlternatingRowColors(true);
  m_list->setSelectionMode(QAbstractItemView::SingleSelection);
  layout->addWidget(m_list);

  m_status->setText(tr("Scanning…"));
  layout->addWidget(m_status);

  m_autoConnectCheck->setChecked(Settings::value(Settings::Client::AutoConnect).toBool());
  layout->addWidget(m_autoConnectCheck);

  auto *buttons = new QDialogButtonBox;
  buttons->addButton(m_btnRescan, QDialogButtonBox::ActionRole);
  buttons->addButton(m_btnConnect, QDialogButtonBox::AcceptRole);
  buttons->addButton(QDialogButtonBox::Cancel);
  layout->addWidget(buttons);

  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
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
  const auto *selected = m_list->currentItem();
  const QString selectedKey = selected ? selected->data(Qt::UserRole + 1).toString() : QString();
  m_list->clear();
  for (const auto &s : servers) {
    if (s.ipAddress.isEmpty() || s.port <= 0 || s.port > 65535)
      continue;
    auto *item = new QListWidgetItem(m_list);
    QString label = s.name;
    if (!s.hostName.isEmpty())
      label += QStringLiteral("  (%1)").arg(s.hostName);
    if (!s.ipAddress.isEmpty())
      label += QStringLiteral("  %1:%2").arg(s.ipAddress).arg(s.port);
    item->setText(label);
    item->setData(Qt::UserRole, QStringLiteral("%1:%2").arg(s.ipAddress).arg(s.port));
    const QString key = QStringLiteral("%1:%2").arg(s.hostName).arg(s.port);
    item->setData(Qt::UserRole + 1, key);
    if (key == selectedKey)
      m_list->setCurrentItem(item);
    if (!s.fingerprint.isEmpty())
      item->setToolTip(tr("Advertised certificate fingerprint (unverified): %1").arg(s.fingerprint));
  }
  if (m_list->count() == 0)
    m_status->setText(tr("No servers found yet — scanning…"));
  else
    m_status->setText(tr("%n server(s) found", nullptr, m_list->count()));
}

void ServerBrowserDialog::onItemActivated(QListWidgetItem *item)
{
  if (!item)
    return;
  m_selectedAddress = item->data(Qt::UserRole).toString();
  m_autoConnect = m_autoConnectCheck->isChecked();
  accept();
}

} // namespace deskflow::gui
