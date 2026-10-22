/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "ScreenLayoutPage.h"
#include "HomeIcons.h"
#include "common/Settings.h"
#include "gui/validators/ComputerNameValidator.h"
#include <QButtonGroup>
#include <QComboBox>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>

class LayoutCanvas : public QWidget
{
public:
  explicit LayoutCanvas(QWidget *parent) : QWidget(parent)
  {
    setMinimumHeight(280);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(ScreenLayoutPage::tr("Screen arrangement canvas"));
  }
  screenlayout::Rectangles nodes;
  QString selected;
  QString local;
  bool freeMode = false;
  int columns = 5, rows = 5;
  std::function<void(const QString &)> onSelect;
  std::function<void(const QString &, QPointF, bool)> onMove;
  void fit()
  {
    m_world = {};
    for (const auto &r : nodes)
      m_world = m_world.isNull() ? r : m_world.united(r);
    if (m_world.isNull())
      m_world = QRectF(0, 0, 640, 400);
    m_world = m_world.adjusted(-180, -130, 180, 130);
    update();
  }
  QRectF visualRectangle(const QString &name) const
  {
    return toView(nodes.value(name));
  }

protected:
  void paintEvent(QPaintEvent *) override
  {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor("#e4e9f0"), 1));
    p.setBrush(QColor("#f7f9fc"));
    p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 12, 12);
    p.save();
    p.setClipRect(rect().adjusted(8, 8, -8, -8));
    if (!freeMode) {
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(QColor("#e2e8f0"), 1, Qt::DashLine));
      for (int y = 0; y < rows; ++y)
        for (int x = 0; x < columns; ++x)
          p.drawRoundedRect(toView(QRectF(x * 320, y * 200, 320, 200)).adjusted(4, 4, -4, -4), 7, 7);
    } else {
      p.setPen(QPen(QColor("#dce4ee"), 2));
      for (int x = 20; x < width(); x += 24)
        for (int y = 20; y < height(); y += 24)
          p.drawPoint(x, y);
    }
    int index = 0;
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it) {
      ++index;
      auto r = toView(it.value());
      if (!freeMode)
        r.adjust(7, 7, -7, -7);
      const bool active = it.key() == selected;
      p.setPen(QPen(active ? QColor("#087fff") : QColor("#cbd8e9"), active ? 2 : 1));
      p.setBrush(active ? QColor("#edf5ff") : QColor(Qt::white));
      p.drawRoundedRect(r, 9, 9);
      QFont f = font();
      f.setPixelSize(21);
      f.setWeight(QFont::Medium);
      p.setFont(f);
      p.setPen(QColor("#448ddd"));
      p.drawText(r.adjusted(8, 5, -8, -r.height() * .45), Qt::AlignCenter, QString::number(index));
      f.setPixelSize(12);
      f.setWeight(QFont::Normal);
      p.setFont(f);
      p.setPen(QColor("#36465b"));
      const auto name = p.fontMetrics().elidedText(it.key(), Qt::ElideRight, std::max(20, int(r.width() - 14)));
      p.drawText(r.adjusted(7, r.height() * .4, -7, -10), Qt::AlignCenter, name);
      if (it.key() == local && r.width() > 95 && r.height() > 70) {
        f.setPixelSize(10);
        p.setFont(f);
        p.setPen(QColor("#8795a7"));
        p.drawText(r.adjusted(5, 0, -5, -4), Qt::AlignHCenter | Qt::AlignBottom, ScreenLayoutPage::tr("This computer"));
      }
    }
    if (freeMode) {
      p.setPen(QPen(QColor("#2bb983"), 4, Qt::SolidLine, Qt::RoundCap));
      for (const auto &link : screenlayout::links(nodes)) {
        auto r = QRectF(link.seam.p1(), link.seam.p2());
        const auto a = toView(QRectF(link.seam.p1(), QSizeF(0, 0))).topLeft();
        const auto b = toView(QRectF(link.seam.p2(), QSizeF(0, 0))).topLeft();
        p.drawLine(a, b);
      }
    }
    p.restore();
  }
  void resizeEvent(QResizeEvent *e) override
  {
    QWidget::resizeEvent(e);
    update();
  }
  void mousePressEvent(QMouseEvent *e) override
  {
    if (e->button() != Qt::LeftButton || !isEnabled())
      return;
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it)
      if (toView(it.value()).contains(e->position())) {
        selected = it.key();
        m_drag = selected;
        m_press = e->position();
        m_origin = it.value().topLeft();
        if (onSelect)
          onSelect(selected);
        update();
        return;
      }
  }
  void mouseMoveEvent(QMouseEvent *e) override
  {
    if (m_drag.isEmpty())
      return;
    QPointF pos = m_origin + (e->position() - m_press) / scale();
    pos.setX(qBound(-1000000.0, qRound(pos.x()) * 1.0, 1000000.0));
    pos.setY(qBound(-1000000.0, qRound(pos.y()) * 1.0, 1000000.0));
    if (freeMode) {
      const auto size = nodes[m_drag].size();
      qreal dx = 12 / scale(), dy = 12 / scale();
      for (auto it = nodes.cbegin(); it != nodes.cend(); ++it) {
        if (it.key() == m_drag)
          continue;
        const auto r = it.value();
        if (std::min(pos.y() + size.height(), r.bottom()) - std::max(pos.y(), r.top()) >= 1)
          for (const qreal x : {r.right(), r.left() - size.width()})
            if (qAbs(pos.x() - x) < dx) {
              dx = qAbs(pos.x() - x);
              pos.setX(x);
            }
        if (std::min(pos.x() + size.width(), r.right()) - std::max(pos.x(), r.left()) >= 1)
          for (const qreal y : {r.bottom(), r.top() - size.height()})
            if (qAbs(pos.y() - y) < dy) {
              dy = qAbs(pos.y() - y);
              pos.setY(y);
            }
      }
    }
    nodes[m_drag].moveTopLeft(pos);
    if (freeMode && onMove)
      onMove(m_drag, pos, false);
    update();
  }
  void mouseReleaseEvent(QMouseEvent *) override
  {
    if (m_drag.isEmpty())
      return;
    const auto name = m_drag;
    m_drag.clear();
    if (onMove)
      onMove(name, nodes[name].topLeft(), true);
    update();
  }

