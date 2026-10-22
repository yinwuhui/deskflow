/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "DeviceListPage.h"
#include "HomeIcons.h"
#include "common/Settings.h"
#include "gui/validators/ComputerNameValidator.h"
#include <QAccessibleWidget>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>
#include <functional>

using deskflow::gui::HomeIcon;
using deskflow::gui::homeIcon;

namespace {
enum ItemRole
{
  Id = Qt::UserRole,
  Name,
  Subtitle,
  ConnectionStateRole,
  Group,
  Collapsed,
  Local,
  Empty
};
class DeviceDelegate : public QStyledItemDelegate
{
public:
  using QStyledItemDelegate::QStyledItemDelegate;
  void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override
  {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const auto r = o.rect;
    auto font = o.font;
    if (i.data(Empty).toBool()) {
      p->setPen(QColor("#8994a1"));
      p->drawText(r.adjusted(18, 0, -18, 0), Qt::AlignCenter | Qt::TextWordWrap, i.data().toString());
      p->restore();
      return;
    }
    if (!i.data(Group).toString().isEmpty()) {
      font.setPixelSize(14);
      font.setWeight(QFont::Medium);
      p->setFont(font);
      p->setPen(QPen(QColor("#8994a1"), 1.5));
      const QPoint center(r.left() + 16, r.center().y());
      QPolygon points;
      if (i.data(Collapsed).toBool())
        points << center + QPoint(-2, -4) << center + QPoint(2, 0) << center + QPoint(-2, 4);
      else
        points << center + QPoint(-4, -2) << center + QPoint(0, 2) << center + QPoint(4, -2);
      p->drawPolyline(points);
      p->setPen(QColor("#2b3037"));
      p->drawText(r.adjusted(30, 0, -35, 0), Qt::AlignVCenter, i.data(Name).toString());
      font.setPixelSize(11);
      font.setWeight(QFont::Normal);
      p->setFont(font);
      p->setPen(QColor("#8994a1"));
      p->drawText(r.adjusted(0, 0, -18, 0), Qt::AlignRight | Qt::AlignVCenter, i.data(Subtitle).toString());
    } else {
      const bool selected = o.state & QStyle::State_Selected;
      p->fillRect(r, selected ? QColor("#edf4fe") : QColor("#ffffff"));
      if (selected) {
        p->setPen(Qt::NoPen);
        p->setBrush(QColor("#087fff"));
        p->drawRoundedRect(QRect(r.left(), r.top() + 13, 3, r.height() - 26), 1.5, 1.5);
      }
      const auto state = static_cast<DeviceListPage::State>(i.data(ConnectionStateRole).toInt());
      p->setPen(Qt::NoPen);
      p->setBrush(
          state == DeviceListPage::State::Connected || state == DeviceListPage::State::Local ? QColor("#2abb80")
          : state == DeviceListPage::State::Discovered                                       ? QColor("#087fff")
                                                                                             : QColor("#c6ccd4")
      );
      p->drawEllipse(QPointF(r.left() + 17, r.center().y()), 3.5, 3.5);
      homeIcon(HomeIcon::Monitor, QColor("#087fff")).paint(p, QRect(r.left() + 31, r.center().y() - 11, 22, 22));
      const int tagWidth = i.data(Local).toBool() ? 53 : 0;
      const auto textBox = r.adjusted(64, 0, -14 - tagWidth, 0);
      font.setPixelSize(14);
      font.setWeight(QFont::Medium);
      p->setFont(font);
      p->setPen(QColor("#2b3037"));
      p->drawText(
          textBox.adjusted(0, 10, 0, -31), Qt::AlignVCenter,
          p->fontMetrics().elidedText(i.data(Name).toString(), Qt::ElideRight, textBox.width())
      );
      font.setPixelSize(11);
      font.setWeight(QFont::Normal);
      p->setFont(font);
      p->setPen(QColor("#8994a1"));
      p->drawText(
          r.adjusted(64, 34, -14, -8), Qt::AlignVCenter,
          p->fontMetrics().elidedText(i.data(Subtitle).toString(), Qt::ElideRight, r.width() - 78)
      );
      if (tagWidth) {
        const QRect tag(r.right() - 57, r.top() + 15, 46, 20);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor("#e1efff"));
        p->drawRoundedRect(tag, 4, 4);
        p->setPen(QColor("#087fff"));
        p->drawText(tag, Qt::AlignCenter, DeviceListPage::tr("This device"));
      }
    }
    if (o.state & QStyle::State_HasFocus) {
      p->setPen(QPen(QColor("#087fff"), 1, Qt::DotLine));
      p->setBrush(Qt::NoBrush);
      p->drawRect(r.adjusted(4, 3, -4, -3));
    }
    p->restore();
  }
};

