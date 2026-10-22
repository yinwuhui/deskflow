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
  void setSlidingSelection(bool enabled)
  {
    m_slidingSelection = enabled;
    update();
  }
  void setSelectionClip(const QRect &selection)
  {
    m_selectionClip = selection;
    update();
  }
  QSize sizeHint() const override
  {
    return {fontMetrics().horizontalAdvance(text()) + 56, 42};
  }

protected:
  bool hitButton(const QPoint &position) const override
  {
    // The whole painted segment is interactive, not the native radio indicator/label rectangle.
    return rect().contains(position);
  }

  void paintEvent(QPaintEvent *) override
  {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (isChecked() && !m_slidingSelection) {
      p.setPen(hasFocus() ? QColor("#b8d8ff") : QColor("#edf0f4"));
      p.setBrush(Qt::white);
      p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 6, 6);
    }
    drawContent(p, !m_slidingSelection && isChecked() ? QColor("#087fff") : QColor("#747d89"));
    if (m_slidingSelection && m_selectionClip.intersects(rect())) {
      p.save();
      p.setClipRect(m_selectionClip);
      drawContent(p, Qt::white);
      p.restore();
    }
    if (m_slidingSelection && isChecked() && hasFocus()) {
      p.setBrush(Qt::NoBrush);
      p.setPen(QColor("#b8d8ff"));
      p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 6, 6);
    }
  }

private:
  void drawContent(QPainter &p, const QColor &color)
  {
    const int textWidth = fontMetrics().horizontalAdvance(text());
    const int left = (width() - textWidth - 25) / 2;
    auto glyph = icon().pixmap(17, 17);
    if (!glyph.isNull()) {
      QPainter tint(&glyph);
      tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
      tint.fillRect(glyph.rect(), color);
      tint.end();
      p.drawPixmap(QRect(left, (height() - 17) / 2, 17, 17), glyph);
    }
    p.setPen(color);
    p.drawText(QRect(left + 25, 0, textWidth, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
  }

  bool m_slidingSelection = false;
  QRect m_selectionClip;
};
