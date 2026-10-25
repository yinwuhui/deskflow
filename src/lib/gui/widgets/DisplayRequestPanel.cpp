// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "DisplayRequestPanel.h"
#include "gui/core/PeerUsageModel.h"
#include <QEvent>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QUuid>
#include <QVBoxLayout>
#ifdef DESKFLOW_DISPLAY
#include "display/DisplayService.h"
#endif
DisplayRequestPanel::DisplayRequestPanel(QWidget *parent) : QWidget(parent)
{
  setObjectName("displayRequestPanel");
  setStyleSheet(R"(
    QWidget#displayRequestPanel {background:white;border:1px solid #d7e5f7;border-radius:12px;}
    QWidget#displayRequestPanel QLabel {background:transparent;border:none;color:#536174;font-size:13px;}
    QWidget#displayRequestPanel QPushButton {border:1px solid #dce4ef;border-radius:7px;padding:9px 16px;background:white;color:#536174;}
    QWidget#displayRequestPanel QPushButton#allowDisplayRequest {background:#087fff;color:white;border-color:#087fff;}
    QWidget#displayRequestPanel QPushButton:disabled {background:#e9eef5;color:#99a4b1;}
  )");
  auto layout = new QVBoxLayout(this);
  layout->setContentsMargins(20, 18, 20, 18);
  layout->setSpacing(12);
  auto title = new QLabel(tr("Screen use request"), this);
  title->setStyleSheet("font-size:17px;font-weight:600;color:#303640;");
  layout->addWidget(title);
  m_peer = new QLabel(this);
  m_peer->setTextFormat(Qt::PlainText);
  m_peer->setWordWrap(true);
  layout->addWidget(m_peer);
  m_status = new QLabel(tr("Allow this computer's screen to extend the other desktop for this session?"), this);
  m_status->setWordWrap(true);
  layout->addWidget(m_status);
  auto buttons = new QHBoxLayout;
  buttons->addStretch();
  m_reject = new QPushButton(tr("Decline"), this);
  m_allow = new QPushButton(tr("Allow once"), this);
  m_allow->setObjectName("allowDisplayRequest");
  buttons->addWidget(m_reject);
  buttons->addWidget(m_allow);
  layout->addLayout(buttons);
  parent->installEventFilter(this);
  connect(m_allow, &QPushButton::clicked, this, [this] { respond(true); });
  connect(m_reject, &QPushButton::clicked, this, [this] { respond(false); });
  hide();
#ifdef DESKFLOW_DISPLAY
  auto service = deskflow::display::DisplayService::applicationService();
  connect(service, &deskflow::display::DisplayService::snapshotChanged, this, [this](const QJsonObject &s) {
    updateRequest(s);
  });
  connect(service, &deskflow::display::DisplayService::commandFinished, this, [this](const QJsonObject &r) {
    if (r.value("id").toString() != m_pending)
      return;
    m_pending.clear();
    updateRequest(r.value("snapshot").toObject());
    if (!r.value("accepted").toBool())
      m_status->setText(PeerUsageModel::reasonText(r.value("reasonCode").toString()));
  });
  updateRequest(service->snapshot());
#endif
}
void DisplayRequestPanel::place()
{
  const int width = qMin(380, parentWidget()->width() - 32);
  setFixedWidth(width);
  adjustSize();
  move(parentWidget()->width() - width - 24, 76);
  raise();
}
bool DisplayRequestPanel::eventFilter(QObject *watched, QEvent *event)
{
  if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible())
    place();
  return QWidget::eventFilter(watched, event);
}
void DisplayRequestPanel::updateRequest(const QJsonObject &snapshot)
{
  m_snapshot = snapshot;
  const bool incoming = snapshot.value("localDisplayRole") == "receiver" &&
                        snapshot.value("availableActions").toArray().contains("Allow");
  setVisible(incoming);
  if (!incoming)
    return;
  m_peer->setText(tr("%1 wants to use this screen.").arg(snapshot.value("peerName").toString()));
  m_allow->setEnabled(m_pending.isEmpty());
  m_reject->setEnabled(m_pending.isEmpty());
  place();
}
void DisplayRequestPanel::respond(bool allow)
{
#ifdef DESKFLOW_DISPLAY
  if (!m_pending.isEmpty() || !m_snapshot.value("availableActions").toArray().contains("Allow"))
    return;
  QJsonObject args;
  for (const auto &key : {"sessionId", "requestId", "generation", "displayEpoch"})
    args.insert(key, m_snapshot.value(key));
  args.insert("allow", allow);
  m_pending = QUuid::createUuid().toString(QUuid::Id128);
  m_allow->setEnabled(false);
  m_reject->setEnabled(false);
  deskflow::display::DisplayService::applicationService()->execute(
      {{"id", m_pending}, {"operation", "respondToDisplayRequest"}, {"args", args}}
  );
#else
  Q_UNUSED(allow);
#endif
}
