/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QPainter>
#include <QRadioButton>

class HomeModeButton : public QRadioButton
{
public:
  using QRadioButton::QRadioButton;
  QSize sizeHint() const override
  {
    return {fontMetrics().horizontalAdvance(text()) + 56, 42};
  }

protected:
  void paintEvent(QPaintEvent *) override
  {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (isChecked()) {
      p.setPen(hasFocus() ? QColor("#b8d8ff") : QColor("#edf0f4"));
      p.setBrush(Qt::white);
      p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 6, 6);
    }
    const auto color = isChecked() ? QColor("#087fff") : QColor("#747d89");
    const int textWidth = fontMetrics().horizontalAdvance(text());
    const int left = (width() - textWidth - 25) / 2;
    icon().paint(&p, QRect(left, (height() - 17) / 2, 17, 17));
    p.setPen(color);
    p.drawText(QRect(left + 25, 0, textWidth, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
  }
};