// Expose genuine button children instead of synthesized Cocoa table cells. Resetting a
// QListWidget model while native accessibility clients inspect its synthetic row arrays
// crashes in QMacAccessibilityElement::dealloc on the bundled Qt/macOS combination.
// The buttons retain keyboard/VoiceOver actions and use the same row painter as the list.
class DeviceRowButton : public QPushButton
{
public:
  DeviceRowButton(QListWidget *list, QListWidgetItem *item) : QPushButton(list->viewport()), m_list(list), m_item(item)
  {
    setText(item->text());
    setAccessibleName(item->text());
    setToolTip(item->toolTip());
    setAutoDefault(false);
  }

protected:
  void paintEvent(QPaintEvent *) override
  {
    QStyleOptionViewItem option;
    option.initFrom(this);
    option.rect = rect();
    option.font = font();
    if (m_item->isSelected())
      option.state |= QStyle::State_Selected;
    if (hasFocus())
      option.state |= QStyle::State_HasFocus;
    QPainter painter(this);
    m_list->itemDelegate()->paint(&painter, option, m_list->model()->index(m_list->row(m_item), 0));
  }

private:
  QListWidget *m_list;
  QListWidgetItem *m_item;
};
QLabel *label(const QString &text, const QString &role, QWidget *parent)
{
  auto l = new QLabel(text, parent);
  l->setTextFormat(Qt::PlainText);
  l->setWordWrap(true);
  l->setProperty("deviceRole", role);
  l->setMinimumWidth(0);
  return l;
}
QPushButton *button(const QString &text, const QString &role, QWidget *parent, HomeIcon icon = HomeIcon::Monitor)
{
  auto b = new QPushButton(text, parent);
  b->setProperty("deviceRole", role);
  b->setIcon(homeIcon(icon, role == "primary" ? Qt::white : QColor("#087fff")));
  b->setIconSize(QSize(18, 18));
  b->setAutoDefault(false);
  return b;
}
} // namespace

DeviceListPage::DeviceListPage(QWidget *parent) : QWidget(parent)
{
  static const bool factoryInstalled = [] {
    QAccessible::installFactory([](const QString &, QObject *object) -> QAccessibleInterface * {
      if (auto list = qobject_cast<QListWidget *>(object); list && list->objectName() == "devicesList")
        return new QAccessibleWidget(list, QAccessible::List);
      return nullptr;
    });
    return true;
  }();
  Q_UNUSED(factoryInstalled);
  setObjectName("deviceListPage");
  build();
}

QString DeviceListPage::normalizedAddress(const QString &address, int defaultPort)
{
  const auto text = address.trimmed();
  if (text.isEmpty() || text.contains(',') || text.contains(QRegularExpression(QStringLiteral("\\s"))))
    return {};
  const auto url = QUrl(QStringLiteral("busymouse://") + text);
  if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || !url.path().isEmpty() || url.hasQuery() ||
      url.hasFragment() || url.port(defaultPort) < 1 || url.port(defaultPort) > 65535)
    return {};
  auto host = url.host().toLower();
  if (host.contains(':'))
    host = '[' + host + ']';
  return host + ':' + QString::number(url.port(defaultPort));
}

