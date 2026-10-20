// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "gui/config/ComputerList.h"
#include <QGraphicsView>
namespace deskflow::gui::workspace {
class DeviceCanvas : public QGraphicsView
{
  Q_OBJECT
public:
  explicit DeviceCanvas(QWidget *parent = nullptr);
  void setDevices(const ComputerList &computers, int columns, const QString &selectedId, bool editable);
  void centerSelected();
  void selectDevice(const QString &id);
  QSize sizeHint() const override
  {
    return {640, 362};
  }
  void setZoom(int percent);
  void pan(int x, int y);
Q_SIGNALS:
  void zoomChanged(int percent);
  void selected(const QString &id);
  void moved(int fromColumn, int fromRow, int toColumn, int toRow);
  void editRequested();

protected:
  void drawBackground(QPainter *painter, const QRectF &rect) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;

private:
  QString m_selectedId;
  bool m_editable = false;
  int m_zoom = 100;
};
} // namespace deskflow::gui::workspace