private:
  qreal scale() const
  {
    return std::max(.001, std::min((width() - 32) / m_world.width(), (height() - 32) / m_world.height()));
  }
  QRectF toView(QRectF r) const
  {
    const auto s = scale();
    const auto origin = QPointF(width() / 2.0, height() / 2.0) - m_world.center() * s;
    return QRectF(r.topLeft() * s + origin, r.size() * s);
  }
  QRectF m_world{0, 0, 960, 600};
  QString m_drag;
  QPointF m_press, m_origin;
};

ScreenLayoutPage::ScreenLayoutPage(ServerConfig &config, QWidget *parent)
    : QWidget(parent),
      m_config(config),
      m_baseline(config),
      m_liveBaseline(config),
      m_draft(config)
{
  setObjectName("screenLayoutPage");
  build();
  load();
  connect(Settings::instance(), &Settings::settingsWritableChanged, this, [this] { updateControls(); });
}

void ScreenLayoutPage::build()
{
  setStyleSheet(QStringLiteral(R"(
    QWidget#screenLayoutPage, QWidget#screenLayoutPage QScrollArea, QWidget#layoutContent {background:white;border:none;}
    QWidget#screenLayoutPage QLabel {background:transparent;color:#303640;font-size:14px;}
    QWidget#screenLayoutPage QLabel[role="muted"] {color:#89929f;font-size:12px;}
    QWidget#screenLayoutPage QLabel[role="heading"] {font-size:19px;font-weight:500;}
    QWidget#screenLayoutPage QPushButton {background:white;border:1px solid #dce3ed;border-radius:8px;padding:9px 15px;color:#5581ae;font-size:13px;}
    QWidget#screenLayoutPage QPushButton:hover {background:#f0f6ff;}
    QWidget#screenLayoutPage QPushButton:checked, QWidget#screenLayoutPage QPushButton[primary="true"] {background:#087fff;border-color:#087fff;color:white;}
    QWidget#screenLayoutPage QPushButton:disabled {color:#a4acb7;background:#f4f6f9;border-color:#edf0f5;}
    QWidget#screenLayoutPage QSpinBox, QWidget#screenLayoutPage QLineEdit, QWidget#screenLayoutPage QComboBox {background:white;border:1px solid #dce3ed;border-radius:7px;color:#526174;padding:8px;}
    QWidget#screenLayoutPage QComboBox::drop-down {border:none;width:22px;}
    QWidget#screenLayoutPage QLabel[error="true"] {color:#b65a47;}
    QWidget#screenLayoutPage QWidget#layoutFooter {background:white;border-top:1px solid #e5eaf0;}
  )"));
  auto outer = new QVBoxLayout(this);
  outer->setContentsMargins(29, 12, 29, 22);
  outer->setSpacing(0);
  auto scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto content = new QWidget(scroll);
  content->setObjectName("layoutContent");
  auto layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 0, 20);
  layout->setSpacing(16);
  auto heading = new QLabel(tr("Screen arrangement"), content);
  heading->setProperty("role", "heading");
  layout->addWidget(heading);
  auto intro = new QLabel(tr("Arrange computers to match their positions on your desk."), content);
  intro->setWordWrap(true);
  intro->setProperty("role", "muted");
  layout->addWidget(intro);
  auto modes = new QHBoxLayout;
  m_fixed = new QPushButton(tr("Fixed regions"), this);
  m_free = new QPushButton(tr("Free arrangement"), this);
  m_fixed->setObjectName("fixedLayoutMode");
  m_free->setObjectName("freeLayoutMode");
  auto modeGroup = new QButtonGroup(this);
  modeGroup->setExclusive(true);
  for (auto b : {m_fixed, m_free}) {
    b->setCheckable(true);
    modeGroup->addButton(b);
    modes->addWidget(b);
  }
  modes->addStretch();
  layout->addLayout(modes);
  connect(m_fixed, &QPushButton::toggled, this, [this](bool on) {
    if (on)
      setFreeMode(false);
  });
  connect(m_free, &QPushButton::toggled, this, [this](bool on) {
    if (on)
      setFreeMode(true);
  });
  m_description = new QLabel(this);
  m_description->setWordWrap(true);
  m_description->setProperty("role", "muted");
  layout->addWidget(m_description);
  m_external = new QLabel(
      tr("An external configuration file is active. Edit that file or disable external configuration to use this "
         "canvas."),
      this
  );
  m_external->setProperty("role", "muted");
  m_external->setWordWrap(true);
  layout->addWidget(m_external);
  m_canvas = new LayoutCanvas(content);
  m_canvas->setObjectName("screenLayoutCanvas");
  layout->addWidget(m_canvas, 1);
  m_canvas->onSelect = [this](const QString &name) { selectComputer(name); };
  m_canvas->onMove = [this](const QString &name, QPointF pos, bool done) {
    if (m_draft.freeLayout() || done) {
      moveComputer(name, pos);
      if (done)
        refresh();
    }
  };
  auto tools = new QHBoxLayout;
  auto add = new QPushButton(tr("Add computer"), this);
  add->setObjectName("addLayoutComputer");
  m_remove = new QPushButton(tr("Remove selected"), this);
  m_remove->setObjectName("removeLayoutComputer");
  auto fit = new QPushButton(tr("Fit to view"), this);
  auto precise = new QPushButton(tr("Precise placement"), this);
  tools->addWidget(add);
  tools->addWidget(m_remove);
  tools->addStretch();
  tools->addWidget(precise);
  tools->addWidget(fit);
  layout->addLayout(tools);
  connect(fit, &QPushButton::clicked, this, [this] { m_canvas->fit(); });
  connect(m_remove, &QPushButton::clicked, this, &ScreenLayoutPage::removeSelected);
  m_editor = new QWidget(this);
  auto editorLayout = new QHBoxLayout(m_editor);
  editorLayout->setContentsMargins(0, 0, 0, 0);
  m_newName = new QLineEdit(m_editor);
  m_newName->setObjectName("newLayoutComputerName");
  m_newName->setPlaceholderText(tr("Enter the other computer's busymouse name"));
  auto confirm = new QPushButton(tr("Add"), this);
  auto cancel = new QPushButton(tr("Cancel"), this);
  editorLayout->addWidget(m_newName, 1);
  editorLayout->addWidget(confirm);
  editorLayout->addWidget(cancel);
  layout->addWidget(m_editor);
  m_editor->hide();
  connect(add, &QPushButton::clicked, this, [this] {
    m_editor->show();
    m_newName->setFocus();
  });
  connect(cancel, &QPushButton::clicked, m_editor, &QWidget::hide);
  connect(confirm, &QPushButton::clicked, this, [this] {
    if (addComputer(m_newName->text().trimmed())) {
      m_newName->clear();
      m_editor->hide();
    }
  });
  m_selection = new QComboBox(this);
  m_selection->setObjectName("layoutComputerSelector");
  m_selection->setAccessibleName(tr("Selected computer"));
  layout->addWidget(m_selection);
  connect(m_selection, &QComboBox::currentTextChanged, this, [this](const QString &name) {
    if (!m_loading)
      selectComputer(name);
  });
  auto details = new QWidget(this);
  auto detailLayout = new QVBoxLayout(details);
  detailLayout->setContentsMargins(0, 0, 0, 0);
  auto fields = new QGridLayout;
  const QStringList labels{tr("Horizontal position"), tr("Vertical position"), tr("Layout width"), tr("Layout height")};
  int i = 0;
  for (auto target : {&m_x, &m_y, &m_width, &m_height}) {
    auto spin = new QSpinBox(this);
    *target = spin;
    spin->setRange(i < 2 ? -1000000 : 80, i < 2 ? 1000000 : 10000);
    spin->setAccessibleName(labels[i]);
    spin->setObjectName(QStringList{"layoutX", "layoutY", "layoutWidth", "layoutHeight"}[i]);
    auto label = new QLabel(labels[i], this);
    label->setBuddy(spin);
    label->setProperty("role", "muted");
    fields->addWidget(label, (i / 2) * 2, (i % 2));
    fields->addWidget(spin, (i / 2) * 2 + 1, (i % 2));
    connect(spin, &QSpinBox::valueChanged, this, [this] {
      if (!m_loading)
        updatePosition();
    });
    ++i;
  }
  detailLayout->addLayout(fields);
  m_hint = new QLabel(tr("Layout units describe relative positions; they do not change display resolution."), this);
  m_hint->setProperty("role", "muted");
  m_hint->setWordWrap(true);
  detailLayout->addWidget(m_hint);
  layout->addWidget(details);
  details->hide();
  connect(precise, &QPushButton::clicked, details, [details] { details->setVisible(details->isHidden()); });
  scroll->setWidget(content);
  outer->addWidget(scroll, 1);
  auto footer = new QWidget(this);
  footer->setObjectName("layoutFooter");
  auto footerLayout = new QHBoxLayout(footer);
  footerLayout->setContentsMargins(0, 17, 0, 0);
  m_status = new QLabel(this);
  m_status->setObjectName("layoutSaveState");
  m_status->setProperty("role", "muted");
  m_status->setWordWrap(true);
  footerLayout->addWidget(m_status, 1);
  m_discard = new QPushButton(tr("Discard changes"), this);
  m_discard->setObjectName("discardLayout");
  m_save = new QPushButton(tr("Save arrangement"), this);
  m_save->setProperty("primary", true);
  m_save->setObjectName("saveLayout");
  footerLayout->addWidget(m_discard);
  footerLayout->addWidget(m_save);
  outer->addWidget(footer);
  connect(m_discard, &QPushButton::clicked, this, &ScreenLayoutPage::discard);
  connect(m_save, &QPushButton::clicked, this, [this] { save(); });
}

