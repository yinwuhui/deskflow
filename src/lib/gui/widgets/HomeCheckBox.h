/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QCheckBox>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>

// Retain Qt's checkbox semantics, keyboard handling and accessibility while
// drawing the light application's indicator independently of the native theme.
class HomeCheckBox : public QCheckBox
{
public:
  explicit HomeCheckBox(const QString &text, QWidget *parent = nullptr) : QCheckBox(text, parent)
  {
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QStringLiteral("font-size: 12px;"));
    setMinimumHeight(26);
  }

  QSize sizeHint() const override
  {
    return {fontMetrics().size(Qt::TextShowMnemonic, text()).width() + 30, qMax(26, fontMetrics().height() + 6)};
  }

  QSize minimumSizeHint() const override
  {
    return sizeHint();
  }

protected:
  bool hitButton(const QPoint &position) const override
  {
    return rect().contains(position);
  }

  void paintEvent(QPaintEvent *) override
  {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool selected = checkState() != Qt::Unchecked;
    const QColor accent(isEnabled() ? "#087fff" : "#a6c7ed");
    const QRect logicalBox(3, (height() - 16) / 2, 16, 16);
    const auto box = QRectF(QStyle::visualRect(layoutDirection(), rect(), logicalBox)).adjusted(.5, .5, -.5, -.5);
    painter.setPen(QPen(selected ? accent : QColor(isEnabled() ? "#aab4c1" : "#d4dbe4"), 1));
    painter.setBrush(selected ? accent : QColor(isEnabled() ? "#ffffff" : "#f3f5f8"));
    painter.drawRoundedRect(box, 3, 3);
    if (selected) {
      painter.setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      if (checkState() == Qt::PartiallyChecked) {
        painter.drawLine(box.center() + QPointF(-3.5, 0), box.center() + QPointF(3.5, 0));
      } else {
        QPainterPath tick;
        tick.moveTo(box.left() + 3, box.top() + 7.5);
        tick.lineTo(box.left() + 6.3, box.top() + 10.5);
        tick.lineTo(box.left() + 12, box.top() + 4.5);
        painter.drawPath(tick);
      }
    }
    if (hasFocus()) {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(QColor("#087fff"), 1));
      painter.drawRoundedRect(box.adjusted(-2, -2, 2, 2), 5, 5);
    }
    const QRect label = QStyle::visualRect(layoutDirection(), rect(), QRect(26, 0, qMax(0, width() - 26), height()));
    const int mnemonic = style()->styleHint(QStyle::SH_UnderlineShortcut, nullptr, this)
                             ? Qt::TextShowMnemonic : Qt::TextHideMnemonic;
    painter.setPen(QColor(isEnabled() ? "#555e69" : "#a5adb8"));
    painter.drawText(label, Qt::AlignVCenter | Qt::AlignLeading | Qt::TextSingleLine | mnemonic, text());
  }
};
