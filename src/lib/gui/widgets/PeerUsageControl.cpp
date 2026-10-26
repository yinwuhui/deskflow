// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "PeerUsageControl.h"
#include "gui/core/PeerUsageModel.h"
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>

PeerUsageControl::PeerUsageControl(QWidget *parent, PeerUsageModel *model)
    : QWidget(parent),
      m_model(model ? model : PeerUsageModel::applicationModel())
{
  setObjectName("peerUsageControl");
  setStyleSheet(R"(
    QWidget#peerUsageControl {background:transparent;}
    QWidget#usageSegments {background:#f3f6fa;border:1px solid #e3e8ef;border-radius:9px;}
    QWidget#usageSelection {background:#087fff;border:none;border-radius:6px;}
    QWidget#peerUsageControl QPushButton {background:transparent;border:none;padding:0;color:#758293;font-size:13px;}
    QWidget#peerUsageControl QPushButton:checked {color:white;}
    QWidget#peerUsageControl QPushButton:disabled {color:#a0a8b4;}
    QWidget#peerUsageControl QPushButton:focus {border:1px solid #88bfff;border-radius:6px;}
    QWidget#peerUsageControl QPushButton#usageCancel, QWidget#peerUsageControl QPushButton#usageStop {color:#087fff;}
    QWidget#peerUsageControl QLabel {background:transparent;color:#8390a1;font-size:12px;}
  )");
  auto layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);
  layout->addWidget(new QLabel(tr("Usage mode"), this));
  m_segments = new QWidget(this);
  m_segments->setObjectName("usageSegments");
  m_segments->setFixedHeight(42);
  m_segments->setMaximumWidth(330);
  m_selection = new QWidget(m_segments);
  m_selection->setObjectName("usageSelection");
  m_selection->setAttribute(Qt::WA_TransparentForMouseEvents);
  auto row = new QHBoxLayout(m_segments);
  row->setContentsMargins(4, 4, 4, 4);
  row->setSpacing(0);
  m_input = new QPushButton(tr("Keyboard & mouse"), m_segments);
  m_input->setObjectName("usageInput");
  m_extended = new QPushButton(tr("Extended display"), m_segments);
  m_extended->setObjectName("usageExtended");
  for (auto button : {m_input, m_extended}) {
    button->setCheckable(true);
    button->setAutoExclusive(true);
    button->setFixedHeight(34);
    button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    row->addWidget(button, 1);
    button->raise();
  }
  m_animation = new QPropertyAnimation(m_selection, "geometry", this);
  m_animation->setDuration(180);
  m_animation->setEasingCurve(QEasingCurve::OutCubic);
  layout->addWidget(m_segments);
  auto statusRow = new QHBoxLayout;
  statusRow->setSpacing(8);
  m_status = new QLabel(this);
  m_status->setObjectName("usageStatus");
  m_status->setWordWrap(true);
  m_status->setMinimumHeight(36);
  m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  statusRow->addWidget(m_status, 1);
  m_cancel = new QPushButton(tr("Cancel"), this);
  m_cancel->setObjectName("usageCancel");
  m_stop = new QPushButton(tr("End extension"), this);
  m_stop->setObjectName("usageStop");
  m_retry = new QPushButton(tr("Retry cleanup"),this);m_retry->setObjectName("usageRetryCleanup");
  statusRow->addWidget(m_retry);
  statusRow->addWidget(m_cancel);
  statusRow->addWidget(m_stop);
  layout->addLayout(statusRow);
  connect(m_model, &PeerUsageModel::changed, this, &PeerUsageControl::refresh);
  connect(m_input, &QPushButton::clicked, this, [this] {
    m_model->choose(m_device, "input");
    refresh();
  });
  connect(m_extended, &QPushButton::clicked, this, [this] {
    m_model->choose(m_device, "extended");
    refresh();
  });
  connect(m_cancel, &QPushButton::clicked, this, [this] { m_model->cancel(m_device); });
  connect(m_stop, &QPushButton::clicked, this, [this] { m_model->stop(m_device); });
  connect(m_retry,&QPushButton::clicked,this,[this]{m_model->retryCleanup(m_device);});
  refresh();
}
void PeerUsageControl::setDevice(const QString &key)
{
  m_device = key;
  refresh();
}
void PeerUsageControl::resizeEvent(QResizeEvent *event)
{
  QWidget::resizeEvent(event);
  if (m_segments->layout())
    m_segments->layout()->activate();
  positionSelection(false);
}
void PeerUsageControl::positionSelection(bool animate)
{
  m_animation->stop();
  auto target = (m_extendedSelected ? m_extended : m_input)->geometry();
  if (animate && isVisible()) {
    m_animation->setStartValue(m_selection->geometry());
    m_animation->setEndValue(target);
    m_animation->start();
  } else
    m_selection->setGeometry(target);
}
void PeerUsageControl::refresh()
{
  const auto s = m_model->snapshot(m_device);
  const auto state = s.value("usageState").toString();
  const auto stage = s.value("switchStage").toString();
  const auto effective = s.value("effectiveMode").toString();
  const auto actions = s.value("availableActions").toArray();
  const auto caps = s.value("inputCapabilities").toObject();
  const bool waiting = m_model->pending(m_device);
  const bool switching = state == "Switching";
  const bool selected = (switching ? s.value("targetMode").toString() : effective) == "extended";
  const bool changed = selected != m_extendedSelected;
  m_extendedSelected = selected;
  m_input->setChecked(!selected);
  m_extended->setChecked(selected);
  const bool enabled = !waiting && !switching && (actions.contains("start") || actions.contains("switch"));
  m_input->setEnabled(enabled && caps.value("input") == "supported");
  m_extended->setEnabled(enabled && caps.value("extended") == "supported");
  m_selection->setVisible(!s.isEmpty());
  m_retry->setVisible(actions.contains("retryCleanup"));m_retry->setEnabled(!waiting);
  m_cancel->setVisible(actions.contains("cancel"));
  m_cancel->setEnabled(!waiting);
  m_stop->setVisible(effective == "extended" && actions.contains("stop") && !switching);
  m_stop->setEnabled(!waiting);
  QString text;
  if (!m_model->connected(m_device))
    text = tr("Connect this computer first.");
  else if (s.isEmpty())
    text = tr("Connected · usage capabilities are not yet available.");
  else if (waiting)
    text = tr("Submitting request…");
  else if (state == "CleanupRequired")
    text = tr("Resources need cleanup before switching again.");
  else if (switching) {
    if (stage == "AwaitingPermission")
      text = tr("Waiting for permission · current mode remains active.");
    else if (stage == "StartingDisplay")
      text = tr("Preparing extended display · waiting for the first frame…");
    else if (stage == "RestoringInput" || stage == "CheckingRollback")
      text = tr("Restoring the previous keyboard and mouse connection…");
    else
      text = tr("Switching mode…");
  } else if (!m_model->error(m_device).isEmpty())
    text = m_model->error(m_device);
  else if (effective == "extended")
    text = tr("Extended display active · drag local windows onto this screen.");
  else if (effective == "input")
    text = tr("Keyboard and mouse sharing · the other computer keeps its own desktop.");
  else
    text = tr("Connected · choose a supported usage mode.");
  const auto reason = s.value("reasonCode").toString();
  if (!switching && !reason.isEmpty() && reason != "None" && m_model->error(m_device).isEmpty())
    text = PeerUsageModel::reasonText(reason);
  m_status->setText(text);
  m_status->setToolTip(text);
  m_input->setToolTip(tr("Share both keyboard and mouse; preserve the existing control direction."));
  m_extended->setToolTip(
      caps.value("extended") == "supported"
          ? tr("Use this computer's screen to extend the local desktop. Requires permission on the other computer.")
          : tr("Extended display support and network availability have not been confirmed.")
  );
  positionSelection(changed);
}
