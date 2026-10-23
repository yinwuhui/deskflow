/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "LogDock.h"
#include "LogWidget.h"
#include "SearchWidget.h"

#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

LogDock::LogDock(QWidget *parent)
    : QWidget(parent),
      m_textLog{new LogWidget(this)},
      m_btnClose{new QPushButton(this)},
      m_toggleAction{new QAction(tr("Log"), this)},
      m_lblTitle{new QLabel(tr("Log"), this)},
      m_searchWidget{new SearchWidget(this)}
{
  setObjectName("logPanel");
  setAttribute(Qt::WA_StyledBackground);
  setFixedHeight(210);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setStyleSheet(QStringLiteral(
      "QWidget#logPanel { background: #f7f8fa; border-top: 1px solid #e6e9ee;"
      " border-bottom-right-radius: 16px; }"
      "QWidget#logPanel QLabel { color: #555e69; }"
      "QWidget#logPanel QPlainTextEdit { background: #ffffff; color: #292d33;"
      " border: 1px solid #e0e5ec; border-radius: 6px; padding: 8px;"
      " selection-background-color: #087fff; selection-color: #ffffff; font-size: 12px; }"
  ));
  m_toggleAction->setCheckable(true);
  connect(m_toggleAction, &QAction::toggled, this, &QWidget::setVisible);
  connect(m_searchWidget, &SearchWidget::findNext, m_textLog, &LogWidget::findNext);
  connect(m_searchWidget, &SearchWidget::findPrevious, m_textLog, &LogWidget::findPrevious);

  m_btnClose->setObjectName("btnCloseLog");
  m_btnClose->setFixedSize(26, 26);
  m_btnClose->setFlat(true);
  m_btnClose->setText(QStringLiteral("×"));
  m_btnClose->setToolTip(tr("Close Log"));
  m_btnClose->setAccessibleName(tr("Close Log"));
  connect(m_btnClose, &QPushButton::clicked, this, &QWidget::hide);

  auto titleLayout = new QHBoxLayout;
  titleLayout->addWidget(m_lblTitle);
  titleLayout->addStretch();
  titleLayout->addWidget(m_searchWidget);
  titleLayout->addWidget(m_btnClose);

  auto layout = new QVBoxLayout(this);
  layout->setContentsMargins(18, 8, 18, 16);
  layout->setSpacing(4);
  layout->addLayout(titleLayout);
  layout->addWidget(m_textLog, 1);
}

QAction *LogDock::toggleViewAction() const
{
  return m_toggleAction;
}

void LogDock::appendLine(const QString &msg)
{
  m_textLog->appendLine(msg);
  if (isVisible() && !window()->isMinimized() && !m_searchWidget->isExpanded())
    m_textLog->scrollToBottom();
}

void LogDock::showEvent(QShowEvent *event)
{
  QWidget::showEvent(event);
  m_toggleAction->setChecked(true);
}

void LogDock::hideEvent(QHideEvent *event)
{
  QWidget::hideEvent(event);
  // Hiding the application to the tray must not close its diagnostics panel.
  if (isHidden())
    m_toggleAction->setChecked(false);
}

void LogDock::changeEvent(QEvent *event)
{
  QWidget::changeEvent(event);
  if (event->type() != QEvent::LanguageChange)
    return;

  m_toggleAction->setText(tr("Log"));
  m_lblTitle->setText(tr("Log"));
  m_btnClose->setToolTip(tr("Close Log"));
  m_btnClose->setAccessibleName(tr("Close Log"));
}
