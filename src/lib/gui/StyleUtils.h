/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QDir>
#include <QFileInfoList>
#include <QFontDatabase>
#include <QIcon>
#include <QPalette>
#include <QStyleHints>
#include <QComboBox>
#include <QAbstractItemView>

#include "common/Constants.h"
#include "widgets/HomeComboBox.h"

namespace deskflow::gui {

// The custom application pages are light even when the desktop uses a dark theme.
// Supply every native control role, rather than combining a white surface with
// dark-theme buttons, popup lists, spin arrows or selection colors.
inline QPalette lightControlPalette()
{
  QPalette palette;
  for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
    const bool disabled = group == QPalette::Disabled;
    const QColor text(disabled ? "#a5adb8" : "#292d33");
    palette.setColor(group, QPalette::Window, QColor("#ffffff"));
    palette.setColor(group, QPalette::WindowText, text);
    palette.setColor(group, QPalette::Base, QColor(disabled ? "#f3f5f8" : "#ffffff"));
    palette.setColor(group, QPalette::AlternateBase, QColor("#f7f8fa"));
    palette.setColor(group, QPalette::Text, text);
    palette.setColor(group, QPalette::Button, QColor("#f7f8fa"));
    palette.setColor(group, QPalette::ButtonText, text);
    palette.setColor(group, QPalette::Highlight, QColor(disabled ? "#a6c7ed" : "#087fff"));
    palette.setColor(group, QPalette::HighlightedText, QColor("#ffffff"));
    palette.setColor(group, QPalette::PlaceholderText, QColor("#89929f"));
    palette.setColor(group, QPalette::ToolTipBase, QColor("#ffffff"));
    palette.setColor(group, QPalette::ToolTipText, QColor("#292d33"));
    palette.setColor(group, QPalette::Light, QColor("#ffffff"));
    palette.setColor(group, QPalette::Midlight, QColor("#edf0f4"));
    palette.setColor(group, QPalette::Mid, QColor("#dce1e7"));
    palette.setColor(group, QPalette::Dark, QColor("#aab4c1"));
    palette.setColor(group, QPalette::Shadow, QColor("#737c8a"));
    palette.setColor(group, QPalette::Link, QColor("#087fff"));
    palette.setColor(group, QPalette::LinkVisited, QColor("#657cba"));
    palette.setColor(group, QPalette::BrightText, QColor("#ffffff"));
    palette.setColor(group, QPalette::Accent, QColor("#087fff"));
  }
  return palette;
}

inline QString lightScrollBarStyle()
{
  return QStringLiteral(
      "QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }"
      "QScrollBar:horizontal { background: transparent; height: 8px; margin: 0; }"
      "QScrollBar::handle:vertical { background: #c6cdd7; border-radius: 4px; min-height: 28px; }"
      "QScrollBar::handle:horizontal { background: #c6cdd7; border-radius: 4px; min-width: 28px; }"
      "QScrollBar::handle:hover { background: #aab4c1; }"
      "QScrollBar::handle:disabled { background: transparent; }"
      "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; background: transparent; }"
      "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; background: transparent; }"
      "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }"
  );
}

inline void setLightComboBoxPalette(QComboBox *combo)
{
  const auto palette = lightControlPalette();
  combo->setPalette(palette);
  // The popup is a separate window and does not inherit the page's palette.
  auto *popup = combo->view()->window();
  popup->setPalette(palette);
  popup->setObjectName("lightComboPopup");
  popup->setStyleSheet(QStringLiteral(
      "QWidget#lightComboPopup { background: #ffffff; color: #292d33; border: 1px solid #dce1e7; }"
  ));
  combo->view()->setPalette(palette);
  // Stylesheet polishing can replace the view's palette again after construction.
  // Pin the popup surface and selection colors in the view's own stylesheet too.
  combo->view()->setStyleSheet(
      combo->view()->styleSheet() + QStringLiteral(
          "QAbstractItemView { background: #ffffff; color: #292d33;"
          " selection-background-color: #087fff; selection-color: #ffffff; }"
      ) + lightScrollBarStyle()
  );
}

inline QComboBox *lightComboBox(QWidget *parent)
{
  auto *combo = new HomeComboBox(parent);
  setLightComboBoxPalette(combo);
  return combo;
}

/**
 * @brief Detects dark mode in a universal manner (all Qt versions).
 * Until better platform support is added, it's more reliable to use the old way (compare text and window lightness),
 * because the newer versions in Qt 6.5+ are not always correct and some return `UnknownScheme`.
 * https://www.qt.io/blog/dark-mode-on-windows-11-with-qt-6.5
 */
inline bool isDarkMode()
{
  const QPalette defaultPalette;
  const auto text = defaultPalette.color(QPalette::WindowText);
  const auto window = defaultPalette.color(QPalette::Window);
  return text.lightness() > window.lightness();
}
/**
 * @brief get a string for the iconMode
 * @returns "dark" or "light"
 */
inline QString iconMode()
{
  return isDarkMode() ? QStringLiteral("dark") : QStringLiteral("light");
}

inline void updateIconTheme()
{
  // Sets the fallback icon path and fallback theme
  const auto themeName = QStringLiteral("%1-%2").arg(kAppId, iconMode());
  if (QIcon::themeName().isEmpty() || QIcon::themeName().startsWith(kAppId))
    QIcon::setThemeName(themeName);
  else
    QIcon::setFallbackThemeName(themeName);
  QIcon::setFallbackSearchPaths({QStringLiteral(":/icons/%1").arg(themeName)});
}
} // namespace deskflow::gui

inline QFont fixedFont()
{
#if defined(Q_OS_WIN)
  QFont f({"Hack", "Liberation Mono", "Monospace", "Andale Mono"});
  f.setStyleHint(QFont::Monospace);
#else
  QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
#endif

#if defined(Q_OS_MACOS)
  f.setPointSize(12);
#endif
  return f;
}