void ScreenLayoutPage::ensureLocalComputer()
{
  const auto name = Settings::value(Settings::Core::ComputerName).toString();
  if (m_draft.computerExists(name))
    return;
  const int center = (m_draft.m_rows / 2) * m_draft.m_columns + m_draft.m_columns / 2;
  int index = m_draft.computers()[center].isNull() ? center : -1;
  if (index < 0)
    for (int i = 0; i < m_draft.computers().size(); ++i)
      if (m_draft.computers()[i].isNull()) {
        index = i;
        break;
      }
  if (index >= 0) {
    Computer computer(name);
    computer.markAsServer();
    m_draft.computers()[index] = computer;
  }
}

void ScreenLayoutPage::seedFreeRectangles()
{
  auto rectangles = m_draft.layoutRectangles();
  for (int i = 0; i < m_draft.computers().size(); ++i) {
    const auto &c = m_draft.computers()[i];
    if (c.isNull() || rectangles.contains(c.name()))
      continue;
    rectangles[c.name()] = QRectF((i % m_draft.m_columns) * 320, (i / m_draft.m_columns) * 200, 320, 200);
  }
  const auto names = rectangles.keys();
  for (const auto &name : names)
    if (!m_draft.computerExists(name))
      rectangles.remove(name);
  m_draft.setLayoutRectangles(rectangles);
}

