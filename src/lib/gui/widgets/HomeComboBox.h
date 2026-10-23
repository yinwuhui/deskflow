/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QComboBox>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionComboBox>
#include <QStyle>

// Keep the native interaction, but make the dropdown affordance visible on every theme.
class HomeComboBox : public QComboBox
{
public:
  explicit HomeComboBox(QWidget *parent = nullptr) : QComboBox(parent)
  {
    setStyleSheet(QStringLiteral("QComboBox::down-arrow { image: none; }"));
  }

protected:
  void paintEvent(QPaintEvent *event) override
  {
    QComboBox::paintEvent(event);
    QStyleOptionComboBox option;
    initStyleOption(&option);
    const auto arrow = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, this);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(arrow.center());
    painter.setPen(QPen(QColor(isEnabled() ? "#77818e" : "#b0b8c2"), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPainterPath chevron;
    chevron.moveTo(-4, -2);
    chevron.lineTo(0, 2);
    chevron.lineTo(4, -2);
    painter.drawPath(chevron);
  }
};
