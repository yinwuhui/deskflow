/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "HomeModeButton.h"
#ifdef Q_OS_MACOS
#include "gui/OSXHelpers.h"
#endif
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWidget>

class HomeModeSelector : public QWidget
{
public:
  explicit HomeModeSelector(QWidget *parent = nullptr)
      : QWidget(parent),
        m_indicator(new QWidget(this)),
        m_animation(new QPropertyAnimation(m_indicator, "geometry", this))
  {
    m_indicator->setObjectName("modeSelectionIndicator");
    m_indicator->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_indicator->setStyleSheet("background: #087fff; border: 1px solid #087fff; border-radius: 6px;");
    m_indicator->hide();
    m_animation->setObjectName("modeSelectionAnimation");
    m_animation->setDuration(200);
    m_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
      updateClips(value.toRect());
    });
  }

  void setButtons(HomeModeButton *left, HomeModeButton *right)
  {
    m_buttons = {left, right};
    for (auto button : m_buttons) {
      button->setSlidingSelection(true);
      button->installEventFilter(this);
      connect(button, &QAbstractButton::toggled, this, [this](bool checked) {
        if (checked)
          moveSelection(true);
      });
    }
    m_indicator->lower();
    moveSelection(false);
  }

protected:
  bool eventFilter(QObject *object, QEvent *event) override
  {
    if ((event->type() == QEvent::Move || event->type() == QEvent::Resize) &&
        (object == m_buttons.value(0) || object == m_buttons.value(1)))
      moveSelection(false);
    return QWidget::eventFilter(object, event);
  }

  void showEvent(QShowEvent *event) override
  {
    QWidget::showEvent(event);
    moveSelection(false);
  }

private:
  void moveSelection(bool animate)
  {
    for (auto button : m_buttons) {
      if (!button->isChecked())
        continue;
      const QRect destination(button->pos(), button->size());
      if (!animate && m_animation->state() == QAbstractAnimation::Running &&
          m_animation->endValue().toRect() == destination)
        return;
      bool canAnimate = animate && isVisible() && m_indicator->isVisible();
#ifdef Q_OS_MACOS
      canAnimate = canAnimate && !prefersOSXReducedMotion();
#endif
      m_animation->stop();
      if (canAnimate && m_indicator->geometry() != destination) {
        // Reversing midway starts at the current visual position, without jumping to an endpoint.
        m_animation->setStartValue(m_indicator->geometry());
        m_animation->setEndValue(destination);
        m_animation->start();
      } else {
        m_indicator->setGeometry(destination);
        updateClips(destination);
      }
      m_indicator->show();
      break;
    }
  }

  void updateClips(const QRect &selection)
  {
    for (auto button : m_buttons)
      button->setSelectionClip(selection.translated(-button->pos()));
  }

  QList<HomeModeButton *> m_buttons;
  QWidget *m_indicator;
  QPropertyAnimation *m_animation;
};