screenlayout::Rectangles ScreenLayoutPage::rectangles() const
{
  screenlayout::Rectangles result;
  for (int i = 0; i < m_draft.computers().size(); ++i) {
    const auto &c = m_draft.computers()[i];
    if (c.isNull())
      continue;
    result[c.name()] = m_draft.freeLayout()
                           ? m_draft.layoutRectangles().value(c.name())
                           : QRectF((i % m_draft.m_columns) * 320, (i / m_draft.m_columns) * 200, 320, 200);
  }
  return result;
}

QRectF ScreenLayoutPage::deviceVisualRect(const QString &name) const
{
  return m_canvas->visualRectangle(name);
}

void ScreenLayoutPage::load()
{
  m_loading = true;
  m_liveBaseline = m_config;
  m_draft = m_config;
  ensureLocalComputer();
  seedFreeRectangles();
  m_baseline = m_draft;
  m_editor->hide();
  m_selected = Settings::value(Settings::Core::ComputerName).toString();
  m_loading = false;
  refresh(true);
}
void ScreenLayoutPage::activate()
{
  if (!isModified())
    load();
}
void ScreenLayoutPage::discard()
{
  load();
}
bool ScreenLayoutPage::isModified() const
{
  return !(m_draft == m_baseline);
}

void ScreenLayoutPage::refresh(bool fit)
{
  m_loading = true;
  m_free->setChecked(m_draft.freeLayout());
  m_fixed->setChecked(!m_draft.freeLayout());
  m_description->setText(
      m_draft.freeLayout()
          ? tr("Drag computers freely. Nearby edges snap together; green edges show where the pointer can cross.")
          : tr("Drag computers into fixed regions. Adjacent regions define the direction of cross-screen movement.")
  );
  m_canvas->nodes = rectangles();
  m_canvas->freeMode = m_draft.freeLayout();
  m_canvas->columns = m_draft.m_columns;
  m_canvas->rows = m_draft.m_rows;
  m_canvas->local = Settings::value(Settings::Core::ComputerName).toString();
  m_selection->clear();
  for (const auto &c : std::as_const(m_draft.computers()))
    if (!c.isNull())
      m_selection->addItem(c.name());
  if (!m_canvas->nodes.contains(m_selected))
    m_selected = m_canvas->nodes.isEmpty() ? QString() : m_canvas->nodes.firstKey();
  m_loading = false;
  selectComputer(m_selected);
  if (fit)
    m_canvas->fit();
  else
    m_canvas->update();
  updateControls();
}

