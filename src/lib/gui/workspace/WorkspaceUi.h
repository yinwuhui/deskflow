// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QCheckBox>
#include <QColor>
#include <QFrame>
#include <QIcon>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QVBoxLayout>

namespace deskflow::gui::workspace {
struct Colors
{
  QColor window, panel, sidebar, text, muted, border, accent, onAccent, selection, warning, warningBackground, error,
      selectionText;
};
Colors contrastColors(const QPalette &palette);
Colors colors(bool dark);
bool isHighContrast();
bool isDark();
void applyTheme(QWidget *root);
QIcon icon(const QString &name, const QColor &color, int size = 20);
QLabel *label(const QString &text, const char *kind = "body", QWidget *parent = nullptr);
void setStatusText(QLabel *target, const QString &text);
QPushButton *button(const QString &text, bool primary = false, QWidget *parent = nullptr);
QFrame *panel(QWidget *parent = nullptr);
QWidget *row(const QString &title, const QString &description, QWidget *control, QWidget *parent = nullptr);
QWidget *page(const QString &title, const QString &description, QWidget *action = nullptr);
QVBoxLayout *contents(QWidget *page);
QWidget *scroll(QWidget *page);
class Switch : public QCheckBox
{
public:
  explicit Switch(QWidget *parent = nullptr) : QCheckBox(parent)
  {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
  }
  QSize sizeHint() const override
  {
    return {40, 32};
  }

protected:
  bool hitButton(const QPoint &point) const override
  {
    return rect().contains(point);
  }
  void paintEvent(QPaintEvent *) override;
};
class Navigation : public QWidget
{
  Q_OBJECT
public:
  explicit Navigation(QWidget *parent = nullptr);
  int currentRow() const
  {
    return m_current;
  }
  void setCurrentRow(int row);
  void refreshIcons();
Q_SIGNALS:
  void currentRowChanged(int row);

private:
  int m_current = -1;
  QList<QPushButton *> m_buttons;
};
} // namespace deskflow::gui::workspace
