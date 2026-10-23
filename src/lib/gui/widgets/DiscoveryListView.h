// SPDX-License-Identifier: MIT
#pragma once
#include <QAccessibleWidget>
#include <QListView>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QStyledItemDelegate>
#include <functional>

// Expose real row widgets instead of Qt's synthetic accessible table cells.
// Cocoa can retain synthetic rows after an asynchronous model update.
class DiscoveryListView : public QListView
{
public:
  explicit DiscoveryListView(QWidget *parent = nullptr) : QListView(parent)
  {
    static const bool installed = [] {
      QAccessible::installFactory([](const QString &, QObject *object) -> QAccessibleInterface * {
        if (auto *list = qobject_cast<QListView *>(object); list && list->property("liveBluetoothDevices").toBool())
          return new QAccessibleWidget(list, QAccessible::List);
        return nullptr;
      });
      return true;
    }();
    Q_UNUSED(installed);
    setProperty("liveBluetoothDevices", true);
    if (auto *accessible = QAccessible::queryAccessibleInterface(this); accessible && accessible->tableInterface())
      QAccessible::deleteAccessibleInterface(QAccessible::uniqueId(accessible));
    setItemDelegate(new RowDelegate(this));
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    viewport()->setAutoFillBackground(false);
    setSpacing(2);
    setStyleSheet(
        "QListView { background: transparent; border: none; outline: 0; padding: 6px; }"
        "QScrollBar:vertical { background: transparent; width: 8px; margin: 5px 1px; }"
        "QScrollBar::handle:vertical { background: #d9dfe8; border-radius: 3px; min-height: 24px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
    );
  }
  std::function<void(const QModelIndex &)> selected;
  void refreshRows()
  {
    if (!model() || m_refreshing)
      return;
    QScopedValueRollback<bool> guard(m_refreshing, true);
    for (int row = 0; row < model()->rowCount(); ++row) {
      const auto index = model()->index(row, 0);
      auto *button = qobject_cast<QPushButton *>(indexWidget(index));
      if (!button) {
        button = new QPushButton(viewport());
        button->setAutoDefault(false);
        button->setCheckable(true);
        button->setStyleSheet(
            "QPushButton { text-align: left; padding: 7px 12px; border: 1px solid transparent;"
            " border-radius: 7px; background: #ffffff; color: #555e69; font-size: 13px; }"
            "QPushButton:hover { background: #f3f7fd; }"
            "QPushButton:checked { background: #eaf3ff; color: #087fff; }"
            "QPushButton:focus { border-color: #a8ccff; }"
        );
        const QPersistentModelIndex persistent(index);
        connect(button, &QPushButton::clicked, this, [this, persistent] {
          if (persistent.isValid() && selected)
            selected(persistent);
        });
        setIndexWidget(index, button);
      }
      const auto label = index.data().toString();
      button->setChecked(index == currentIndex());
      button->setAccessibleName(label);
      button->setToolTip(index.data(Qt::ToolTipRole).toString());
      button->setText(button->fontMetrics().elidedText(label, Qt::ElideRight, qMax(30, viewport()->width() - 24)));
    }
  }

protected:
  void currentChanged(const QModelIndex &current, const QModelIndex &previous) override
  {
    QListView::currentChanged(current, previous);
    refreshRows();
  }
  void resizeEvent(QResizeEvent *event) override
  {
    QListView::resizeEvent(event);
    refreshRows();
  }

private:
  bool m_refreshing = false;
  class RowDelegate : public QStyledItemDelegate
  {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *, const QStyleOptionViewItem &, const QModelIndex &) const override
    {
      // Actual buttons paint every row; avoid a second native selection frame behind them.
    }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
      auto size = QStyledItemDelegate::sizeHint(option, index);
      size.setHeight(qMax(40, size.height()));
      return size;
    }
  };
};