void DeviceListPage::build()
{
  setStyleSheet(QStringLiteral(R"(
    QWidget#deviceListPage { background: #ffffff; color: #2b3037; font-size: 14px; }
    QWidget#deviceListPage QLabel { background: transparent; border: none; }
    QWidget#deviceListPage QLabel[deviceRole="title"] { font-size: 20px; font-weight: 500; }
    QWidget#deviceListPage QLabel[deviceRole="heading"] { font-size: 15px; font-weight: 500; }
    QWidget#deviceListPage QLabel[deviceRole="muted"] { color: #8994a1; font-size: 12px; }
    QWidget#deviceListPage QLabel[deviceRole="status"] { color: #8994a1; font-size: 11px; }
    QWidget#deviceListPage QPushButton { border: none; background: transparent; padding: 7px; color: #087fff; }
    QWidget#deviceListPage QPushButton:focus { border: 1px solid #087fff; }
    QWidget#deviceListPage QPushButton:disabled { color: #a5adb7; }
    QWidget#deviceListPage QPushButton[deviceRole="primary"] { background: #087fff; color: white; border-radius: 7px; padding: 11px 22px; }
    QWidget#deviceListPage QPushButton[deviceRole="secondary"] { border: 1px solid #dfe4ea; border-radius: 7px; padding: 9px 15px; }
    QWidget#deviceListPage QPushButton[deviceRole="circle"] { background: #edf5ff; border-radius: 18px; }
    QWidget#deviceListPage QPushButton[deviceRole="action"] { background: #edf5ff; border-radius: 12px; padding: 13px; }
    QWidget#deviceListPage QLineEdit { border: 1px solid #dce1e7; border-radius: 7px; padding: 9px 12px; background: white; color: #2b3037; }
    QWidget#deviceListPage QLineEdit#deviceSearch { border-radius: 19px; font-size: 12px; padding-left: 30px; }
    QWidget#deviceListPage QComboBox { border: 1px solid #e5e8ed; border-radius: 6px; padding: 6px; color: #758190; background: white; }
    QWidget#deviceListPage QComboBox#deviceFilter { border: none; font-size: 11px; padding: 3px; }
    QWidget#deviceListPage QComboBox#deviceFilter::drop-down { border: none; width: 16px; }
    QWidget#deviceListPage QListWidget { border: none; background: white; outline: none; }
    QWidget#deviceListPage QScrollArea { border: none; background: white; }
    QWidget#deviceListPage QWidget#deviceDetails, QWidget#deviceAddPage { background: white; }
    QWidget#deviceListPage QWidget#deviceListFooter { border-top: 1px solid #e8ebf0; background: white; }
    QWidget#deviceListPage QFrame#deviceContext { background: #f6f8fb; border-radius: 9px; }
    QWidget#deviceListPage QSplitter::handle { background: #e8ebf0; }
    QWidget#deviceListPage QLabel#deviceAddError { color: #bd554b; font-size: 12px; }
  )"));
  auto outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  auto splitter = new QSplitter(Qt::Horizontal, this);
  splitter->setObjectName("deviceSplitter");
  splitter->setChildrenCollapsible(false);
  splitter->setHandleWidth(1);
  auto listPane = new QWidget(splitter);
  listPane->setMinimumWidth(230);
  auto left = new QVBoxLayout(listPane);
  left->setContentsMargins(0, 20, 0, 0);
  left->setSpacing(0);
  auto searchRow = new QHBoxLayout;
  searchRow->setContentsMargins(18, 0, 18, 0);
  auto searchBox = new QFrame(listPane);
  searchBox->setObjectName("deviceSearchBox");
  searchBox->setFixedHeight(40);
  searchBox->setStyleSheet(
      QStringLiteral("QFrame#deviceSearchBox { border: 1px solid #dce1e7; border-radius: 19px; background: white; }")
  );
  auto searchLayout = new QHBoxLayout(searchBox);
  searchLayout->setContentsMargins(11, 0, 10, 0);
  searchLayout->setSpacing(7);
  auto searchIcon = new QLabel(searchBox);
  searchIcon->setPixmap(homeIcon(HomeIcon::Search).pixmap(16, 16));
  searchLayout->addWidget(searchIcon);
  m_search = new QLineEdit(searchBox);
  m_search->setObjectName("deviceSearch");
  m_search->setPlaceholderText(tr("Search name or address"));
  m_search->setAccessibleName(tr("Search name or address"));
  m_search->setStyleSheet(QStringLiteral("border: none; background: transparent; padding: 8px 0; font-size: 12px;"));
  m_search->setClearButtonEnabled(true);
  searchLayout->addWidget(m_search, 1);
  searchRow->addWidget(searchBox, 1);
  auto add = button({}, "circle", listPane, HomeIcon::MonitorUp);
  add->setFixedSize(36, 36);
  add->setObjectName("deviceAddButton");
  add->setAccessibleName(tr("Add device"));
  add->setToolTip(tr("Add device"));
  searchRow->addWidget(add);
  left->addLayout(searchRow);
  auto tools = new QHBoxLayout;
  tools->setContentsMargins(18, 14, 13, 7);
  m_count = label({}, "status", listPane);
  tools->addWidget(m_count, 1);
  m_filter = new QComboBox(listPane);
  m_filter->setObjectName("deviceFilter");
  m_filter->setAccessibleName(tr("Filter connection state"));
  m_filter->addItem(tr("All states"), -1);
  m_filter->addItem(tr("Connected"), int(State::Connected));
  m_filter->addItem(tr("Discoverable"), int(State::Discovered));
  m_filter->addItem(tr("Not connected"), int(State::Offline));
  tools->addWidget(m_filter);
  auto refresh = button({}, "icon", listPane, HomeIcon::Refresh);
  refresh->setObjectName("deviceRefreshButton");
  refresh->setAccessibleName(tr("Refresh local devices"));
  refresh->setToolTip(tr("Refresh local devices"));
  tools->addWidget(refresh);
  left->addLayout(tools);
  m_list = new QListWidget(listPane);
  m_list->setObjectName("devicesList");
  // With an active native accessibility client, QListWidget's base constructor can
  // cache its standard table interface before the identifying object name exists.
  // Replace that empty interface before inserting rows, when no synthetic cells exist.
  if (const auto accessible = QAccessible::queryAccessibleInterface(m_list); accessible && accessible->tableInterface())
    QAccessible::deleteAccessibleInterface(QAccessible::uniqueId(accessible));
  m_list->setAccessibleName(tr("Devices"));
  m_list->setItemDelegate(new DeviceDelegate(m_list));
  m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_list->setSelectionMode(QAbstractItemView::SingleSelection);
  left->addWidget(m_list, 1);
  m_discovery = label(tr("Local discovery is starting…"), "status", listPane);
  m_discovery->setContentsMargins(18, 18, 18, 18);
  m_discovery->setStyleSheet(QStringLiteral("padding: 18px;"));
  left->addWidget(m_discovery);
  m_detail = new QStackedWidget(splitter);
  m_detail->setObjectName("deviceDetailsStack");
  m_detail->setMinimumWidth(265);
  auto scroll = new QScrollArea(m_detail);
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_details = new QWidget(scroll);
  m_details->setObjectName("deviceDetails");
  auto detailsLayout = new QVBoxLayout(m_details);
  detailsLayout->setContentsMargins(28, 27, 28, 22);
  scroll->setWidget(m_details);
  m_detail->addWidget(scroll);
  auto addScroll = new QScrollArea(m_detail);
  addScroll->setWidgetResizable(true);
  addScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto form = new QWidget(addScroll);
  form->setObjectName("deviceAddPage");
  auto formLayout = new QVBoxLayout(form);
  formLayout->setContentsMargins(28, 27, 28, 22);
  formLayout->setSpacing(15);
  formLayout->addWidget(label(tr("Add device"), "title", form));
  auto kindLabel = label(tr("Device purpose"), "muted", form);
  formLayout->addWidget(kindLabel);
  m_addKind = new QComboBox(form);
  m_addKind->setObjectName("deviceAddKind");
  m_addKind->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  m_addKind->setMinimumContentsLength(12);
  m_addKind->addItems(
      {tr("Receive this computer's keyboard and mouse"), tr("Use another computer's keyboard and mouse")}
  );
  kindLabel->setBuddy(m_addKind);
  formLayout->addWidget(m_addKind);
  auto nameLabel = label(tr("Computer name"), "muted", form);
  m_addName = new QLineEdit(form);
  m_addName->setObjectName("deviceAddName");
  m_addName->setMaxLength(255);
  nameLabel->setBuddy(m_addName);
  m_addName->setPlaceholderText(tr("Match the name shown in the other busymouse"));
  formLayout->addWidget(nameLabel);
  formLayout->addWidget(m_addName);
  m_addressRow = new QWidget(form);
  auto addressLayout = new QVBoxLayout(m_addressRow);
  addressLayout->setContentsMargins(0, 0, 0, 0);
  auto addressLabel = label(tr("Server address"), "muted", m_addressRow);
  m_addAddress = new QLineEdit(m_addressRow);
  m_addAddress->setObjectName("deviceAddAddress");
  m_addAddress->setPlaceholderText(tr("IP or hostname, optionally with :port"));
  addressLabel->setBuddy(m_addAddress);
  addressLayout->addWidget(addressLabel);
  addressLayout->addWidget(m_addAddress);
  formLayout->addWidget(m_addressRow);
  m_addressRow->hide();
  m_addError = label({}, "muted", form);
  m_addError->setObjectName("deviceAddError");
  formLayout->addWidget(m_addError);
  auto formButtons = new QHBoxLayout;
  formButtons->addStretch();
  auto cancel = button(tr("Cancel"), "secondary", form);
  auto submit = button(tr("Add device"), "primary", form, HomeIcon::MonitorUp);
  cancel->setIcon({});
  submit->setIcon({});
  submit->setObjectName("deviceAddSubmit");
  formButtons->addWidget(cancel);
  formButtons->addWidget(submit);
  formLayout->addLayout(formButtons);
  formLayout->addWidget(label(
      tr("For a receiving computer, continue to screen arrangement and save its position. For a server, save its "
         "address here and connect when ready."),
      "muted", form
  ));
  formLayout->addStretch();
  addScroll->setWidget(form);
  m_detail->addWidget(addScroll);
  splitter->setSizes({300, 470});
  splitter->setStretchFactor(0, 0);
  splitter->setStretchFactor(1, 1);
  outer->addWidget(splitter, 1);
  auto footer = new QWidget(this);
  footer->setObjectName("deviceListFooter");
  auto footerLayout = new QHBoxLayout(footer);
  footerLayout->setContentsMargins(23, 14, 23, 14);
  m_status = label({}, "status", footer);
  m_status->setObjectName("devicePageStatus");
  m_status->setAccessibleName(tr("Device status"));
  footerLayout->addWidget(m_status, 1);
  footerLayout->addWidget(label(tr("Local network"), "status", footer));
  outer->addWidget(footer);
  connect(m_search, &QLineEdit::textChanged, this, &DeviceListPage::rebuildList);
  connect(m_filter, &QComboBox::currentIndexChanged, this, &DeviceListPage::rebuildList);
  connect(add, &QPushButton::clicked, this, &DeviceListPage::showAddForm);
  connect(refresh, &QPushButton::clicked, this, &DeviceListPage::refreshRequested);
  connect(cancel, &QPushButton::clicked, this, [this] { m_detail->setCurrentIndex(0); });
  connect(submit, &QPushButton::clicked, this, &DeviceListPage::submitAdd);
  connect(m_addName, &QLineEdit::returnPressed, this, &DeviceListPage::submitAdd);
  connect(m_addAddress, &QLineEdit::returnPressed, this, &DeviceListPage::submitAdd);
  connect(m_addKind, &QComboBox::currentIndexChanged, this, [this](int index) {
    m_addressRow->setVisible(index == 1);
    m_addError->clear();
  });
  connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
    if (m_rebuilding || !item || item->data(Id).toString().isEmpty())
      return;
    m_selected = item->data(Id).toString();
    m_detail->setCurrentIndex(0);
    rebuildDetail();
  });
  connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
    const auto group = item->data(Group).toString();
    if (!group.isEmpty()) {
      if (m_collapsed.contains(group))
        m_collapsed.remove(group);
      else
        m_collapsed.insert(group);
      rebuildList();
    }
  });
  connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
    if (!item->data(Group).toString().isEmpty())
      Q_EMIT m_list->itemActivated(item);
    else if (!item->data(Id).toString().isEmpty())
      m_detail->setCurrentIndex(0);
  });
}

