// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "WorkspaceUi.h"
#include <QAccessible>
#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QStyle>
#include <QStyleHints>
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include <QAccessibilityHints>
#endif
#ifdef Q_OS_MACOS
#include "gui/OSXHelpers.h"
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#endif
namespace deskflow::gui::workspace {
bool isHighContrast()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
  if (qApp->styleHints()->accessibility()->contrastPreference() == Qt::ContrastPreference::HighContrast)
    return true;
#endif
#ifdef Q_OS_MACOS
  return isOSXHighContrast();
#elif defined(Q_OS_WIN)
  HIGHCONTRAST settings{};
  settings.cbSize = sizeof(settings);
  return SystemParametersInfo(SPI_GETHIGHCONTRAST, sizeof(settings), &settings, 0) &&
         (settings.dwFlags & HCF_HIGHCONTRASTON);
#else
  return false;
#endif
}
Colors contrastColors(const QPalette &palette)
{
  // Keep foreground/background pairs supplied by the system together.
  const auto text = palette.color(QPalette::WindowText);
  return {
      palette.color(QPalette::Window),
      palette.color(QPalette::Base),
      palette.color(QPalette::Window),
      text,
      text,
      text,
      palette.color(QPalette::Highlight),
      palette.color(QPalette::HighlightedText),
      palette.color(QPalette::Highlight),
      text,
      palette.color(QPalette::Window),
      text,
      palette.color(QPalette::HighlightedText)
  };
}
Colors colors(bool dark)
{
  if (isHighContrast())
    return contrastColors(qApp->palette());
  if (dark)
    return {"#21252B", "#2B3038", "#282D34", "#EDF1F8", "#AEB8C7", "#454F5E", "#85B8FF",
            "#16253A", "#2B415F", "#ECC17D", "#433724", "#F0AAA3", "#EDF1F8"};
  return {"#F7F8FA", "#FFFFFF", "#EEF0F4", "#27303B", "#626E7F", "#DCE2EA", "#0867D5",
          "#FFFFFF", "#E7EFFD", "#88550D", "#FFF4DF", "#B93C36", "#27303B"};
}
bool isDark()
{
  const auto scheme = qApp->styleHints()->colorScheme();
  return scheme == Qt::ColorScheme::Dark || (qApp->palette().color(QPalette::Window).lightness() < 128);
}
void applyTheme(QWidget *root)
{
  const auto c = colors(isDark());
  auto palette = root->palette();
  palette.setColor(QPalette::Window, c.window);
  palette.setColor(QPalette::Base, c.panel);
  palette.setColor(QPalette::AlternateBase, c.sidebar);
  palette.setColor(QPalette::WindowText, c.text);
  palette.setColor(QPalette::Text, c.text);
  palette.setColor(QPalette::Button, c.panel);
  palette.setColor(QPalette::ButtonText, c.text);
  palette.setColor(QPalette::Highlight, c.selection);
  palette.setColor(QPalette::HighlightedText, c.selectionText);
  palette.setColor(QPalette::PlaceholderText, c.muted);
  root->setPalette(palette);
  root->setAutoFillBackground(true);
  const QString style = QString(R"(
#workspaceRoot { background:%1; color:%4; font-size:14px; }
#workspaceRoot QWidget { color:%4; }
#workspaceRoot QLabel { background:transparent; }
#workspaceRoot QLabel[kind="title"] { font-size:23px; font-weight:600; }
#workspaceRoot QLabel[kind="subtitle"] { font-size:14px; font-weight:600; }
#workspaceRoot QLabel[kind="body"] { font-size:14px; }
#workspaceRoot QLabel[kind="muted"] { font-size:12px; color:%5; }
#workspaceRoot QLabel[kind="error"] { font-size:12px; color:%11; }
#workspaceRoot QFrame[panel="true"] { background:%2; border:1px solid %6; border-radius:8px; }
#workspaceRoot QWidget[sidebar="true"] { background:%3; border-right:1px solid %6; }
#workspaceRoot QFrame[separator="true"] { background:%6; border:0; min-height:1px; max-height:1px; }
#workspaceRoot QPushButton { background:%2; border:1px solid %6; border-radius:6px; padding:6px 11px; min-height:20px; font-size:12px; }
#workspaceRoot QPushButton:hover { background:%9; }
#workspaceRoot QPushButton:pressed { background:%3; }
#workspaceRoot QPushButton[primary="true"] { background:%7; border-color:%7; color:%8; font-weight:600; }
#workspaceRoot QPushButton[danger="true"] { color:%11; }
#workspaceRoot QPushButton[nav="true"] { text-align:left; background:transparent; border:0; padding:10px; font-size:13px; }
#workspaceRoot QPushButton[nav="true"]:checked { background:%2; font-weight:600; }
#workspaceRoot QPushButton[tab="true"] { border:0; background:transparent; }
#workspaceRoot QPushButton[tab="true"]:checked { background:%9; color:%7; }
#workspaceRoot QPushButton:disabled { color:%5; background:%1; }
#workspaceRoot QPushButton:focus, #workspaceRoot QLineEdit:focus, #workspaceRoot QComboBox:focus, #workspaceRoot QSpinBox:focus, #workspaceRoot QDoubleSpinBox:focus, #workspaceRoot QPlainTextEdit:focus { border:2px solid %7; }
#workspaceRoot QLineEdit, #workspaceRoot QComboBox, #workspaceRoot QSpinBox, #workspaceRoot QDoubleSpinBox { background:%2; border:1px solid %6; border-radius:6px; padding:6px 8px; min-height:20px; font-size:13px; selection-background-color:%9; selection-color:%12; }
#workspaceRoot QPlainTextEdit, #workspaceRoot QListWidget, #workspaceRoot QTreeWidget { background:%2; border:1px solid %6; border-radius:6px; padding:5px; }
#workspaceRoot QCheckBox { spacing:8px; background:transparent; }
#workspaceRoot QCheckBox::indicator { width:17px; height:17px; }
#workspaceRoot QScrollArea { border:0; background:transparent; }
#workspaceRoot QScrollArea QWidget { background:transparent; }
#workspaceRoot QScrollBar:vertical { background:transparent; border:0; width:10px; margin:2px; }
#workspaceRoot QScrollBar:horizontal { background:transparent; border:0; height:10px; margin:2px; }
#workspaceRoot QScrollBar::handle:vertical { background:%6; min-height:28px; border-radius:3px; }
#workspaceRoot QScrollBar::handle:horizontal { background:%6; min-width:28px; border-radius:3px; }
#workspaceRoot QScrollBar::handle:hover { background:%5; }
#workspaceRoot QScrollBar::add-line, #workspaceRoot QScrollBar::sub-line { border:0; background:transparent; width:0; height:0; }
#workspaceRoot QScrollBar::add-page, #workspaceRoot QScrollBar::sub-page { background:transparent; }
#workspaceRoot QWidget[warning="true"] { background:%10; border-radius:8px; }
)")
                            .arg(
                                c.window.name(), c.panel.name(), c.sidebar.name(), c.text.name(), c.muted.name(),
                                c.border.name(), c.accent.name(), c.onAccent.name(), c.selection.name(),
                                c.warningBackground.name(), c.error.name(), c.selectionText.name()
                            );
  root->setStyleSheet(
      style + (isHighContrast() ? QString(R"(
#workspaceRoot QFrame[panel="true"], #workspaceRoot QPushButton { border-width:2px; }
#workspaceRoot QPushButton[tab="true"]:checked { background:%1; color:%2; border:2px solid %3; }
#workspaceRoot QPushButton[nav="true"]:checked { border:2px solid %3; }
#workspaceRoot QPushButton:focus, #workspaceRoot QLineEdit:focus, #workspaceRoot QComboBox:focus, #workspaceRoot QSpinBox:focus, #workspaceRoot QDoubleSpinBox:focus, #workspaceRoot QPlainTextEdit:focus { border:3px solid %3; }
)")
                                      .arg(c.selection.name(), c.selectionText.name(), c.accent.name())
                                : QString())
  );
}
QIcon icon(const QString &name, const QColor &color, int size)
{
  QPixmap image(size * 2, size * 2);
  image.setDevicePixelRatio(2);
  image.fill(Qt::transparent);
  QPainter p(&image);
  p.setRenderHint(QPainter::Antialiasing);
  p.scale(size / 24.0, size / 24.0);
  p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p.setBrush(Qt::NoBrush);
  if (name == "settings") {
    for (const auto y : {6, 12, 18})
      p.drawLine(QPointF(3, y), QPointF(21, y));
    p.setBrush(colors(isDark()).sidebar);
    p.drawEllipse(QPointF(8, 6), 2, 2);
    p.drawEllipse(QPointF(16, 12), 2, 2);
    p.drawEllipse(QPointF(9, 18), 2, 2);
  } else if (name == "activity") {
    QPainterPath path;
    path.moveTo(2, 12);
    path.lineTo(6, 12);
    path.lineTo(9, 3);
    path.lineTo(14, 21);
    path.lineTo(17, 12);
    path.lineTo(22, 12);
    p.drawPath(path);
  } else if (name == "help") {
    p.drawEllipse(QRectF(3, 3, 18, 18));
    QPainterPath path;
    path.moveTo(9, 9);
    path.cubicTo(9, 5, 17, 6, 15, 10);
    path.cubicTo(14, 12, 12, 11, 12, 14);
    p.drawPath(path);
    p.drawPoint(QPointF(12, 17));
  } else if (name == "laptop") {
    p.drawRoundedRect(QRectF(5, 4, 14, 12), 1, 1);
    p.drawLine(2, 20, 22, 20);
    p.drawLine(5, 16, 2, 20);
    p.drawLine(19, 16, 22, 20);
  } else {
    p.drawRoundedRect(QRectF(3, 3, 18, 13), 1.5, 1.5);
    p.drawLine(12, 16, 12, 21);
    p.drawLine(8, 21, 16, 21);
  }
  return QIcon(image);
}
QLabel *label(const QString &text, const char *kind, QWidget *parent)
{
  auto *result = new QLabel(text, parent);
  result->setTextFormat(Qt::PlainText);
  result->setWordWrap(true);
  result->setProperty("kind", kind);
  result->setMinimumWidth(0);
  return result;
}
void setStatusText(QLabel *target, const QString &text)
{
  if (target->text() == text)
    return;
  target->setText(text);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  if (target->isVisible() && !text.isEmpty() && QAccessible::isActive()) {
    QAccessibleAnnouncementEvent event(target, text);
    QAccessible::updateAccessibility(&event);
  }
#endif
}
QPushButton *button(const QString &text, bool primary, QWidget *parent)
{
  auto *result = new QPushButton(text, parent);
  result->setProperty("primary", primary);
  result->setCursor(Qt::PointingHandCursor);
  result->setAutoDefault(false);
  result->setAccessibleName(text);
  return result;
}
QFrame *panel(QWidget *parent)
{
  auto *result = new QFrame(parent);
  result->setProperty("panel", true);
  return result;
}
QWidget *row(const QString &title, const QString &description, QWidget *control, QWidget *parent)
{
  auto *result = new QWidget(parent);
  auto *layout = new QHBoxLayout(result);
  layout->setContentsMargins(16, 14, 16, 14);
  layout->setSpacing(16);
  auto *text = new QVBoxLayout;
  text->setSpacing(4);
  text->addWidget(label(title));
  if (!description.isEmpty())
    text->addWidget(label(description, "muted"));
  layout->addLayout(text, 1);
  control->setAccessibleName(title);
  control->setAccessibleDescription(description);
  layout->addWidget(control, 0, Qt::AlignVCenter);
  return result;
}
QWidget *page(const QString &title, const QString &description, QWidget *action)
{
  auto *result = new QWidget;
  auto *layout = new QVBoxLayout(result);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(20);
  layout->setAlignment(Qt::AlignTop);
  auto *header = new QHBoxLayout;
  auto *text = new QVBoxLayout;
  text->setSpacing(6);
  text->addWidget(label(title, "title"));
  text->addWidget(label(description, "muted"));
  header->addLayout(text, 1);
  if (action)
    header->addWidget(action, 0, Qt::AlignVCenter);
  layout->addLayout(header);
  return result;
}
QVBoxLayout *contents(QWidget *page)
{
  return qobject_cast<QVBoxLayout *>(page->layout());
}
QWidget *scroll(QWidget *page)
{
  auto *result = new QScrollArea;
  result->setWidgetResizable(true);
  result->setFrameShape(QFrame::NoFrame);
  result->setWidget(page);
  result->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  return result;
}
void Switch::paintEvent(QPaintEvent *)
{
  const auto c = colors(isDark());
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  if (!isEnabled())
    p.setOpacity(.4);
  const QRectF track(3, (height() - 19) / 2.0, 34, 19);
  p.setPen(QPen(isChecked() ? c.accent : c.border, 1));
  p.setBrush(isChecked() ? c.accent : c.sidebar);
  p.drawRoundedRect(track, 9.5, 9.5);
  p.setPen(QPen(c.border, .5));
  p.setBrush(isHighContrast() ? (isChecked() ? c.onAccent : c.text) : QColor(Qt::white));
  const auto left = isChecked() ? track.right() - 17 : track.left() + 2;
  p.drawEllipse(QRectF(left, track.top() + 2, 15, 15));
  if (hasFocus()) {
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(c.accent, 2));
    p.drawRoundedRect(track.adjusted(-2, -2, 2, 2), 11, 11);
  }
}
Navigation::Navigation(QWidget *parent) : QWidget(parent)
{
  setAttribute(Qt::WA_StyledBackground, true);
  setProperty("sidebar", true);
  setFixedWidth(142);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 22, 10, 16);
  layout->setSpacing(6);
  const QStringList labels{tr("设备"), tr("设置"), tr("诊断"), tr("帮助")};
  for (int i = 0; i < labels.size(); ++i) {
    if (i == 3)
      layout->addStretch();
    auto *item = button(labels[i], false, this);
    item->setProperty("nav", true);
    item->setCheckable(true);
    item->setIconSize(QSize(18, 18));
    m_buttons.append(item);
    layout->addWidget(item);
    connect(item, &QPushButton::clicked, this, [this, i] { setCurrentRow(i); });
  }
  refreshIcons();
  setCurrentRow(0);
}
void Navigation::setCurrentRow(int row)
{
  if (row < 0 || row >= m_buttons.size() || row == m_current)
    return;
  m_current = row;
  for (int i = 0; i < m_buttons.size(); ++i)
    m_buttons[i]->setChecked(i == row);
  Q_EMIT currentRowChanged(row);
}
void Navigation::refreshIcons()
{
  const QStringList names{"monitor", "settings", "activity", "help"};
  for (int i = 0; i < m_buttons.size(); ++i)
    m_buttons[i]->setIcon(icon(names[i], colors(isDark()).text));
}
} // namespace deskflow::gui::workspace
