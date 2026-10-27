// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "DisplayRequestPanel.h"
#include "gui/core/PeerUsageModel.h"
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QUuid>
#include <QVBoxLayout>
#ifdef DESKFLOW_DISPLAY
#include "display/DisplayService.h"
#include "display/PeerUsageService.h"
#include "display/ScreenInventoryService.h"
#endif
DisplayRequestPanel::DisplayRequestPanel(QWidget *parent) : QFrame(parent)
{
  setObjectName("displayRequestPanel");
  setStyleSheet(R"(
    QFrame#displayRequestPanel {background:white;border:1px solid #d7e5f7;border-radius:12px;}
    QFrame#displayRequestPanel QLabel {background:transparent;border:none;color:#536174;font-size:13px;}
    QFrame#displayRequestPanel QPushButton {border:1px solid #dce4ef;border-radius:7px;padding:9px 16px;background:white;color:#536174;}
    QFrame#displayRequestPanel QPushButton#allowDisplayRequest {background:#087fff;color:white;border-color:#087fff;}
    QFrame#displayRequestPanel QPushButton:disabled {background:#e9eef5;color:#99a4b1;}
  )");
  auto layout = new QVBoxLayout(this);
  layout->setContentsMargins(20, 18, 20, 18);
  layout->setSpacing(12);
  m_title = new QLabel(this);
  m_title->setObjectName("displayRequestTitle");
  m_title->setTextFormat(Qt::PlainText);
  m_title->setStyleSheet("font-size:14px;font-weight:500;color:#303640;");
  layout->addWidget(m_title);
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
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  for (auto label : {m_title, m_peer, m_status})
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  m_title->setWordWrap(true);
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
  connect(
      deskflow::display::PeerUsageService::applicationService(), &deskflow::display::PeerUsageService::commandFinished,
      this,
      [this, service](const QJsonObject &reply) {
        if (reply.value("id") != m_pending)
          return;
        m_pending.clear();
        m_errorText=reply.value("accepted").toBool()?QString():PeerUsageModel::reasonText(reply.value("reasonCode").toString());
        updateRequest(service->snapshot());
      }
  );
  connect(PeerUsageModel::applicationModel(), &PeerUsageModel::changed, this, [this, service] {
    updateRequest(service->snapshot());
  });
  updateRequest(service->snapshot());
#endif
}
QString DisplayRequestPanel::peerForDevice(const QString &device)
{
  static const QRegularExpression identity("^[0-9a-f]{64}$");
  if (device.startsWith("peer:") && identity.match(device.mid(5)).hasMatch())
    return device.mid(5);
  const auto peer = PeerUsageModel::applicationModel()->snapshot(device).value("peerId").toString();
  if (identity.match(peer).hasMatch())
    return peer;
#ifdef DESKFLOW_DISPLAY
  // Cached identity is used only to place cleanup UI; it never authorizes a request.
  return deskflow::display::ScreenInventoryService::applicationService()->forDevice(device).value("peerId").toString();
#else
  return {};
#endif
}
bool DisplayRequestPanel::needsAttention(const QJsonObject &snapshot)
{
  return snapshot.value("state") == "CleanupRequired" ||
         (snapshot.value("localDisplayRole") == "receiver" &&
          snapshot.value("availableActions").toArray().contains("Allow"));
}
void DisplayRequestPanel::setDevice(const QString &device)
{
  if(m_device!=device)m_errorText.clear();
  m_device = device;
#ifdef DESKFLOW_DISPLAY
  updateRequest(deskflow::display::DisplayService::applicationService()->snapshot());
#endif
}
void DisplayRequestPanel::updateRequest(const QJsonObject &snapshot)
{
  m_snapshot = snapshot;
  const bool incoming = snapshot.value("localDisplayRole") == "receiver" &&
                        snapshot.value("availableActions").toArray().contains("Allow");
  const bool cleanup = snapshot.value("state") == "CleanupRequired";
  const auto peer = peerForDevice(m_device);
  const bool matches = !peer.isEmpty() && peer == snapshot.value("peerId").toString();
  setVisible(matches && (incoming || cleanup));
  m_reject->setVisible(!cleanup);
  m_allow->setText(cleanup ? tr("Retry cleanup") : tr("Allow once"));
  if (!matches || (!incoming && !cleanup))
    return;
  m_title->setText(cleanup ? tr("Extended display needs attention") : tr("Screen use request"));
  m_peer->setText(
      cleanup ? tr("Display resources need cleanup.")
              : tr("%1 wants to use this screen.").arg(snapshot.value("peerName").toString())
  );
  m_status->setText(
      cleanup ? tr("Verify cleanup before starting another use.")
              : tr("Allow this computer's screen to extend the other desktop for this session?")
  );
  if(!m_errorText.isEmpty())m_status->setText(m_errorText);
  m_allow->setEnabled(m_pending.isEmpty());
  m_reject->setEnabled(m_pending.isEmpty());
}
void DisplayRequestPanel::respond(bool allow)
{
#ifdef DESKFLOW_DISPLAY
  if (m_snapshot.value("state") == "CleanupRequired" && allow && m_pending.isEmpty()) {
    m_errorText.clear();
    auto service = deskflow::display::PeerUsageService::applicationService();
    const auto peer = m_snapshot.value("peerId").toString();
    const auto usage = service->snapshot(peer);
    m_pending = QUuid::createUuid().toString(QUuid::Id128);
    m_allow->setEnabled(false);
    service->execute(
        {{"id", m_pending},
         {"operation", "retryPeerUsageCleanup"},
         {"args",
          QJsonObject{
              {"peerId", peer},
              {"operationId", QUuid::createUuid().toString(QUuid::Id128)},
              {"baseConnectionId", usage.value("baseConnectionId")},
              {"baseConnectionGeneration", usage.value("baseConnectionGeneration")}
          }}}
    );
    return;
  }
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
