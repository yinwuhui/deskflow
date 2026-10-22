/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QIcon>

namespace deskflow::gui {

// Raster variants are generated from the checked-in SVG, keeping branding usable
// even on installations without Qt's optional SVG image plugin.
inline QIcon brandIcon()
{
  QIcon icon;
  for (const int size : {32, 64, 128, 256, 512, 1024})
    icon.addFile(QStringLiteral(":/brand/deskflow-%1.png").arg(size), QSize(size, size));
  return icon;
}

inline QIcon brandTrayIcon(bool light = false, bool mask = false)
{
  QIcon icon(
      light ? QStringLiteral(":/brand/deskflow-symbolic-light.png")
            : QStringLiteral(":/brand/deskflow-symbolic-dark.png")
  );
  icon.setIsMask(mask);
  return icon;
}

} // namespace deskflow::gui
