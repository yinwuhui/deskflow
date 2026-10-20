/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "KeySequenceWidget.h"

#include <QMouseEvent>

KeySequenceWidget::KeySequenceWidget(QWidget *parent, const KeySequence &seq)
    : QPushButton(parent),
      m_KeySequence(seq),
      m_BackupSequence(seq)
{
  setFocusPolicy(Qt::StrongFocus);
  setAccessibleName(tr("录制键盘或鼠标组合"));
  updateOutput();
}

void KeySequenceWidget::setKeySequence(const KeySequence &seq)
{
  m_KeySequence = seq;
  m_BackupSequence = seq;

  setStatus(Stopped);
  updateOutput();
}

void KeySequenceWidget::mousePressEvent(QMouseEvent *event)
{
  event->accept();

  if (status() == Stopped) {
    startRecording();
    return;
  }

  if (m_KeySequence.appendMouseButton(event->button()))
    stopRecording();

  updateOutput();
}

void KeySequenceWidget::startRecording()
{
  m_BackupSequence = m_KeySequence;
  m_KeySequence = KeySequence();
  setDown(true);
  setFocus();
  grabKeyboard();
  setStatus(Recording);
  updateOutput();
}

void KeySequenceWidget::stopRecording()
{
  if (!keySequence().valid()) {
    m_KeySequence = backupSequence();
    updateOutput();
  }

  setDown(false);
  setStatus(Stopped);
  releaseKeyboard();
  focusNextChild();
  updateOutput();
  Q_EMIT keySequenceChanged();
}

void KeySequenceWidget::cancelRecording()
{
  if (status() != Recording)
    return;
  m_KeySequence = m_BackupSequence;
  setStatus(Stopped);
  setDown(false);
  releaseKeyboard();
  updateOutput();
}

bool KeySequenceWidget::event(QEvent *event)
{
  if (status() == Recording) {
    switch (event->type()) {
    case QEvent::KeyPress:
      keyPressEvent(static_cast<QKeyEvent *>(event));
      return true;

    case QEvent::MouseButtonRelease:
    case QEvent::ShortcutOverride:
      event->accept();
      return true;

    case QEvent::FocusOut:
      stopRecording();
      if (!valid()) {
        m_KeySequence = backupSequence();
        updateOutput();
      }
      break;

    default:
      break;
    }
  }

  return QPushButton::event(event);
}

void KeySequenceWidget::keyPressEvent(QKeyEvent *event)
{
  event->accept();

  if (status() == Stopped) {
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
      startRecording();
    else
      QPushButton::keyPressEvent(event);
    return;
  }

  if (m_KeySequence.appendKey(event->key(), event->modifiers()))
    stopRecording();

  updateOutput();
}

void KeySequenceWidget::updateOutput()
{
  if (status() == Recording) {
    setText(tr("正在录制，请按键或点击鼠标…"));
    return;
  }
  if (m_KeySequence.toString().isEmpty()) {
    setText(tr("点击或按空格开始录制"));
    return;
  }
  QString s;

  if (m_KeySequence.isMouseButton())
    s = mousePrefix() + m_KeySequence.toString() + mousePostfix();
  else
    s = keyPrefix() + m_KeySequence.toString() + keyPostfix();

  setText(s);
}