void ScreenLayoutPage::selectComputer(const QString &name)
{
  m_loading = true;
  m_selected = name;
  m_canvas->selected = name;
  m_selection->setCurrentText(name);
  const auto r = rectangles().value(name);
  m_x->setRange(m_draft.freeLayout() ? -1000000 : 0, m_draft.freeLayout() ? 1000000 : m_draft.m_columns - 1);
  m_y->setRange(m_draft.freeLayout() ? -1000000 : 0, m_draft.freeLayout() ? 1000000 : m_draft.m_rows - 1);
  m_x->setValue(qRound(m_draft.freeLayout() ? r.x() : r.x() / 320));
  m_y->setValue(qRound(m_draft.freeLayout() ? r.y() : r.y() / 200));
  m_width->setValue(qRound(r.width()));
  m_height->setValue(qRound(r.height()));
  m_width->setEnabled(m_draft.freeLayout());
  m_height->setEnabled(m_draft.freeLayout());
  m_loading = false;
  m_canvas->update();
  updateControls();
}

void ScreenLayoutPage::setFreeMode(bool enabled)
{
  if (m_loading || m_config.useExternalConfig() || !Settings::isWritable())
    return;
  m_draft.setFreeLayout(enabled);
  refresh(true);
}

bool ScreenLayoutPage::moveComputer(const QString &name, QPointF position)
{
  if (m_config.useExternalConfig() || !Settings::isWritable())
    return false;
  if (m_draft.freeLayout()) {
    auto data = m_draft.layoutRectangles();
    if (!data.contains(name))
      return false;
    auto r = data[name];
    r.moveTopLeft(position);
    data[name] = r;
    m_draft.setLayoutRectangles(data);
    m_canvas->nodes = data;
    selectComputer(name);
    return true;
  }
  const int x = qBound(0, qRound(position.x() / 320), m_draft.m_columns - 1),
            y = qBound(0, qRound(position.y() / 200), m_draft.m_rows - 1), target = y * m_draft.m_columns + x;
  int source = -1;
  for (int i = 0; i < m_draft.computers().size(); ++i)
    if (m_draft.computers()[i].name() == name) {
      source = i;
      break;
    }
  if (source < 0)
    return false;
  if (target != source && !m_draft.computers()[target].isNull()) {
    message(tr("That region already contains a computer."), true);
    return false;
  }
  if (source != target)
    m_draft.computers().swapItemsAt(source, target);
  refresh();
  return true;
}

