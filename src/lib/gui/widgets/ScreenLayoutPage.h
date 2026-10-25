/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include "gui/config/ServerConfig.h"
#include <QWidget>

class QLabel;
class QPushButton;
class QSpinBox;
class QLineEdit;
class QComboBox;
class LayoutCanvas;
class PeerUsageControl;

class ScreenLayoutPage : public QWidget
{
  Q_OBJECT
public:
  explicit ScreenLayoutPage(ServerConfig &config, QWidget *parent = nullptr);
  void activate();
  bool isModified() const;
  bool save();
  void discard();
  bool addComputer(const QString &name);
  bool moveComputer(const QString &name, QPointF position);
  void setFreeMode(bool enabled);
  screenlayout::Rectangles rectangles() const;
  QRectF deviceVisualRect(const QString &name) const;
  void suggestComputer(const QString &name);
  void focusComputer(const QString &name);
Q_SIGNALS:
  void saved();

protected:
  void changeEvent(QEvent *event) override;

private:
  void build();
  void load();
  void ensureLocalComputer();
  void seedFreeRectangles();
  void refresh(bool fit = false);
  void selectComputer(const QString &name);
  void updateControls();
  void removeSelected();
  void updatePosition();
  void message(const QString &text, bool error = false);
  QString validationMessage() const;
  ServerConfig &m_config;
  ServerConfig m_baseline;
  ServerConfig m_liveBaseline;
  ServerConfig m_draft;
  PeerUsageControl *m_usage = nullptr;
  LayoutCanvas *m_canvas = nullptr;
  QPushButton *m_fixed = nullptr;
  QPushButton *m_free = nullptr;
  QPushButton *m_save = nullptr;
  QPushButton *m_discard = nullptr;
  QPushButton *m_remove = nullptr;
  QWidget *m_editor = nullptr;
  QLineEdit *m_newName = nullptr;
  QComboBox *m_selection = nullptr;
  QLabel *m_description = nullptr;
  QLabel *m_status = nullptr;
  QLabel *m_hint = nullptr;
  QLabel *m_external = nullptr;
  QSpinBox *m_x = nullptr;
  QSpinBox *m_y = nullptr;
  QSpinBox *m_width = nullptr;
  QSpinBox *m_height = nullptr;
  QString m_selected;
  bool m_loading = false;
};
