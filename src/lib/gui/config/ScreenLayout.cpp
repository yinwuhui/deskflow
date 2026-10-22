/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "ScreenLayout.h"
#include <QSet>
#include <cmath>

namespace screenlayout {
QList<Link> links(const Rectangles &rectangles)
{
  QList<Link> result;
  const auto names = rectangles.keys();
  for (const auto &source : names)
    for (const auto &target : names) {
      if (source == target)
        continue;
      const auto a = rectangles[source], b = rectangles[target];
      const auto y0 = std::max(a.top(), b.top()), y1 = std::min(a.bottom(), b.bottom());
      const auto x0 = std::max(a.left(), b.left()), x1 = std::min(a.right(), b.right());
      const auto add = [&](const QString &side, qreal start, qreal end, qreal aOrigin, qreal aLength, qreal bOrigin,
                           qreal bLength, QLineF seam) {
        result.append(
            {source, target, side, (start - aOrigin) / aLength, (end - aOrigin) / aLength, (start - bOrigin) / bLength,
             (end - bOrigin) / bLength, seam}
        );
      };
      if (y1 - y0 >= 1 && qAbs(a.right() - b.left()) < .01)
        add("right", y0, y1, a.top(), a.height(), b.top(), b.height(), QLineF(a.right(), y0, a.right(), y1));
      if (y1 - y0 >= 1 && qAbs(a.left() - b.right()) < .01)
        add("left", y0, y1, a.top(), a.height(), b.top(), b.height(), QLineF(a.left(), y0, a.left(), y1));
      if (x1 - x0 >= 1 && qAbs(a.bottom() - b.top()) < .01)
        add("down", x0, x1, a.left(), a.width(), b.left(), b.width(), QLineF(x0, a.bottom(), x1, a.bottom()));
      if (x1 - x0 >= 1 && qAbs(a.top() - b.bottom()) < .01)
        add("up", x0, x1, a.left(), a.width(), b.left(), b.width(), QLineF(x0, a.top(), x1, a.top()));
    }
  return result;
}

Error validate(const Rectangles &rectangles)
{
  for (auto it = rectangles.cbegin(); it != rectangles.cend(); ++it) {
    const auto r = it.value();
    if (it.key().isEmpty() || !std::isfinite(r.x()) || !std::isfinite(r.y()) || !std::isfinite(r.width()) ||
        !std::isfinite(r.height()) || qAbs(r.x()) > 1000000 || qAbs(r.y()) > 1000000 || r.width() < 80 ||
        r.height() < 80 || r.width() > 10000 || r.height() > 10000)
      return Error::InvalidRectangle;
    for (auto other = std::next(it); other != rectangles.cend(); ++other) {
      const auto overlap = r.intersected(other.value());
      if (overlap.width() > .01 && overlap.height() > .01)
        return Error::Overlap;
    }
  }
  if (rectangles.size() < 2)
    return Error::None;
  const auto edges = links(rectangles);
  QSet<QString> visited{rectangles.firstKey()};
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &link : edges)
      if (visited.contains(link.source) && !visited.contains(link.target)) {
        visited.insert(link.target);
        changed = true;
      }
  }
  return visited.size() == rectangles.size() ? Error::None : Error::Disconnected;
}

QString linkText(const Link &link)
{
  const auto percent = [](qreal value) { return QString::number(qBound(0.0, value, 1.0) * 100, 'f', 6); };
  return QStringLiteral("%1(%2,%3) = %4(%5,%6)")
      .arg(
          link.side, percent(link.sourceStart), percent(link.sourceEnd), link.target, percent(link.targetStart),
          percent(link.targetEnd)
      );
}
} // namespace screenlayout
