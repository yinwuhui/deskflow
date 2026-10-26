// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QEnterEvent>
#include <QHoverEvent>
#include <QIcon>
#include <QMouseEvent>
#include <QObject>
#include <QPainter>
#include <QPushButton>
#include <QWidget>

// Keep the existing accessible buttons and actions. The hover area includes
// the gaps and disabled zoom button, like a macOS window-control group.
class MacWindowControls : public QObject
{
public:
  MacWindowControls(QWidget *group, QPushButton *close, QPushButton *minimize, QPushButton *zoom)
      : QObject(group),
        m_group(group),
        m_close(close),
        m_minimize(minimize),
        m_zoom(zoom),
        m_closeIcon(glyph(true)),
        m_minimizeIcon(glyph(false))
  {
    group->setAttribute(Qt::WA_Hover);
    group->setMouseTracking(true);
    group->installEventFilter(this);
    group->window()->installEventFilter(this);
    for (auto *button : {close, minimize, zoom}) {
      button->setAttribute(Qt::WA_Hover);
      button->setMouseTracking(true);
      button->installEventFilter(this);
    }
  }

protected:
  bool eventFilter(QObject *object, QEvent *event) override
  {
    auto *widget = qobject_cast<QWidget *>(object);
    if (object == m_group || object == m_close || object == m_minimize || object == m_zoom) {
      QPoint position;
      bool movement = true;
      switch (event->type()) {
      case QEvent::HoverEnter:
      case QEvent::HoverMove:
        position = static_cast<QHoverEvent *>(event)->position().toPoint();
        break;
      case QEvent::Enter:
        position = static_cast<QEnterEvent *>(event)->position().toPoint();
        break;
      case QEvent::MouseMove:
        position = static_cast<QMouseEvent *>(event)->position().toPoint();
        break;
      default:
        movement = false;
      }
      if (movement) {
        const auto bounds = m_close->geometry().united(m_minimize->geometry()).united(m_zoom->geometry());
        setHovered(bounds.adjusted(-2, -2, 2, 2).contains(widget->mapTo(m_group, position)));
      } else if (object == m_group && (event->type() == QEvent::HoverLeave || event->type() == QEvent::Hide)) {
        setHovered(false);
      } else if (event->type() == QEvent::EnabledChange)
        updateIcons();
    } else if (object == m_group->window() && event->type() == QEvent::WindowDeactivate)
      setHovered(false);
    return false; // Never consume mouse clicks, keyboard actions or window
                  // dragging.
  }

private:
  static QIcon glyph(bool close)
  {
    QIcon icon;
    for (int scale : {1, 2, 3}) {
      QPixmap pixels(7 * scale, 7 * scale);
      pixels.setDevicePixelRatio(scale);
      pixels.fill(Qt::transparent);
      QPainter painter(&pixels);
      painter.setRenderHint(QPainter::Antialiasing);
      painter.setPen(QPen(QColor(close ? "#7b211b" : "#865b08"), 1.05, Qt::SolidLine, Qt::RoundCap));
      if (close) {
        painter.drawLine(QPointF(1.5, 1.5), QPointF(5.5, 5.5));
        painter.drawLine(QPointF(5.5, 1.5), QPointF(1.5, 5.5));
      } else
        painter.drawLine(QPointF(1, 3.5), QPointF(6, 3.5));
      painter.end();
      icon.addPixmap(pixels);
    }
    return icon;
  }
  void setHovered(bool value)
  {
    if (m_hovered == value)
      return;
    m_hovered = value;
    updateIcons();
  }
  void updateIcons()
  {
    m_close->setIconSize(QSize(7, 7));
    m_minimize->setIconSize(QSize(7, 7));
    m_close->setIcon(m_hovered && m_close->isEnabled() ? m_closeIcon : QIcon{});
    m_minimize->setIcon(m_hovered && m_minimize->isEnabled() ? m_minimizeIcon : QIcon{});
  }
  QWidget *m_group;
  QPushButton *m_close, *m_minimize, *m_zoom;
  QIcon m_closeIcon, m_minimizeIcon;
  bool m_hovered = false;
};
