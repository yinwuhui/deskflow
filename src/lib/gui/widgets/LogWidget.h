/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>
#include <QWidget>

class QPlainTextEdit;
class QTemporaryFile;
class QTextDocument;

class LogWidget : public QWidget
{
  Q_OBJECT
public:
  explicit LogWidget(QWidget *parent = nullptr, QTextDocument *document = nullptr);
  QTextDocument *document() const;
  void appendLine(const QString &msg);
  void findNext(const QString &text);
  void findPrevious(const QString &text);
  void scrollToBottom() const;

Q_SIGNALS:
  void searchResult(int matches);

private:
  int countMatches(const QString &text) const;
  QPlainTextEdit *m_textLog = nullptr;
};
