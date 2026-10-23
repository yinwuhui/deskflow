/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QWidget>

class LogWidget;
class QLabel;
class QPushButton;
class SearchWidget;
class QAction;

// Inline diagnostics, contained by the same shell and palette as the main pages.
class LogDock : public QWidget
{
  Q_OBJECT
public:
  explicit LogDock(QWidget *parent = nullptr);
  void appendLine(const QString &msg);
  QAction *toggleViewAction() const;

protected:
  void showEvent(QShowEvent *event) override;
  void hideEvent(QHideEvent *event) override;
  void changeEvent(QEvent *e) override;

private:
  LogWidget *m_textLog = nullptr;
  QPushButton *m_btnClose = nullptr;
  QAction *m_toggleAction = nullptr;
  QLabel *m_lblTitle = nullptr;
  SearchWidget *m_searchWidget = nullptr;
};
