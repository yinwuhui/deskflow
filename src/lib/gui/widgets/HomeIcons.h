/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

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
  Bluetooth,
  BluetoothConnected,
  BluetoothDisabled,
  MonitorSelected,
  PanelSelected,
  LayoutSelected,
  SettingsSelected,
  Minimize,
  Close
};

// Render the bundled Remix alpha masks, preserving caller colors and high-DPI support.
// PNG masks keep the interface independent of Qt's optional SVG image plugin.
inline QIcon homeIcon(HomeIcon icon, const QColor &color = QColor("#77818e"))
{
  const char *name = nullptr;
  switch (icon) {
  case HomeIcon::Monitor:
    name = "computer-line";
    break;
  case HomeIcon::Devices:
    name = "device-line";
    break;
  case HomeIcon::Layout:
    name = "layout-grid-line";
    break;
  case HomeIcon::Settings:
    name = "settings-3-line";
    break;
  case HomeIcon::Keyboard:
    name = "keyboard-line";
    break;
  case HomeIcon::Copy:
    name = "file-copy-line";
    break;
  case HomeIcon::Edit:
    name = "edit-line";
    break;
  case HomeIcon::Help:
    name = "question-line";
    break;
  case HomeIcon::Logs:
    name = "file-list-line";
    break;
  case HomeIcon::Shield:
    name = "shield-check-line";
    break;
  case HomeIcon::Refresh:
    name = "refresh-line";
    break;
  case HomeIcon::Play:
    name = "play-circle-line";
    break;
  case HomeIcon::Stop:
    name = "stop-circle-line";
    break;
  case HomeIcon::Search:
    name = "search-line";
    break;
  case HomeIcon::Pointer:
    name = "cursor-line";
    break;
  case HomeIcon::ChevronRight:
    name = "arrow-right-s-line";
    break;
  case HomeIcon::ChevronDown:
    name = "arrow-down-s-line";
    break;
  case HomeIcon::Menu:
    name = "menu-line";
    break;
  case HomeIcon::MonitorUp:
    name = "arrow-up-circle-line";
    break;
  case HomeIcon::MonitorDown:
    name = "arrow-down-circle-line";
    break;
  case HomeIcon::Panel:
    name = "layout-left-line";
    break;
  case HomeIcon::Adjust:
    name = "equalizer-line";
    break;
  case HomeIcon::Wifi:
    name = "wifi-line";
    break;
  case HomeIcon::Bluetooth:
    name = "bluetooth-line";
    break;
  case HomeIcon::Minimize:
    name = "subtract-line";
    break;
  case HomeIcon::Close:
    name = "close-line";
    break;
  case HomeIcon::MonitorSelected:
    name = "computer-fill";
    break;
  case HomeIcon::PanelSelected:
    name = "layout-left-fill";
    break;
  case HomeIcon::LayoutSelected:
    name = "layout-grid-fill";
    break;
  case HomeIcon::SettingsSelected:
    name = "settings-3-fill";
    break;
  case HomeIcon::BluetoothConnected:
    name = "bluetooth-connect-line";
    break;
  case HomeIcon::BluetoothDisabled:
    name = "bluetooth-disabled-line";
    break;
  }
  if (!name)
    return {};
  const QPixmap mask(QStringLiteral(":/icons/remix/masks/%1.png").arg(QString::fromLatin1(name)));
  if (mask.isNull())
    return {};
  QIcon result;
  for (const int size : {16, 20, 24, 48}) {
    for (const int scale : {1, 2}) {
      QPixmap pixmap = mask.scaled(size * scale, size * scale, Qt::KeepAspectRatio, Qt::SmoothTransformation);
      QPainter painter(&pixmap);
      painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
      painter.fillRect(pixmap.rect(), color);
      painter.end();
      pixmap.setDevicePixelRatio(scale);
      result.addPixmap(pixmap);
    }
  }
  return result;
}

// Checkable buttons select QIcon::On automatically, including programmatic navigation.
inline QIcon homeNavigationIcon(HomeIcon outline, HomeIcon filled)
{
  const auto off = homeIcon(outline, QColor("#77818e"));
  const auto on = homeIcon(filled, QColor("#087fff"));
  QIcon result;
  for (const int size : {16, 20, 24, 48}) {
    for (const qreal scale : {1.0, 2.0}) {
      result.addPixmap(off.pixmap(QSize(size, size), scale), QIcon::Normal, QIcon::Off);
      result.addPixmap(on.pixmap(QSize(size, size), scale), QIcon::Normal, QIcon::On);
    }
  }
  return result;
}

} // namespace deskflow::gui
