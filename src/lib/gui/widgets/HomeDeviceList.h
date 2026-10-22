/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include "HomeIcons.h"
#include <QCoreApplication>
#include <QListWidget>
#include <QPainter>
#include <QScopedValueRollback>
#include <QStyledItemDelegate>

class HomeDeviceList : public QListWidget
{
public:
  enum Role
  {
    Name = Qt::UserRole,
    Detail,
    Connected,
    Empty
  };
  explicit HomeDeviceList(QWidget *parent = nullptr) : QListWidget(parent)
  {
    setViewMode(QListView::IconMode);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setFlow(QListView::LeftToRight);
    setWrapping(true);
    setSpacing(0);
    setFrameShape(QFrame::NoFrame);
    setSelectionMode(QAbstractItemView::NoSelection);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setItemDelegate(new CardDelegate(this));
  }
  void fitCards()
  {
    if (m_fitting)
      return;
    QScopedValueRollback<bool> guard(m_fitting, true);
    const int columns = viewport()->width() >= 450 ? 2 : 1;
    setFixedHeight(qMax(72, ((count() + columns - 1) / columns) * 80 - 8));
    viewport()->update();
  }
  QRect visualRect(const QModelIndex &index) const override
  {
    if (!index.isValid())
      return {};
    const int columns = viewport()->width() >= 450 ? 2 : 1;
    const int cardWidth = (viewport()->width() - (columns - 1) * 12) / columns;
    return {index.row() % columns * (cardWidth + 12), index.row() / columns * 80, cardWidth, 72};
  }
  QModelIndex indexAt(const QPoint &point) const override
  {
    for (int i = 0; i < count(); ++i) {
      auto index = model()->index(i, 0);
      if (visualRect(index).contains(point))
        return index;
    }
    return {};
  }

protected:
  void paintEvent(QPaintEvent *) override
  {
    QPainter painter(viewport());
    QStyleOptionViewItem option;
    initViewItemOption(&option);
    for (int i = 0; i < count(); ++i) {
      const auto index = model()->index(i, 0);
      option.rect = visualRect(index);
      itemDelegate()->paint(&painter, option, index);
    }
  }
  void resizeEvent(QResizeEvent *event) override
  {
    QListWidget::resizeEvent(event);
    fitCards();
  }

private:
  bool m_fitting = false;
  class CardDelegate : public QStyledItemDelegate
  {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
      p->save();
      p->setRenderHint(QPainter::Antialiasing);
      auto box = QRectF(option.rect).adjusted(.5, .5, -.5, -.5);
      p->setPen(QColor("#e5e9ef"));
      p->setBrush(Qt::white);
      p->drawRoundedRect(box, 10, 10);
      if (index.data(Empty).toBool()) {
        p->setPen(QColor("#8b929c"));
        p->drawText(box.adjusted(16, 10, -16, -10), Qt::AlignCenter | Qt::TextWordWrap, index.data().toString());
        p->restore();
        return;
      }
      const QRectF iconBox(box.left() + 13, box.center().y() - 18, 36, 36);
      p->setPen(Qt::NoPen);
      p->setBrush(QColor("#eef5ff"));
      p->drawRoundedRect(iconBox, 7, 7);
      deskflow::gui::homeIcon(deskflow::gui::HomeIcon::Monitor, QColor("#338cec"))
          .paint(p, iconBox.adjusted(8, 8, -8, -8).toRect());
      const bool connected = index.data(Connected).toBool();
      const QString state = QCoreApplication::translate("MainWindow", connected ? "Connected" : "Not connected");
      auto font = option.font;
      font.setPixelSize(11);
      p->setFont(font);
      const int stateWidth = p->fontMetrics().horizontalAdvance(state) + 12;
      const qreal right = box.right() - 12;
      const qreal nameLeft = iconBox.right() + 11;
      const int nameWidth = qMax(30, int(right - stateWidth - nameLeft - 8));
      font.setPixelSize(13);
      font.setWeight(QFont::Medium);
      p->setFont(font);
      p->setPen(QColor("#292d33"));
      p->drawText(
          QRectF(nameLeft, box.center().y() - 22, nameWidth, 23), Qt::AlignVCenter,
          p->fontMetrics().elidedText(index.data(Name).toString(), Qt::ElideRight, nameWidth)
      );
      font.setPixelSize(11);
      font.setWeight(QFont::Normal);
      p->setFont(font);
      p->setPen(QColor("#8b929c"));
      p->drawText(
          QRectF(nameLeft, box.center().y() + 3, box.right() - nameLeft - 12, 20), Qt::AlignVCenter,
          p->fontMetrics().elidedText(index.data(Detail).toString(), Qt::ElideRight, box.right() - nameLeft - 12)
      );
      p->drawText(QRectF(right - stateWidth + 12, box.center().y() - 10, stateWidth, 20), Qt::AlignVCenter, state);
      p->setBrush(connected ? QColor("#32bb7b") : QColor("#c6cbd1"));
      p->setPen(Qt::NoPen);
      p->drawEllipse(QPointF(right - stateWidth + 4, box.center().y()), 3, 3);
      p->restore();
    }
  };
};
