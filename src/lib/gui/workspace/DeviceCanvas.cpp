// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "DeviceCanvas.h"
#include "WorkspaceUi.h"
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QWheelEvent>
#include <climits>
#include <cmath>
namespace deskflow::gui::workspace {
namespace {
constexpr int columnSpacing = 124, rowSpacing = 94;
class DeviceItem : public QGraphicsItem
{
public:
  QString name, subtitle;
  bool local = false;
  QRectF boundingRect() const override
  {
    return {0, 0, 114, 85};
  }
  void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override
  {
    const auto c = colors(isDark());
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(QPen(isSelected() ? c.accent : c.border, isSelected() ? 1.5 : 1));
    p->setBrush(isSelected() ? c.selection : c.window);
    p->drawRoundedRect(boundingRect().adjusted(1, 1, -1, -1), 7, 7);
    icon(local ? "laptop" : "monitor", isSelected() ? c.selectionText : c.text, 18).paint(p, QRect(48, 11, 18, 18));
    QFont f = p->font();
    f.setPixelSize(12);
    f.setWeight(QFont::Medium);
    p->setFont(f);
    p->setPen(isSelected() ? c.selectionText : c.text);
    p->drawText(QRectF(7, 36, 100, 19), Qt::AlignCenter, QFontMetrics(f).elidedText(name, Qt::ElideMiddle, 100));
    f.setPixelSize(11);
    f.setWeight(QFont::Normal);
    p->setFont(f);
    p->setPen(isSelected() ? c.selectionText : c.muted);
    p->drawText(QRectF(4, 60, 106, 17), Qt::AlignCenter, subtitle);
    if (hasFocus()) {
      p->setPen(QPen(c.accent, 2, Qt::DashLine));
      p->setBrush(Qt::NoBrush);
      p->drawRoundedRect(boundingRect().adjusted(4, 4, -4, -4), 5, 5);
    }
  }
};
} // namespace
DeviceCanvas::DeviceCanvas(QWidget *parent) : QGraphicsView(parent)
{
  setObjectName("deviceCanvas");
  setAccessibleName(tr("完整电脑排列。方向键选择，Alt加方向键移动，Enter编辑。"));
  setScene(new QGraphicsScene(this));
  setFrameShape(QFrame::NoFrame);
  setMinimumHeight(280);
  setMaximumHeight(362);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  setRenderHint(QPainter::Antialiasing);
  setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
  setTransformationAnchor(AnchorUnderMouse);
  setResizeAnchor(AnchorViewCenter);
  setFocusPolicy(Qt::StrongFocus);
  connect(scene(), &QGraphicsScene::selectionChanged, this, [this] {
    const auto selection = scene()->selectedItems();
    if (selection.isEmpty())
      return;
    m_selectedId = selection.first()->data(0).toString();
    Q_EMIT selected(m_selectedId);
  });
}
void DeviceCanvas::setDevices(const ComputerList &computers, int columns, const QString &selectedId, bool editable)
{
  const auto center = mapToScene(viewport()->rect().center());
  const bool restoreFocus = hasFocus();
  const QSignalBlocker blocked(scene());
  scene()->clear();
  m_selectedId = selectedId;
  m_editable = editable;
  int localColumn = 0, localRow = 0;
  for (int i = 0; i < computers.size(); ++i)
    if (computers[i].isServer()) {
      localColumn = i % columns;
      localRow = i / columns;
      break;
    }
  for (int i = 0; i < computers.size(); ++i) {
    const auto &computer = computers[i];
    if (computer.isNull())
      continue;
    auto *item = new DeviceItem;
    item->name = computer.displayName();
    item->local = computer.isServer();
    item->subtitle =
        item->local ? tr("键盘与鼠标") : tr("列 %1，行 %2").arg(i % columns - localColumn).arg(i / columns - localRow);
    item->setData(0, computer.stableId());
    item->setData(1, i % columns);
    item->setData(2, i / columns);
    item->setToolTip(
        computer.displayName().toHtmlEscaped() + "<br>" + computer.name().toHtmlEscaped() + "<br>" +
        item->subtitle.toHtmlEscaped()
    );
    item->setPos((i % columns) * columnSpacing, (i / columns) * rowSpacing);
    item->setFlags(
        QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemIsFocusable |
        (editable ? QGraphicsItem::ItemIsMovable : QGraphicsItem::GraphicsItemFlag(0))
    );
    scene()->addItem(item);
    item->setSelected(computer.stableId() == selectedId);
    if (restoreFocus && computer.stableId() == selectedId)
      item->setFocus();
  }
  scene()->setSceneRect(
      QRectF(-200, -150, columns * columnSpacing + 400, (computers.size() / std::max(1, columns)) * rowSpacing + 300)
  );
  setDragMode(editable ? NoDrag : ScrollHandDrag);
  centerOn(center);
  viewport()->update();
}
void DeviceCanvas::selectDevice(const QString &id)
{
  const QSignalBlocker block(scene());
  m_selectedId = id;
  for (auto *item : scene()->items())
    item->setSelected(item->data(0).toString() == id);
  viewport()->update();
}
void DeviceCanvas::centerSelected()
{
  for (auto *item : scene()->items())
    if (item->data(0).toString() == m_selectedId) {
      centerOn(item);
      return;
    }
}
void DeviceCanvas::setZoom(int percent)
{
  m_zoom = std::clamp(percent, 50, 175);
  QTransform transform;
  transform.scale(m_zoom / 100.0, m_zoom / 100.0);
  setTransform(transform);
  Q_EMIT zoomChanged(m_zoom);
}
void DeviceCanvas::pan(int x, int y)
{
  horizontalScrollBar()->setValue(horizontalScrollBar()->value() + x);
  verticalScrollBar()->setValue(verticalScrollBar()->value() + y);
}
void DeviceCanvas::drawBackground(QPainter *p, const QRectF &rect)
{
  const auto c = colors(isDark());
  p->fillRect(rect, c.panel);
  p->setPen(QPen(c.border, 1));
  const auto left = std::floor(rect.left() / 20) * 20, top = std::floor(rect.top() / 20) * 20;
  for (qreal x = left; x < rect.right(); x += 20)
    for (qreal y = top; y < rect.bottom(); y += 20)
      p->drawPoint(QPointF(x, y));
}
void DeviceCanvas::mouseReleaseEvent(QMouseEvent *event)
{
  QGraphicsView::mouseReleaseEvent(event);
  if (!m_editable || event->button() != Qt::LeftButton)
    return;
  const auto selected = scene()->selectedItems();
  if (selected.isEmpty())
    return;
  auto *item = selected.first();
  const auto x = qRound(item->pos().x() / columnSpacing), y = qRound(item->pos().y() / rowSpacing);
  const auto fromX = item->data(1).toInt(), fromY = item->data(2).toInt();
  item->setPos(fromX * columnSpacing, fromY * rowSpacing);
  if (fromX != x || fromY != y)
    Q_EMIT moved(fromX, fromY, x, y);
}
void DeviceCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
  QGraphicsView::mouseDoubleClickEvent(event);
  if (!scene()->selectedItems().isEmpty())
    Q_EMIT editRequested();
}
void DeviceCanvas::keyPressEvent(QKeyEvent *event)
{
  const auto selected = scene()->selectedItems();
  if (!selected.isEmpty()) {
    auto *item = selected.first();
    int x = item->data(1).toInt(), y = item->data(2).toInt(), dx = 0, dy = 0;
    if (event->key() == Qt::Key_Left)
      dx = -1;
    else if (event->key() == Qt::Key_Right)
      dx = 1;
    else if (event->key() == Qt::Key_Up)
      dy = -1;
    else if (event->key() == Qt::Key_Down)
      dy = 1;
    else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
      Q_EMIT editRequested();
      return;
    }
    if ((dx || dy) && event->modifiers().testFlag(Qt::AltModifier) && m_editable) {
      Q_EMIT moved(x, y, x + dx, y + dy);
      event->accept();
      return;
    }
    if ((dx || dy) && !event->modifiers().testFlag(Qt::AltModifier)) {
      QGraphicsItem *target = nullptr;
      int best = INT_MAX;
      for (auto *candidate : scene()->items()) {
        const int cx = candidate->data(1).toInt() - x, cy = candidate->data(2).toInt() - y;
        const int along = cx * dx + cy * dy;
        if (along <= 0)
          continue;
        const int across = std::abs(cx * dy - cy * dx);
        const int score = across * 1000 + along;
        if (score < best) {
          best = score;
          target = candidate;
        }
      }
      if (target) {
        scene()->clearSelection();
        target->setSelected(true);
        target->setFocus();
        ensureVisible(target);
      }
      event->accept();
      return;
    }
  }
  QGraphicsView::keyPressEvent(event);
}
void DeviceCanvas::wheelEvent(QWheelEvent *event)
{
  if (event->modifiers().testFlag(Qt::ControlModifier) || event->modifiers().testFlag(Qt::MetaModifier)) {
    setZoom(m_zoom + (event->angleDelta().y() > 0 ? 10 : -10));
    event->accept();
    return;
  }
  QGraphicsView::wheelEvent(event);
}
} // namespace deskflow::gui::workspace
