/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ComputerSetupModel.h"

#include "common/Constants.h"
#include "gui/config/Computer.h"

#include <QIODevice>
#include <QIcon>
#include <QMimeData>

const QString ComputerSetupModel::m_MimeType = "application/x-deskflow-computer";

ComputerSetupModel::ComputerSetupModel(ComputerList &computers, int numColumns, int numRows)
    : QAbstractTableModel(nullptr),
      m_computers(computers),
      m_NumColumns(numColumns),
      m_NumRows(numRows)
{
  // bound the grid so that multiplying columns by rows cannot overflow.
  if (m_NumColumns < 1 || m_NumColumns > kMaxGridSize || m_NumRows < 1 || m_NumRows > kMaxGridSize) {
    qCritical("grid size out of bounds: %d columns x %d rows", m_NumColumns, m_NumRows);
    m_NumColumns = kServerGridWidth;
    m_NumRows = kServerGridHeight;
  }

  const int span = m_NumColumns * m_NumRows;
  if (span > m_computers.size()) {
    qCritical(
        "computer list too small for grid, computers: %lld, cells: %d", static_cast<long long>(m_computers.size()), span
    );
    m_computers.resize(span);
  }
}

QVariant ComputerSetupModel::data(const QModelIndex &index, int role) const
{
  if (!index.isValid() || index.row() >= m_NumRows || index.row() < 0 || index.column() < 0 ||
      index.column() >= m_NumColumns)
    return QVariant();

  if (computer(index).isNull())
    return QVariant();

  switch (role) {
  case Qt::DecorationRole:
    return computer(index).pixmap();

  case Qt::ToolTipRole:
    return QString(tr("<center>Computer: <b>%1</b></center>"
                      "<br>Double click to edit settings"
                      "<br>Drag computer to the trashcan to remove it"))
        .arg(computer(index).name());

  case Qt::DisplayRole:
    return computer(index).name();

  default:
    break;
  }
  return QVariant();
}

Qt::ItemFlags ComputerSetupModel::flags(const QModelIndex &index) const
{
  if (!index.isValid() || index.row() >= m_NumRows || index.column() >= m_NumColumns)
    return Qt::NoItemFlags;

  if (!computer(index).isNull())
    return Qt::ItemIsEnabled | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled;

  return Qt::ItemIsDropEnabled;
}

Qt::DropActions ComputerSetupModel::supportedDropActions() const
{
  return Qt::MoveAction | Qt::CopyAction;
}

QStringList ComputerSetupModel::mimeTypes() const
{
  return QStringList() << m_MimeType;
}

QMimeData *ComputerSetupModel::mimeData(const QModelIndexList &indexes) const
{
  auto *pMimeData = new QMimeData();
  QByteArray encodedData;

  QDataStream stream(&encodedData, QIODevice::WriteOnly);

  for (const QModelIndex &index : indexes) {
    if (index.isValid())
      stream << index.column() << index.row() << computer(index);
  }

  pMimeData->setData(m_MimeType, encodedData);

  return pMimeData;
}

bool ComputerSetupModel::dropMimeData(
    const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent
)
{
  if (action == Qt::IgnoreAction)
    return true;

  if (!data->hasFormat(m_MimeType))
    return false;

  if (!parent.isValid() || parent.row() >= m_NumRows || parent.column() >= m_NumColumns || row != -1 || column != -1)
    return false;

  QByteArray encodedData = data->data(m_MimeType);
  QDataStream stream(&encodedData, QIODevice::ReadOnly);

  int sourceColumn = -1;
  int sourceRow = -1;

  stream >> sourceColumn;
  stream >> sourceRow;

  const auto pColumn = parent.column();
  const auto pRow = parent.row();

  // don't drop computer onto itself
  if (sourceColumn == pColumn && sourceRow == pRow)
    return false;

  Computer droppedComputer;
  stream >> droppedComputer;
  if (stream.status() != QDataStream::Ok || sourceColumn < -1 || sourceRow < -1 || sourceColumn >= m_NumColumns ||
      sourceRow >= m_NumRows)
    return false;

  if (auto oldComputer = Computer(computer(pColumn, pRow));
      !oldComputer.isNull() && sourceColumn != -1 && sourceRow != -1) {
    // mark the computer so it isn't deleted after the dragndrop succeeded
    // see ComputerSetupView::startDrag()
    oldComputer.setSwapped(true);
    computer(sourceColumn, sourceRow) = oldComputer;
  }

  computer(pColumn, pRow) = droppedComputer;

  Q_EMIT computersChanged();

  return true;
}

void ComputerSetupModel::addComputer(const Computer &newComputer)
{
  m_computers.addComputerByPriority(newComputer);
  Q_EMIT computersChanged();
}

bool ComputerSetupModel::isFull() const
{
  auto emptyComputer = std::ranges::find_if(m_computers, [](const Computer &item) { return item.isNull(); });
  return (static_cast<QList<Computer>::const_iterator>(emptyComputer) == m_computers.cend());
}

bool ComputerSetupModel::moveComputer(int column, int row, int targetColumn, int targetRow)
{
  if (column < 0 || row < 0 || targetColumn < 0 || targetRow < 0 || column >= m_NumColumns || row >= m_NumRows ||
      targetColumn >= m_NumColumns || targetRow >= m_NumRows || computer(column, row).isNull())
    return false;
  std::swap(computer(column, row), computer(targetColumn, targetRow));
  Q_EMIT dataChanged(index(row, column), index(row, column));
  Q_EMIT dataChanged(index(targetRow, targetColumn), index(targetRow, targetColumn));
  Q_EMIT computersChanged();
  return true;
}
bool ComputerSetupModel::resizeGrid(int columns, int rows)
{
  if (columns < 1 || rows < 1 || columns > kMaxGridSize || rows > kMaxGridSize)
    return false;
  for (int row = 0; row < m_NumRows; ++row)
    for (int column = 0; column < m_NumColumns; ++column)
      if ((column >= columns || row >= rows) && !computer(column, row).isNull())
        return false;
  ComputerList resized(columns);
  resized.resize(columns * rows);
  for (int row = 0; row < std::min(rows, m_NumRows); ++row)
    for (int column = 0; column < std::min(columns, m_NumColumns); ++column)
      resized[row * columns + column] = computer(column, row);
  beginResetModel();
  m_computers = resized;
  m_NumColumns = columns;
  m_NumRows = rows;
  endResetModel();
  Q_EMIT computersChanged();
  return true;
}

void ComputerSetupModel::resetGrid(int columns, int rows)
{
  beginResetModel();
  m_NumColumns = std::clamp(columns, 1, 100);
  m_NumRows = std::clamp(rows, 1, 100);
  if (m_computers.size() < m_NumColumns * m_NumRows)
    m_computers.resize(m_NumColumns * m_NumRows);
  endResetModel();
  Q_EMIT computersChanged();
}
