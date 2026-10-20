/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "LogWidget.h"

#include <gui/Logger.h>
#include <gui/StyleUtils.h>

#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QVBoxLayout>

LogWidget::LogWidget(QWidget *parent, QTextDocument *shared) : QWidget{parent}, m_textLog{new QPlainTextEdit(this)}
{
  if (shared)
    m_textLog->setDocument(shared);
  m_textLog->setReadOnly(true);
  m_textLog->setMaximumBlockCount(10000);
  m_textLog->setLineWrapMode(QPlainTextEdit::NoWrap);
  m_textLog->setFont(fixedFont());

  auto layout = new QVBoxLayout;
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(m_textLog);

  setLayout(layout);

  if (!shared)
    connect(deskflow::gui::Logger::instance(), &deskflow::gui::Logger::newLine, this, &LogWidget::appendLine);
}

QTextDocument *LogWidget::document() const
{
  return m_textLog->document();
}

void LogWidget::appendLine(const QString &msg)
{
  constexpr qsizetype lineLimit = 16384, documentLimit = 2 * 1024 * 1024;
  m_textLog->appendPlainText(msg.size() <= lineLimit ? msg : msg.left(lineLimit) + tr("…（此行过长，已截断）"));
  auto *doc = m_textLog->document();
  if (doc->characterCount() > documentLimit) {
    auto block = doc->findBlock(doc->characterCount() - documentLimit);
    if (block.isValid())
      block = block.next();
    QTextCursor cursor(doc);
    cursor.setPosition(block.isValid() ? block.position() : doc->characterCount() - documentLimit);
    cursor.setPosition(0, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
  }
}

void LogWidget::findNext(const QString &text)
{
  Q_EMIT searchResult(countMatches(text));
  if (text.isEmpty())
    return;

  if (!m_textLog->find(text)) {
    m_textLog->moveCursor(QTextCursor::Start);
    m_textLog->find(text);
  }
}

void LogWidget::findPrevious(const QString &text)
{
  Q_EMIT searchResult(countMatches(text));
  if (text.isEmpty())
    return;

  if (!m_textLog->find(text, QTextDocument::FindBackward)) {
    m_textLog->moveCursor(QTextCursor::End);
    m_textLog->find(text, QTextDocument::FindBackward);
  }
}

void LogWidget::scrollToBottom() const
{
  auto sb = m_textLog->verticalScrollBar();
  sb->setValue(sb->maximum());
}

int LogWidget::countMatches(const QString &text) const
{
  if (text.isEmpty())
    return 0;
  int matches = 0;
  QTextCursor cursor(m_textLog->document());
  while (!(cursor = m_textLog->document()->find(text, cursor)).isNull())
    ++matches;
  return matches;
}
