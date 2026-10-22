/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QLineF>
#include <QMap>
#include <QRectF>
#include <QString>

namespace screenlayout {
using Rectangles = QMap<QString, QRectF>;
struct Link
{
  QString source;
  QString target;
  QString side;
  qreal sourceStart;
  qreal sourceEnd;
  qreal targetStart;
  qreal targetEnd;
  QLineF seam;
};
enum class Error
{
  None,
  InvalidRectangle,
  Overlap,
  Disconnected
};
QList<Link> links(const Rectangles &rectangles);
Error validate(const Rectangles &rectangles);
QString linkText(const Link &link);
} // namespace screenlayout