void DeviceListPage::setDevices(const QList<Device> &devices, bool sharing, const QString &status, bool busy)
{
  const bool changed = devices != m_devices || sharing != m_sharing || busy != m_busy;
  m_devices = devices;
  m_sharing = sharing;
  m_busy = busy;
  message(status);
  if (changed) {
    if (!device())
      m_selected = m_devices.isEmpty() ? QString() : m_devices.first().id;
    rebuildList();
    rebuildDetail();
  }
}

const DeviceListPage::Device *DeviceListPage::device() const
{
  for (const auto &d : m_devices)
    if (d.id == m_selected)
      return &d;
  return nullptr;
}

void DeviceListPage::selectDevice(const QString &id)
{
  m_selected = id;
  m_detail->setCurrentIndex(0);
  rebuildList();
  rebuildDetail();
}

QString DeviceListPage::stateText(State state) const
{
  switch (state) {
  case State::Local:
    return tr("This computer");
  case State::Connected:
    return tr("Connected");
  case State::Discovered:
    return tr("Discoverable");
  case State::Offline:
    return tr("Not connected");
  }
  return {};
}

bool DeviceListPage::matches(const Device &d) const
{
  const auto query = m_search->text().trimmed();
  return (query.isEmpty() || d.name.contains(query, Qt::CaseInsensitive) ||
          d.address.contains(query, Qt::CaseInsensitive)) &&
         (m_filter->currentData().toInt() == -1 || m_filter->currentData().toInt() == int(d.state));
}