void ScreenLayoutPage::updatePosition()
{
  if (m_selected.isEmpty())
    return;
  if (m_draft.freeLayout()) {
    auto data = m_draft.layoutRectangles();
    auto r = data[m_selected];
    r.setSize(QSizeF(m_width->value(), m_height->value()));
    r.moveTopLeft(QPointF(m_x->value(), m_y->value()));
    data[m_selected] = r;
    m_draft.setLayoutRectangles(data);
    m_canvas->nodes = rectangles();
    m_canvas->update();
    updateControls();
  } else
    moveComputer(m_selected, QPointF(m_x->value() * 320, m_y->value() * 200));
}

bool ScreenLayoutPage::addComputer(const QString &name)
{
  if (m_config.useExternalConfig() || !Settings::isWritable())
    return false;
  validators::ComputerNameValidator validator{QString()};
  if (name.isEmpty() || !validator.validate(name)) {
    message(tr("Use a valid busymouse computer name: letters, numbers, underscores, dots or hyphens."), true);
    return false;
  }
  for (const auto &c : std::as_const(m_draft.computers())) {
    if (c.name().compare(name, Qt::CaseInsensitive) == 0) {
      message(tr("A computer with this name already exists."), true);
      return false;
    }
    for (const auto &alias : c.aliases())
      if (alias.compare(name, Qt::CaseInsensitive) == 0) {
        message(tr("This name is already used as a computer alias."), true);
        return false;
      }
  }
  if (m_draft.isFull()) {
    message(tr("All fixed regions are occupied. Remove a computer before adding another."), true);
    return false;
  }
  const auto previous = rectangles();
  m_draft.addClient(name);
  seedFreeRectangles();
  auto data = m_draft.layoutRectangles();
  if (!previous.isEmpty()) {
    QRectF right = previous.first();
    for (const auto &r : previous)
      if (r.right() > right.right())
        right = r;
    data[name] = QRectF(right.right(), right.top(), 320, 200);
    m_draft.setLayoutRectangles(data);
  }
  m_selected = name;
  refresh(true);
  return true;
}

void ScreenLayoutPage::suggestComputer(const QString &name)
{
  m_editor->show();
  m_newName->setText(name);
  m_newName->setFocus();
}

void ScreenLayoutPage::focusComputer(const QString &name)
{
  if (m_draft.computerExists(name))
    selectComputer(name);
  else
    suggestComputer(name);
}

