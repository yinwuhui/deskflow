/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ComputerSetupView.h"

#include "ComputerSetupModel.h"
#include "dialogs/ComputerSettingsDialog.h"

#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QResizeEvent>

ComputerSetupView::ComputerSetupView(QWidget *parent) : QTableView(parent)
{
  setDropIndicatorShown(true);
  setDragDropMode(DragDrop);
  setSelectionMode(SingleSelection);

  setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

  setIconSize(QSize(96, 96));
  horizontalHeader()->hide();
  verticalHeader()->hide();
}

void ComputerSetupView::setModel(QAbstractItemModel *model)
{
  QTableView::setModel(model);
  connect(model, &QAbstractItemModel::modelReset, this, &ComputerSetupView::setTableSize);
  setTableSize();
}

ComputerSetupModel *ComputerSetupView::model() const
{
  return qobject_cast<ComputerSetupModel *>(QTableView::model());
}

void ComputerSetupView::showComputerConfig(int col, int row)
{
  ComputerSettingsDialog dlg(this, &model()->computer(col, row), &model()->m_computers);
  dlg.exec();
  Q_EMIT model()->computersChanged();
}

void ComputerSetupView::setZoomPercent(int percent)
{
  m_zoomPercent = std::clamp(percent, 50, 200);
  setIconSize(QSize(96, 96) * m_zoomPercent / 100);
  setTableSize();
}

void ComputerSetupView::setTableSize()
{
  if (!model())
    return;
  const auto cell = QSize(128, 120) * m_zoomPercent / 100;
  horizontalHeader()->setDefaultSectionSize(cell.width());
  verticalHeader()->setDefaultSectionSize(cell.height());
  for (int i = 0; i < model()->columnCount(); ++i)
    setColumnWidth(i, cell.width());
  for (int i = 0; i < model()->rowCount(); ++i)
    setRowHeight(i, cell.height());
}

void ComputerSetupView::resizeEvent(QResizeEvent *event)
{
  setTableSize();
  event->ignore();
}

void ComputerSetupView::mouseDoubleClickEvent(QMouseEvent *event)
{
  if (event->buttons() & Qt::LeftButton) {
    int col = columnAt(event->pos().x());
    int row = rowAt(event->pos().y());

    if (col >= 0 && row >= 0 && !model()->computer(col, row).isNull()) {
      showComputerConfig(col, row);
    }
  } else
    event->ignore();
}

void ComputerSetupView::dragEnterEvent(QDragEnterEvent *event)
{
  // we accept anything that enters us by a drag as long as the
  // mime type is okay. anything else is dealt with in dragMoveEvent()
  if (event->mimeData()->hasFormat(ComputerSetupModel::mimeType()))
    event->accept();
  else
    event->ignore();
}

void ComputerSetupView::dragMoveEvent(QDragMoveEvent *event)
{
  if (event->mimeData()->hasFormat(ComputerSetupModel::mimeType())) {
    // where does the event come from? myself or someone else?
    if (event->source() == this) {
      // myself is ok, but then it must be a move action, never a copy
      event->setDropAction(Qt::MoveAction);
      event->accept();
    } else {
      const auto &point = event->position().toPoint();
      int col = columnAt(point.x());
      int row = rowAt(point.y());

      // a drop from outside is not allowed if there's a computer already there.
      if (col < 0 || row < 0 || !model()->computer(col, row).isNull())
        event->ignore();
      else {
        event->acceptProposedAction();
      }
    }
  } else
    event->ignore();
}

// this is reimplemented from QAbstractItemView::startDrag()
void ComputerSetupView::startDrag(Qt::DropActions)
{
  QModelIndexList indexes = selectedIndexes();

  if (indexes.count() != 1)
    return;

  QMimeData *pData = model()->mimeData(indexes);
  if (pData == nullptr)
    return;

  const QPixmap &pixmap = model()->computer(indexes[0]).pixmap();
  auto *pDrag = new QDrag(this);
  pDrag->setPixmap(pixmap);
  pDrag->setMimeData(pData);
  pDrag->setHotSpot(QPoint(iconSize().width() / 2, iconSize().height() / 2));

  if (pDrag->exec(Qt::MoveAction, Qt::MoveAction) == Qt::MoveAction) {
    selectionModel()->clear();

    // make sure to only delete the drag source if computerss weren't swapped
    // see ComputerSetupModel::dropMimeData
    if (!model()->computer(indexes[0]).swapped())
      model()->computer(indexes[0]) = Computer();
    else
      model()->computer(indexes[0]).setSwapped(false);

    Q_EMIT model()->computersChanged();
  }
}

void ComputerSetupView::initViewItemOption(QStyleOptionViewItem *option) const
{
  option->initFrom(this);
  option->decorationSize = iconSize();
  option->showDecorationSelected = true;
  option->decorationPosition = QStyleOptionViewItem::Top;
  option->decorationAlignment = Qt::AlignHCenter | Qt::AlignVCenter;
  option->displayAlignment = Qt::AlignTop | Qt::AlignHCenter;
  option->textElideMode = Qt::ElideMiddle;
}

void ComputerSetupView::keyPressEvent(QKeyEvent *event)
{
  auto selected = currentIndex();
  if (selected.isValid() && event->modifiers().testFlag(Qt::AltModifier)) {
    int column = selected.column(), row = selected.row();
    if (event->key() == Qt::Key_Left)
      --column;
    else if (event->key() == Qt::Key_Right)
      ++column;
    else if (event->key() == Qt::Key_Up)
      --row;
    else if (event->key() == Qt::Key_Down)
      ++row;
    else {
      QTableView::keyPressEvent(event);
      return;
    }
    if (model()->moveComputer(selected.column(), selected.row(), column, row)) {
      setCurrentIndex(model()->index(row, column));
      scrollTo(currentIndex(), EnsureVisible);
    }
    event->accept();
    return;
  }
  if (selected.isValid() && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
      !model()->computer(selected).isNull()) {
    showComputerConfig(selected.column(), selected.row());
    event->accept();
    return;
  }
  QTableView::keyPressEvent(event);
}