void DeviceListPage::addGroup(const QString &key, const QString &title, const QList<Device> &devices)
{
  QList<Device> filtered;
  for (const auto &d : devices)
    if (matches(d))
      filtered.append(d);
  if (filtered.isEmpty())
    return;
  const bool collapsed = m_collapsed.contains(key) && m_search->text().isEmpty();
  auto header = new QListWidgetItem(m_list);
  header->setText(title);
  header->setData(Group, key);
  header->setData(Name, title);
  header->setData(Subtitle, QString::number(filtered.size()));
  header->setData(Collapsed, collapsed);
  header->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
  header->setSizeHint(QSize(0, 42));
  attachAccessibleRow(header);
  if (collapsed)
    return;
  for (const auto &d : filtered) {
    auto item = new QListWidgetItem(m_list);
    const auto subtitle = (d.address.isEmpty() ? QString() : d.address + QStringLiteral(" · ")) + stateText(d.state);
    item->setText(d.name + ' ' + subtitle);
    item->setData(Id, d.id);
    item->setData(Name, d.name);
    item->setData(Subtitle, subtitle);
    item->setData(ConnectionStateRole, int(d.state));
    item->setData(Local, d.kind == Kind::Local);
    item->setToolTip(d.name + '\n' + subtitle);
    item->setSizeHint(QSize(0, 70));
    attachAccessibleRow(item);
    if (d.id == m_selected && !m_list->currentItem())
      m_list->setCurrentItem(item);
  }
}

