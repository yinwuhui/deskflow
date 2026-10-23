// SPDX-License-Identifier: MIT
#pragma once
#include <QCheckBox>
#include <QPainter>

// Native checkbox semantics and keyboard access with a compact switch presentation.
class HomeToggleSwitch : public QCheckBox
{
public:
  explicit HomeToggleSwitch(const QString &text, QWidget *parent = nullptr) : QCheckBox(text, parent)
  {
    setMinimumHeight(26);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  }
  QSize sizeHint() const override
  {
    return {fontMetrics().horizontalAdvance(text()) + 48, 26};
  }

protected:
  bool hitButton(const QPoint &point) const override
  {
    return rect().contains(point);
  }
  void paintEvent(QPaintEvent *) override
  {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled())
      painter.setOpacity(0.5);
    const bool rtl = layoutDirection() == Qt::RightToLeft;
    const QRectF track(rtl ? width() - 34 : 0, (height() - 20) / 2.0, 34, 20);
    painter.setPen(Qt::NoPen);
    painter.setBrush(isChecked() ? QColor("#087fff") : QColor("#aeb7c4"));
    painter.drawRoundedRect(track, 10, 10);
    painter.setBrush(Qt::white);
    const bool right = isChecked() != rtl;
    painter.drawEllipse(QRectF(track.x() + (right ? 17 : 3), track.y() + 3, 14, 14));
    painter.setPen(palette().color(QPalette::WindowText));
    const QRect textRect = rtl ? QRect(0, 0, width() - 44, height()) : QRect(44, 0, width() - 44, height());
    painter.drawText(textRect, Qt::AlignVCenter | (rtl ? Qt::AlignRight : Qt::AlignLeft), text());
    if (hasFocus()) {
      painter.setPen(QPen(QColor("#087fff"), 1));
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(track.adjusted(-1, -1, 1, 1), 11, 11);
    }
  }
};
