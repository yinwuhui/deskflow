/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QLabel>
#include <QResizeEvent>

// Keep a long network identity on one line without changing the stored computer name.
class HomeIdentityLabel : public QLabel
{
public:
  explicit HomeIdentityLabel(QWidget *parent = nullptr) : QLabel(parent)
  {
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  }

  void setIdentity(const QString &identity)
  {
    m_identity = identity;
    setToolTip(identity);
    setAccessibleName(identity);
    refresh();
  }

protected:
  void resizeEvent(QResizeEvent *event) override
  {
    QLabel::resizeEvent(event);
    refresh();
  }

  void changeEvent(QEvent *event) override
  {
    QLabel::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
      refresh();
  }

private:
  void refresh()
  {
    setText(fontMetrics().elidedText(m_identity, Qt::ElideRight, contentsRect().width()));
  }

  QString m_identity;
};