void DeviceListPage::attachAccessibleRow(QListWidgetItem *item)
{
  auto button = new DeviceRowButton(m_list, item);
  m_list->setItemWidget(item, button);
  connect(button, &QPushButton::clicked, this, [this, item] {
    m_list->setCurrentItem(item);
    Q_EMIT m_list->itemClicked(item);
    m_list->viewport()->update();
  });
}

void DeviceListPage::rebuildList()
{
  const bool searchFocused = m_search->hasFocus();
  const bool filterFocused = m_filter->hasFocus();
  m_rebuilding = true;
  const auto scroll = m_list->verticalScrollBar()->value();
  m_list->clear();
  QList<Device> recent, mine, discovered;
  int count = 0;
  for (const auto &d : m_devices) {
    if (matches(d))
      ++count;
    if (d.recent)
      recent.append(d);
    if (d.discovered && d.kind != Kind::Local)
      discovered.append(d);
    if (!d.discovered || d.configured || d.recent || d.kind == Kind::Local)
      mine.append(d);
  }
  m_count->setText(tr("%n device(s)", nullptr, count));
  addGroup("recent", tr("Recent connections"), recent);
  addGroup("mine", tr("My devices"), mine);
  addGroup("discovery", tr("Local discovery"), discovered);
  if (count == 0) {
    auto empty = new QListWidgetItem(tr("No matching devices"), m_list);
    empty->setFlags(Qt::NoItemFlags);
    empty->setData(Empty, true);
    empty->setSizeHint(QSize(0, 70));
  }
  m_list->verticalScrollBar()->setValue(scroll);
  m_rebuilding = false;
  if (searchFocused)
    m_search->setFocus(Qt::OtherFocusReason);
  else if (filterFocused)
    m_filter->setFocus(Qt::OtherFocusReason);
}

void DeviceListPage::addFact(QVBoxLayout *layout, const QString &name, const QString &value)
{
  auto row = new QHBoxLayout;
  row->setSpacing(15);
  row->setContentsMargins(0, 8, 0, 8);
  auto key = label(name, "muted", m_details);
  auto val = label(value.isEmpty() ? tr("Not available") : value, "value", m_details);
  val->setTextInteractionFlags(Qt::TextSelectableByMouse);
  val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  val->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  row->addWidget(key);
  row->addWidget(val, 1);
  layout->addLayout(row);
}

