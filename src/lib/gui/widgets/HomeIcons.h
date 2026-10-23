/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QIcon>
#include <QPainter>
#include <QPainterPath>

namespace deskflow::gui {

enum class HomeIcon
{
  Monitor,
  Devices,
  Layout,
  Settings,
  Keyboard,
  Copy,
  Edit,
  Help,
  Logs,
  Shield,
  Refresh,
  Play,
  Stop,
  Search,
  Pointer,
  ChevronRight,
  ChevronDown,
  Menu,
  MonitorUp,
  MonitorDown,
  Panel,
  Adjust,
  Wifi,
  Bluetooth
};

// Small outline icons rendered by Qt itself, including on installations without the SVG image plugin.
inline QIcon homeIcon(HomeIcon icon, const QColor &color = QColor("#77818e"))
{
  QIcon result;
  for (const int size : {16, 20, 24, 48}) {
    QPixmap pixmap(size * 2, size * 2);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 24.0, size / 24.0);
    p.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    const auto line = [&p](qreal x1, qreal y1, qreal x2, qreal y2) { p.drawLine(QPointF(x1, y1), QPointF(x2, y2)); };
    switch (icon) {
    case HomeIcon::Wifi:
      p.drawArc(QRectF(2, 4, 20, 17), 35 * 16, 110 * 16);
      p.drawArc(QRectF(6, 9, 12, 10), 35 * 16, 110 * 16);
      p.drawEllipse(QPointF(12, 19), 0.8, 0.8);
      break;
    case HomeIcon::Bluetooth:
      line(12, 2, 12, 22);
      line(12, 2, 19, 8);
      line(19, 8, 5, 19);
      line(12, 22, 19, 16);
      line(19, 16, 5, 5);
      break;
    case HomeIcon::Panel:
      p.drawRoundedRect(QRectF(3, 3, 18, 18), 2, 2);
      line(8, 3, 8, 21);
      line(8, 9, 21, 9);
      break;
    case HomeIcon::Adjust:
      for (const auto &point : {QPointF(7, 5), QPointF(17, 12), QPointF(10, 19)}) {
        line(3, point.y(), point.x() - 2.5, point.y());
        line(point.x() + 2.5, point.y(), 21, point.y());
        p.drawEllipse(point, 2.5, 2.5);
      }
      break;
    case HomeIcon::Pointer:
      p.drawPolygon(QPolygonF{QPointF(4, 3), QPointF(21, 10), QPointF(13, 13), QPointF(10, 21)});
      break;
    case HomeIcon::ChevronDown:
      line(5, 9, 12, 16);
      line(12, 16, 19, 9);
      break;
    case HomeIcon::ChevronRight:
      line(9, 5, 16, 12);
      line(16, 12, 9, 19);
      break;
    case HomeIcon::Menu:
      for (const int y : {5, 12, 19})
        line(4, y, 20, y);
      break;
    case HomeIcon::MonitorUp:
    case HomeIcon::MonitorDown:
      p.drawRoundedRect(QRectF(3, 3, 18, 14), 2, 2);
      line(12, 17, 12, 21);
      line(8, 21, 16, 21);
      line(12, 6, 12, 13);
      if (icon == HomeIcon::MonitorUp) {
        line(8, 10, 12, 6);
        line(12, 6, 16, 10);
      } else {
        line(8, 9, 12, 13);
        line(12, 13, 16, 9);
      }
      break;
    case HomeIcon::Monitor:
      p.drawRoundedRect(QRectF(3, 4, 18, 13), 2, 2);
      line(12, 17, 12, 21);
      line(8, 21, 16, 21);
      break;
    case HomeIcon::Devices:
      p.drawRoundedRect(QRectF(2, 3, 14, 12), 2, 2);
      p.drawRoundedRect(QRectF(12, 9, 9, 12), 2, 2);
      line(6, 19, 9, 19);
      line(7, 15, 7, 19);
      break;
    case HomeIcon::Layout:
      for (const int x : {3, 14})
        for (const int y : {3, 14})
          p.drawRoundedRect(QRectF(x, y, 7, 7), 1.2, 1.2);
      break;
    case HomeIcon::Settings:
      p.drawEllipse(QPointF(12, 12), 6.4, 6.4);
      p.drawEllipse(QPointF(12, 12), 2.5, 2.5);
      for (int i = 0; i < 8; ++i) {
        p.save();
        p.translate(12, 12);
        p.rotate(i * 45);
        line(0, -7, 0, -10);
        p.restore();
      }
      break;
    case HomeIcon::Keyboard:
      p.drawRoundedRect(QRectF(2, 5, 20, 14), 2, 2);
      for (const int x : {6, 10, 14, 18}) {
        line(x, 9, x + .4, 9);
        line(x, 12, x + .4, 12);
      }
      line(7, 16, 17, 16);
      break;
    case HomeIcon::Copy:
      p.drawRoundedRect(QRectF(8, 8, 13, 13), 2, 2);
      line(15, 5, 15, 3);
      line(15, 3, 3, 3);
      line(3, 3, 3, 15);
      line(3, 15, 5, 15);
      break;
    case HomeIcon::Edit:
      p.drawPolygon(QPolygonF{QPointF(4, 20), QPointF(5, 14), QPointF(16, 3), QPointF(21, 8), QPointF(10, 19)});
      line(14, 5, 19, 10);
      break;
    case HomeIcon::Help:
      p.drawEllipse(QPointF(12, 12), 9, 9);
      p.drawArc(QRectF(9, 6, 6, 6), 0, 180 * 16);
      line(15, 9, 12, 13);
      line(12, 16, 12, 16.4);
      break;
    case HomeIcon::Logs:
      p.drawRoundedRect(QRectF(7, 3, 12, 17), 1.5, 1.5);
      p.drawRoundedRect(QRectF(3, 3, 4, 6), 1, 1);
      p.drawRoundedRect(QRectF(15, 17, 7, 4), 1, 1);
      for (const int y : {7, 11, 15})
        line(10, y, 16, y);
      break;
    case HomeIcon::Shield: {
      QPainterPath path;
      path.moveTo(12, 2);
      path.lineTo(21, 6);
      path.lineTo(20, 13);
      path.quadTo(19, 19, 12, 22);
      path.quadTo(5, 19, 4, 13);
      path.lineTo(3, 6);
      path.closeSubpath();
      p.drawPath(path);
      line(8, 12, 11, 15);
      line(11, 15, 16, 9);
      break;
    }
    case HomeIcon::Refresh:
      p.drawArc(QRectF(4, 4, 16, 16), 40 * 16, 285 * 16);
      line(20, 3, 20, 8);
      line(15, 8, 20, 8);
      break;
    case HomeIcon::Play:
      p.drawPolygon(QPolygonF{QPointF(7, 4), QPointF(20, 12), QPointF(7, 20)});
      break;
    case HomeIcon::Stop:
      p.drawRoundedRect(QRectF(5, 5, 14, 14), 2, 2);
      break;
    case HomeIcon::Search:
      p.drawEllipse(QPointF(10, 10), 6, 6);
      line(15, 15, 21, 21);
      break;
    }
    p.end();
    result.addPixmap(pixmap);
  }
  return result;
}

} // namespace deskflow::gui