void ScreenLayoutPage::removeSelected()
{
  if (m_selected.isEmpty() || m_selected == Settings::value(Settings::Core::ComputerName).toString())
    return;
  for (const auto &hotkey : std::as_const(m_draft.hotkeys()))
    for (const auto &action : hotkey.actions())
      if (action.switchComputerName() == m_selected || action.typeComputerNames().contains(m_selected)) {
        message(tr("Remove this computer's hotkey references before removing it from the layout."), true);
        return;
      }
  for (auto &c : m_draft.computers())
    if (c.name() == m_selected) {
      c = Computer();
      break;
    }
  auto data = m_draft.layoutRectangles();
  data.remove(m_selected);
  m_draft.setLayoutRectangles(data);
  m_selected.clear();
  refresh(true);
}

QString ScreenLayoutPage::validationMessage() const
{
  if (!m_draft.freeLayout())
    return {};
  switch (screenlayout::validate(rectangles())) {
  case screenlayout::Error::None:
    return {};
  case screenlayout::Error::InvalidRectangle:
    return tr("Enter valid layout positions and sizes before saving.");
  case screenlayout::Error::Overlap:
    return tr("Computers overlap. Separate their rectangles before saving.");
  case screenlayout::Error::Disconnected:
    return tr(
        "Some computers have no connecting edge. Bring their edges together; corner-only contact cannot be crossed."
    );
  }
  return {};
}

void ScreenLayoutPage::message(const QString &text, bool error)
{
  m_status->setText(text);
  m_status->setProperty("error", error);
  m_status->style()->unpolish(m_status);
  m_status->style()->polish(m_status);
}

void ScreenLayoutPage::updateControls()
{
  const bool editable = Settings::isWritable() && !m_config.useExternalConfig();
  m_external->setVisible(m_config.useExternalConfig());
  for (auto w :
       {static_cast<QWidget *>(m_fixed), static_cast<QWidget *>(m_free), static_cast<QWidget *>(m_canvas),
        static_cast<QWidget *>(m_selection), static_cast<QWidget *>(m_x), static_cast<QWidget *>(m_y),
        static_cast<QWidget *>(m_editor)})
    w->setEnabled(editable);
  findChild<QPushButton *>("addLayoutComputer")->setEnabled(editable);
  m_width->setEnabled(editable && m_draft.freeLayout());
  m_height->setEnabled(editable && m_draft.freeLayout());
  m_remove->setEnabled(
      editable && !m_selected.isEmpty() && m_selected != Settings::value(Settings::Core::ComputerName).toString()
  );
  const auto error = validationMessage();
  m_save->setEnabled(editable && isModified() && error.isEmpty());
  m_discard->setEnabled(isModified());
  message(
      !editable          ? tr("Arrangement is read-only.")
      : !error.isEmpty() ? error
      : isModified()     ? tr("Unsaved arrangement. Saving restarts an active sharing service.")
                         : tr("Arrangement saved. Each mode keeps its own positions."),
      !error.isEmpty()
  );
}

bool ScreenLayoutPage::save()
{
  if (!Settings::isWritable() || m_config.useExternalConfig())
    return false;
  if (!(m_config == m_liveBaseline)) {
    message(
        tr("Computer configuration changed while editing. Discard and reopen the arrangement before saving."), true
    );
    return false;
  }
  const auto error = validationMessage();
  if (!error.isEmpty()) {
    message(error, true);
    return false;
  }
  m_config = m_draft;
  m_config.commit();
  QStringList known;
  for (const auto &c : std::as_const(m_draft.computers()))
    if (!c.isNull())
      known << QStringLiteral("computer_%1").arg(c.name());
  Settings::removeUnknownComputers(known);
  Settings::save();
  load();
  Q_EMIT saved();
  return true;
}

void ScreenLayoutPage::changeEvent(QEvent *event)
{
  QWidget::changeEvent(event);
  if (event->type() != QEvent::LanguageChange || !layout())
    return;
  QTimer::singleShot(0, this, [this] {
    const bool dirty = isModified();
    const auto draft = m_draft, baseline = m_baseline, live = m_liveBaseline;
    const auto selected = m_selected;
    m_loading = true;
    const auto children = findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (auto child : children)
      delete child;
    delete layout();
    build();
    if (dirty) {
      m_draft = draft;
      m_baseline = baseline;
      m_liveBaseline = live;
      m_selected = selected;
      m_loading = false;
      refresh(true);
    } else
      load();
  });
}