void DeviceListPage::rebuildDetail()
{
  auto layout = static_cast<QVBoxLayout *>(m_details->layout());
  // Delete nested rows as well as their widgets. Do not invalidate the active signal sender mid-click.
  const auto clear = [](auto &&self, QLayout *l) -> void {
    while (auto item = l->takeAt(0)) {
      if (item->layout())
        self(self, item->layout());
      if (item->widget()) {
        item->widget()->hide();
        item->widget()->setParent(nullptr);
        item->widget()->deleteLater();
      }
      delete item;
    }
  };
  clear(clear, layout);
  const auto ptr = device();
  if (!ptr) {
    layout->addWidget(label(tr("Select a device to view its details."), "muted", m_details));
    layout->addStretch();
    return;
  }
  const auto d = *ptr;
  const bool local = d.kind == Kind::Local, server = d.kind == Kind::Server;
  const bool connected = d.state == State::Connected;
  auto title = label(d.name, "title", m_details);
  title->setObjectName("deviceDetailName");
  layout->addWidget(title);
  auto state = label(stateText(d.state), "muted", m_details);
  state->setObjectName("deviceDetailState");
  layout->addWidget(state);
  layout->addSpacing(15);
  addFact(layout, tr("Computer name"), d.name);
  addFact(layout, tr("Local address"), d.address);
  if (local)
    addFact(
        layout, tr("Working mode"),
        m_sharing ? tr("Share this computer's keyboard and mouse") : tr("Use another computer's keyboard and mouse")
    );
  else {
    addFact(layout, tr("Device source"), d.source);
    addFact(
        layout, tr("Device purpose"),
        server                     ? tr("Keyboard and mouse server")
        : d.kind == Kind::Receiver ? tr("Receiving computer")
                                   : tr("Connection history")
    );
  }
  layout->addSpacing(20);
  const auto mainText = local       ? tr("View screen arrangement")
                        : server    ? (connected ? tr("Disconnect") : tr("Use this computer's keyboard and mouse"))
                        : connected ? tr("Adjust screen position")
                                    : tr("View connection instructions");
  auto primary = button(mainText, "primary", m_details, server ? HomeIcon::MonitorDown : HomeIcon::Layout);
  primary->setObjectName("devicePrimaryAction");
  primary->setEnabled(!m_busy || (!local && !server));
  // Long translated action labels must not enlarge the entire application at minimum width.
  if (server && !connected)
    primary->setText(tr("Connect to server"));
  layout->addWidget(primary, 0, Qt::AlignHCenter);
  auto hint = label(
      server && !connected ? tr("Connecting switches this computer to receiving mode.")
      : local              ? tr("Set computer positions and the edges used to switch between them.")
      : connected          ? tr("Keyboard and mouse sharing is connected.")
                           : tr("Waiting for this computer to connect."),
      "status", m_details
  );
  hint->setAlignment(Qt::AlignCenter);
  layout->addWidget(hint);
  QCheckBox *autoConnect = nullptr;
  if (server && !connected) {
    autoConnect = new QCheckBox(tr("Connect to this server automatically next time"), m_details);
    autoConnect->setObjectName("deviceAutoConnect");
    autoConnect->setChecked(Settings::value(Settings::Client::AutoConnect).toBool());
    autoConnect->setStyleSheet(QStringLiteral("font-size: 11px; color: #8994a1;"));
    layout->addWidget(autoConnect);
  }
  layout->addSpacing(23);
  layout->addWidget(label(local ? tr("Common actions") : tr("Device actions"), "heading", m_details));
  auto actions = new QHBoxLayout;
  actions->setSpacing(12);
  const auto quick = [&](const QString &text, const QString &name, HomeIcon ico, const std::function<void()> &slot) {
    auto column = new QVBoxLayout;
    auto b = button({}, "action", m_details, ico);
    b->setFixedSize(48, 48);
    b->setIconSize(QSize(25, 25));
    b->setAccessibleName(text);
    b->setObjectName(name);
    auto caption = label(text, "muted", m_details);
    caption->setAlignment(Qt::AlignCenter);
    column->addWidget(b, 0, Qt::AlignHCenter);
    column->addWidget(caption);
    actions->addLayout(column, 1);
    connect(b, &QPushButton::clicked, this, slot);
    return b;
  };
  auto arrange = quick(tr("Screen arrangement"), "deviceLayoutAction", HomeIcon::Layout, [this, d] {
    Q_EMIT layoutRequested(d.kind == Kind::Local ? QString() : d.name);
  });
  arrange->setEnabled(!server);
  if (server)
    arrange->setToolTip(tr("Arrange screens on the server computer."));
  auto settings = quick(
      local    ? tr("Advanced settings")
      : server ? tr("Receiving settings")
               : tr("Device settings"),
      "deviceSettingsAction", HomeIcon::Adjust,
      [this, d, server] {
        if (server)
          Q_EMIT inputSettingsRequested();
        else
          Q_EMIT settingsRequested(d.kind == Kind::Local ? QString() : d.name);
      }
  );
  settings->setEnabled(local || server || d.configured);
  quick(tr("Connection diagnosis"), "deviceDiagnosisAction", HomeIcon::Logs, [this] { Q_EMIT diagnosisRequested(); });
  layout->addLayout(actions);
  layout->addSpacing(24);
  auto context = new QFrame(m_details);
  context->setObjectName("deviceContext");
  auto contextLayout = new QVBoxLayout(context);
  contextLayout->setContentsMargins(15, 14, 15, 14);
  QString contextTitle, contextText;
  if (local) {
    contextTitle = tr("One keyboard and mouse, multiple computers");
    contextText = tr("Arrange computer positions and switching directions in screen arrangement.");
  } else if (d.state == State::Discovered) {
    contextTitle = tr("Discoverable does not mean connected");
    contextText = tr("A server was found on your local network. Connect to establish keyboard and mouse sharing.");
  } else if (connected) {
    contextTitle = tr("Keyboard and mouse sharing is connected");
    contextText =
        server ? tr("This computer receives input from the selected server. Arrange screens on that server.")
               : tr("This computer receives your keyboard and mouse. Adjust its position in screen arrangement.");
  } else {
    contextTitle = tr("Device is not connected");
    contextText = tr("A saved device or connection record does not confirm that the computer is currently available.");
  }
  contextLayout->addWidget(label(contextTitle, "heading", context));
  contextLayout->addWidget(label(contextText, "muted", context));
  layout->addWidget(context);
  auto guide = label(
      tr("On the other computer, choose receiving mode and enter this computer's address. Computer names must match "
         "those in screen arrangement."),
      "muted", m_details
  );
  guide->setObjectName("deviceConnectionGuide");
  guide->hide();
  layout->addWidget(guide);
  connect(primary, &QPushButton::clicked, this, [this, d, server, local, connected, guide, autoConnect] {
    if (server) {
      if (connected)
        Q_EMIT disconnectRequested();
      else
        Q_EMIT connectRequested(d.address, d.name, autoConnect && autoConnect->isChecked());
    } else if (local || connected)
      Q_EMIT layoutRequested(local ? QString() : d.name);
    else
      guide->setVisible(!guide->isVisible());
  });
  layout->addStretch(1);
  layout->addSpacing(24);
  auto info = button(tr("Copy connection information"), "link", m_details, HomeIcon::Copy);
  info->setObjectName("deviceCopyAction");
  connect(info, &QPushButton::clicked, this, [this, d] {
    QApplication::clipboard()->setText(
        tr("Computer name: %1\nAddress: %2").arg(d.name, d.address.isEmpty() ? tr("Not available") : d.address)
    );
    message(tr("Connection information copied"));
  });
  layout->addWidget(info, 0, Qt::AlignRight);
}

void DeviceListPage::showAddForm()
{
  m_addName->clear();
  m_addAddress->clear();
  m_addError->clear();
  m_addKind->setCurrentIndex(m_sharing ? 0 : 1);
  m_detail->setCurrentIndex(1);
  m_addName->setFocus();
}

void DeviceListPage::submitAdd()
{
  const auto name = m_addName->text().trimmed();
  if (name.isEmpty() || !validators::ComputerNameValidator({}).validate(name)) {
    m_addError->setText(tr("Enter a valid computer name: letters, numbers, dots, hyphens or underscores."));
    return;
  }
  if (m_addKind->currentIndex() == 0) {
    Q_EMIT layoutRequested(name);
    return;
  }
  const auto address = normalizedAddress(m_addAddress->text(), Settings::value(Settings::Core::Port).toInt());
  if (address.isEmpty()) {
    m_addError->setText(tr("Enter a valid IP address or hostname and a port from 1 to 65535."));
    return;
  }
  Q_EMIT addServerRequested(name, address);
  m_detail->setCurrentIndex(0);
}

void DeviceListPage::message(const QString &text, bool error)
{
  m_status->setText(text);
  m_status->setStyleSheet(error ? QStringLiteral("color: #bd554b") : QString());
}

void DeviceListPage::setDiscoveryAvailable(bool available)
{
  m_discovery->setText(available ? tr("Local discovery is active") : tr("Local discovery is unavailable"));
}

void DeviceListPage::resizeEvent(QResizeEvent *event)
{
  QWidget::resizeEvent(event);
  const int margin = width() < 650 ? 18 : 28;
  m_details->layout()->setContentsMargins(margin, 27, margin, 22);
}

void DeviceListPage::changeEvent(QEvent *event)
{
  QWidget::changeEvent(event);
  if (event->type() == QEvent::LanguageChange) {
    // Rebuild translated static controls while retaining the selected device and live snapshot.
    auto old = layout();
    while (auto item = old->takeAt(0)) {
      if (item->widget())
        item->widget()->deleteLater();
      delete item;
    }
    delete old;
    build();
    rebuildList();
    rebuildDetail();
  }
}
