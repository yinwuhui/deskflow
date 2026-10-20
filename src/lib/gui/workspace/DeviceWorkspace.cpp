// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "DeviceWorkspace.h"
#include "DeviceCanvas.h"
#include "WorkspaceUi.h"
#include "common/Settings.h"
#include "gui/dialogs/ActionDialog.h"
#include "gui/dialogs/ComputerSettingsDialog.h"
#include "gui/dialogs/HotkeyDialog.h"
#include "gui/widgets/KeySequenceWidget.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <memory>
namespace deskflow::gui::workspace {
namespace {
void renameReferences(HotkeyList &rules, const QMap<QString, QString> &renames)
{
  for (auto &rule : rules)
    for (int i = 0; i < rule.actions().size(); ++i) {
      auto &action = rule.actionAt(i);
      if (renames.contains(action.switchComputerName()))
        action.setSwitchComputerName(renames.value(action.switchComputerName()));
      for (const auto &target : action.typeComputerNames())
        if (renames.contains(target)) {
          action.removeComputer(target);
          action.addComputer(renames.value(target));
        }
    }
}
QString actionSummary(const Action &action)
{
  QString detail;
  switch (action.type()) {
  case 0:
    detail = QObject::tr("按下 %1").arg(action.keySequence().toString());
    break;
  case 1:
    detail = QObject::tr("释放 %1").arg(action.keySequence().toString());
    break;
  case 2:
    detail = QObject::tr("按下并释放 %1").arg(action.keySequence().toString());
    break;
  case 3:
    detail = QObject::tr("切换到 %1").arg(action.switchComputerName());
    break;
  case 4:
    detail =
        QObject::tr("切换到%1电脑")
            .arg(
                QStringList{QObject::tr("左侧"), QObject::tr("右侧"), QObject::tr("上方"), QObject::tr("下方")}.value(
                    action.switchDirection()
                )
            );
    break;
  case 5:
    detail = QObject::tr("切换到下一台电脑");
    break;
  case 6:
    detail = QStringList{QObject::tr("切换光标锁定"), QObject::tr("锁定光标"), QObject::tr("解除光标锁定")}.value(
        action.lockCursorMode()
    );
    break;
  case 7:
    detail = QObject::tr("重启来源端（会中断共享）");
    break;
  default:
    detail = QObject::tr("未知动作，原值保留");
    break;
  }
  if (action.type() < 3)
    detail += action.haveComputers() && !action.typeComputerNames().isEmpty()
                  ? QObject::tr(" → %1").arg(action.typeComputerNames().join(QObject::tr("、")))
                  : QObject::tr(" → 全部电脑");
  return (action.activeOnRelease() ? QObject::tr("释放时：") : QObject::tr("按下时：")) + detail;
}
QMap<QString, QVariant> profileValues()
{
  QMap<QString, QVariant> result;
  auto &settings = Settings::proxy().get();
  for (const auto &key : settings.allKeys())
    result.insert(key, settings.value(key));
  return result;
}
void restoreValues(const QMap<QString, QVariant> &values)
{
  auto &settings = Settings::proxy().get();
  for (const auto &key : settings.allKeys())
    if (!values.contains(key))
      settings.remove(key);
  for (auto it = values.cbegin(); it != values.cend(); ++it)
    settings.setValue(it.key(), it.value());
}
} // namespace
DeviceWorkspace::DeviceWorkspace(ServerConfig &saved, std::function<bool()> persist, QWidget *parent)
    : QWidget(parent),
      m_saved(saved),
      m_draft(saved),
      m_base(saved),
      m_model(m_draft.m_computers, saved.m_columns, saved.m_rows),
      m_persist(std::move(persist))
{
  setObjectName("deviceWorkspace");
  build();
  connect(&m_model, &ComputerSetupModel::computersChanged, this, [this] {
    m_draft.m_columns = m_model.columnCount();
    m_draft.m_rows = m_model.rowCount();
    QTimer::singleShot(0, this, [this] { sync(); });
  });
  sync();
  QTimer::singleShot(0, m_canvas, &DeviceCanvas::centerSelected);
}
void DeviceWorkspace::build()
{
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(20);
  layout->setAlignment(Qt::AlignTop);
  auto *header = new QHBoxLayout;
  auto *title = new QVBoxLayout;
  title->setSpacing(7);
  title->addWidget(label(tr("完整电脑排列"), "title"));
  title->addWidget(label(tr("每个图块代表一台电脑。可多行多列排列，保留空位和相邻链路。"), "muted"));
  header->addLayout(title, 1);
  m_edit = button(tr("调整排列"), true);
  m_save = button(tr("保存排列"), true);
  m_cancel = button(tr("取消"));
  m_add = button(tr("预先添加电脑…"));
  m_remove = button(tr("移除选中电脑…"));
  for (auto *b : {m_cancel, m_save, m_edit, m_add, m_remove})
    header->addWidget(b, 0, Qt::AlignVCenter);
  layout->addLayout(header);
  connect(m_edit, &QPushButton::clicked, this, &DeviceWorkspace::beginEditing);
  connect(m_save, &QPushButton::clicked, this, [this] { saveLayout(); });
  connect(m_cancel, &QPushButton::clicked, this, &DeviceWorkspace::discardLayout);
  connect(m_add, &QPushButton::clicked, this, [this] { addDevice(); });
  connect(m_remove, &QPushButton::clicked, this, &DeviceWorkspace::removeSelected);
  m_issue = label({}, "error");
  m_issue->hide();
  layout->addWidget(m_issue);
  auto *canvasBox = panel(this);
  auto *canvasLayout = new QVBoxLayout(canvasBox);
  canvasLayout->setContentsMargins(0, 0, 0, 0);
  canvasLayout->setSpacing(0);
  auto *toolbar = new QWidget;
  auto *tools = new QHBoxLayout(toolbar);
  tools->setContentsMargins(12, 10, 12, 10);
  tools->setSpacing(8);
  tools->addWidget(label(tr("选中电脑")));
  m_selection = new QComboBox;
  m_selection->setMinimumWidth(160);
  m_selection->setAccessibleName(tr("选中电脑"));
  tools->addWidget(m_selection);
  tools->addStretch();
  auto *minus = button("−");
  auto *plus = button("＋");
  auto *center = button(tr("居中"));
  auto *zoom = new QLabel("100%");
  for (auto *b : {minus, plus})
    b->setMinimumWidth(32);
  tools->addWidget(minus);
  tools->addWidget(zoom);
  tools->addWidget(plus);
  tools->addWidget(center);
  canvasLayout->addWidget(toolbar);
  m_canvas = new DeviceCanvas;
  canvasLayout->addWidget(m_canvas, 1);
  auto *footer = new QWidget;
  auto *foot = new QHBoxLayout(footer);
  foot->setContentsMargins(12, 10, 12, 10);
  m_footer = label({}, "muted");
  foot->addWidget(m_footer, 1);
  for (const auto &delta : QList<QPair<QString, QPoint>>{
           {tr("← 平移"), {-100, 0}}, {tr("平移 →"), {100, 0}}, {"↑", {0, -90}}, {"↓", {0, 90}}
       }) {
    auto *action = button(delta.first);
    foot->addWidget(action);
    connect(action, &QPushButton::clicked, this, [this, offset = delta.second] {
      m_canvas->pan(offset.x(), offset.y());
    });
  }
  canvasLayout->addWidget(footer);
  layout->addWidget(canvasBox);
  auto *zoomValue = new int(100); // lifetime follows the shared callback below
  auto scale = std::shared_ptr<int>(zoomValue);
  connect(minus, &QPushButton::clicked, this, [this, scale, zoom] {
    *scale = std::max(50, *scale - 10);
    m_canvas->setZoom(*scale);
    zoom->setText(QString::number(*scale) + "%");
  });
  connect(plus, &QPushButton::clicked, this, [this, scale, zoom] {
    *scale = std::min(175, *scale + 10);
    m_canvas->setZoom(*scale);
    zoom->setText(QString::number(*scale) + "%");
  });
  connect(m_canvas, &DeviceCanvas::zoomChanged, this, [scale, zoom](int percent) {
    *scale = percent;
    zoom->setText(QString::number(percent) + "%");
  });
  connect(center, &QPushButton::clicked, m_canvas, &DeviceCanvas::centerSelected);
  connect(m_selection, &QComboBox::currentIndexChanged, this, [this] {
    m_selectedId = m_selection->currentData().toString();
    sync(false);
    m_canvas->centerSelected();
  });
  connect(m_canvas, &DeviceCanvas::selected, this, [this](const QString &id) {
    if (id == m_selectedId)
      return;
    m_selectedId = id;
    sync(false);
  });
  connect(m_canvas, &DeviceCanvas::moved, this, [this](int column, int row, int targetColumn, int targetRow) {
    if (!m_editing || m_external || m_receiver)
      return;
    if (!moveDevice(column, row, targetColumn, targetRow))
      showIssue(tr("排列超过100×100范围，原位置保持保留。"));
  });
  connect(m_canvas, &DeviceCanvas::editRequested, this, &DeviceWorkspace::editSelected);
  auto *selectionRow = new QHBoxLayout;
  auto *selectionText = new QVBoxLayout;
  selectionText->setSpacing(4);
  m_selectionName = label({}, "subtitle");
  m_selectionDescription = label({}, "muted");
  selectionText->addWidget(m_selectionName);
  selectionText->addWidget(m_selectionDescription);
  selectionRow->addLayout(selectionText, 1);
  m_verify = button(tr("验证实际输入"));
  selectionRow->addWidget(m_verify);
  connect(m_verify, &QPushButton::clicked, this, [this] { Q_EMIT requestVerify(selectedIdentifier()); });
  m_details = button(tr("设备高级设置…"));
  selectionRow->addWidget(m_details);
  connect(m_details, &QPushButton::clicked, this, &DeviceWorkspace::editSelected);
  layout->addLayout(selectionRow);
  m_coordinates = panel(this);
  auto *positions = new QHBoxLayout(m_coordinates);
  positions->setContentsMargins(16, 12, 16, 12);
  positions->addWidget(label(tr("精确位置（相对本机）")), 1);
  auto *column = m_positionColumn = new QSpinBox;
  auto *row = m_positionRow = new QSpinBox;
  column->setRange(-99, 99);
  row->setRange(-99, 99);
  column->setAccessibleName(tr("目标列"));
  row->setAccessibleName(tr("目标行"));
  positions->addWidget(label(tr("列")));
  positions->addWidget(column);
  positions->addWidget(label(tr("行")));
  positions->addWidget(row);
  auto *move = button(tr("移动"));
  positions->addWidget(move);
  layout->addWidget(m_coordinates);
  connect(move, &QPushButton::clicked, this, [this, column, row] {
    if (!moveSelectedTo(column->value(), row->value()))
      showIssue(tr("无法移动到该位置，原排列保持保留。"));
  });

  auto *bottom = new QHBoxLayout;
  bottom->addWidget(label(tr("离线电脑仍保留位置；连接、身份与输入验证分别显示。"), "muted"), 1);
  auto *configuration = button(tr("复杂布局与配置文件…"));
  bottom->addWidget(configuration);
  connect(configuration, &QPushButton::clicked, this, &DeviceWorkspace::requestConfiguration);
  layout->addLayout(bottom);
  m_cards = new QWidget;
  layout->addWidget(m_cards);
}
void DeviceWorkspace::showIssue(const QString &message)
{
  m_issue->setText(message);
  m_issue->setVisible(!message.isEmpty());
}
void DeviceWorkspace::sync(bool refreshCanvas)
{
  const QSignalBlocker blocked(m_selection);
  m_selection->clear();
  bool selectedFound = false;
  for (const auto &computer : m_draft.m_computers)
    if (!computer.isNull()) {
      m_selection->addItem(computer.displayName(), computer.stableId());
      selectedFound |= computer.stableId() == m_selectedId;
    }
  if (!selectedFound && !m_draft.m_computers.isEmpty()) {
    auto local = std::find_if(m_draft.m_computers.cbegin(), m_draft.m_computers.cend(), [](const Computer &c) {
      return c.isServer();
    });
    if (local != m_draft.m_computers.cend())
      m_selectedId = local->stableId();
    else if (m_selection->count())
      m_selectedId = m_selection->itemData(0).toString();
  }
  m_selection->setCurrentIndex(m_selection->findData(m_selectedId));
  if (refreshCanvas)
    m_canvas->setDevices(m_draft.m_computers, m_draft.m_columns, m_selectedId, m_editing && !m_external && !m_receiver);
  else
    m_canvas->selectDevice(m_selectedId);
  for (const auto &computer : m_draft.m_computers)
    if (!computer.isNull() && computer.stableId() == m_selectedId) {
      m_selectionName->setText(computer.displayName());
      m_selectionDescription->setText(
          computer.isServer() ? tr("键盘与鼠标来源") : tr("连接标识：%1").arg(computer.name())
      );
      m_remove->setEnabled(!computer.isServer() && !m_external && !m_receiver && Settings::isWritable());
      break;
    }
  int originX = 0, originY = 0;
  for (int i = 0; i < m_draft.m_computers.size(); ++i)
    if (m_draft.m_computers[i].isServer()) {
      originX = i % m_draft.m_columns;
      originY = i / m_draft.m_columns;
      break;
    }
  for (int i = 0; i < m_draft.m_computers.size(); ++i)
    if (m_draft.m_computers[i].stableId() == m_selectedId) {
      m_positionColumn->setValue(i % m_draft.m_columns - originX);
      m_positionRow->setValue(i / m_draft.m_columns - originY);
      break;
    }
  const bool canEdit = Settings::isWritable() && !m_external && !m_receiver;
  m_edit->setVisible(!m_editing);
  m_edit->setEnabled(canEdit);
  m_save->setVisible(m_editing);
  m_cancel->setVisible(m_editing);
  m_save->setEnabled(canEdit);
  m_add->setEnabled(canEdit);
  m_details->setEnabled(canEdit);
  m_coordinates->setVisible(m_editing);
  m_footer->setText(tr("共%1台电脑 · 可选择任意电脑并居中查看").arg(m_selection->count()));
  if (m_external)
    showIssue(tr("排列由当前生效的外部配置文件管理。请到配置文件页修改。"));
  updateCards();
}
void DeviceWorkspace::updateCards()
{
  m_verify->setEnabled(m_connected.contains(selectedIdentifier()));
  if (auto *old = m_cards->layout()) {
    while (auto *item = old->takeAt(0)) {
      if (item->widget())
        item->widget()->deleteLater();
      delete item;
    }
    delete old;
  }
  auto *cards = new QGridLayout(m_cards);
  cards->setContentsMargins(0, 0, 0, 0);
  cards->setSpacing(8);
  int index = 0;
  const auto columns = std::clamp(width() / 175, 1, 5);
  for (const auto &computer : m_draft.m_computers) {
    if (computer.isNull() || computer.isServer())
      continue;
    if (index >= 10)
      break;
    auto *card = button({}, false, m_cards);
    auto *content = new QVBoxLayout(card);
    content->setContentsMargins(12, 10, 12, 10);
    content->setSpacing(5);
    content->addWidget(label(computer.displayName(), "subtitle"));
    const auto session = m_sessions.value(computer.name());
    const bool online = m_connected.contains(computer.name());
    content->addWidget(label(
        (online ? tr("已连接") : tr("离线")) + QString(" · ") +
            (session.manuallyVerifiedAt.isValid()
                 ? (online && session.verificationAttempt == session.attempt ? tr("人工验证") : tr("曾验证输入"))
                 : tr("输入未验证")),
        "muted"
    ));
    card->setAccessibleName(computer.displayName() + "，" + (online ? tr("已连接") : tr("离线")));
    connect(card, &QPushButton::clicked, this, &DeviceWorkspace::requestTrust);
    cards->addWidget(card, index / columns, index % columns);
    ++index;
  }
}

bool DeviceWorkspace::hasUnsavedChanges() const
{
  return m_draft.m_computers != m_saved.m_computers || m_draft.m_Hotkeys != m_saved.m_Hotkeys ||
         m_draft.m_columns != m_saved.m_columns || m_draft.m_rows != m_saved.m_rows;
}
void DeviceWorkspace::reloadSaved()
{
  m_draft = m_saved;
  m_base = m_saved;
  m_model.resetGrid(m_draft.m_columns, m_draft.m_rows);
  m_editing = false;
  sync();
  updateRuleRows();
}
void DeviceWorkspace::setSessionState(
    const QStringList &connected, const QMap<QString, ApplicationState::Session> &sessions, bool external,
    bool receiver, const QString &source
)
{
  Q_UNUSED(source);
  const bool managementChanged = m_external != external || m_receiver != receiver;
  bool changed = m_connected != connected || m_sessions.size() != sessions.size();
  if (!changed)
    for (auto it = sessions.cbegin(); it != sessions.cend(); ++it) {
      const auto old = m_sessions.value(it.key());
      if (old.attempt != it->attempt || old.manuallyVerifiedAt != it->manuallyVerifiedAt ||
          old.verificationAttempt != it->verificationAttempt) {
        changed = true;
        break;
      }
    }
  m_connected = connected;
  m_sessions = sessions;
  m_external = external;
  m_receiver = receiver;
  if (managementChanged) {
    sync();
    updateRuleRows();
  } else if (changed)
    updateCards();
}
bool DeviceWorkspace::moveDevice(int fromColumn, int fromRow, int toColumn, int toRow)
{
  if (!m_editing || m_external || m_receiver || !Settings::isWritable())
    return false;
  const auto oldColumns = m_model.columnCount(), oldRows = m_model.rowCount();
  const int shiftX = std::max(0, -toColumn), shiftY = std::max(0, -toRow);
  const int columns = std::max(oldColumns + shiftX, toColumn + shiftX + 1),
            rows = std::max(oldRows + shiftY, toRow + shiftY + 1);
  if (columns > 100 || rows > 100)
    return false;
  if (shiftX || shiftY) {
    ComputerList expanded(columns);
    expanded.resize(columns * rows);
    for (int y = 0; y < oldRows; ++y)
      for (int x = 0; x < oldColumns; ++x)
        expanded[(y + shiftY) * columns + x + shiftX] = m_draft.m_computers[y * oldColumns + x];
    m_draft.m_computers = expanded;
    m_model.resetGrid(columns, rows);
  } else if ((columns != oldColumns || rows != oldRows) && !m_model.resizeGrid(columns, rows))
    return false;
  return m_model.moveComputer(fromColumn + shiftX, fromRow + shiftY, toColumn + shiftX, toRow + shiftY);
}
bool DeviceWorkspace::moveSelectedTo(int column, int row)
{
  int originX = 0, originY = 0;
  for (int i = 0; i < m_draft.m_computers.size(); ++i)
    if (m_draft.m_computers[i].isServer()) {
      originX = i % m_model.columnCount();
      originY = i / m_model.columnCount();
      break;
    }
  for (int i = 0; i < m_draft.m_computers.size(); ++i)
    if (m_draft.m_computers[i].stableId() == m_selectedId)
      return moveDevice(i % m_model.columnCount(), i / m_model.columnCount(), originX + column, originY + row);
  return false;
}

void DeviceWorkspace::beginEditing()
{
  if (m_external || m_receiver || !Settings::isWritable())
    return;
  m_editing = true;
  showIssue({});
  sync();
}
void DeviceWorkspace::discardLayout()
{
  m_base.m_computers = m_saved.m_computers;
  m_base.m_columns = m_saved.m_columns;
  m_base.m_rows = m_saved.m_rows;
  m_draft.m_computers = m_saved.m_computers;
  m_draft.m_columns = m_saved.m_columns;
  m_draft.m_rows = m_saved.m_rows;
  m_model.resetGrid(m_draft.m_columns, m_draft.m_rows);
  m_editing = false;
  showIssue({});
  sync();
}
QString DeviceWorkspace::selectedIdentifier() const
{
  for (const auto &computer : m_draft.m_computers)
    if (computer.stableId() == m_selectedId)
      return computer.name();
  return {};
}
void DeviceWorkspace::addDevice(const QString &identifier)
{
  if (m_external || m_receiver || !Settings::isWritable())
    return;
  beginEditing();
  Computer computer(identifier);
  ComputerSettingsDialog dialog(this, &computer, &m_draft.m_computers);
  dialog.setObjectName("workspaceRoot");
  applyTheme(&dialog);
  if (dialog.exec() != QDialog::Accepted)
    return;
  if (m_model.isFull()) {
    const auto columns = m_model.columnCount();
    const auto rows = m_model.rowCount();
    if ((columns >= 100 && rows >= 100) ||
        !m_model.resizeGrid(columns < 100 ? columns + 1 : columns, columns < 100 ? rows : rows + 1)) {
      showIssue(tr("当前排列已满，最多支持 100 × 100 个位置。"));
      return;
    }
  }
  m_draft.m_computers.addComputerByPriority(computer);
  m_selectedId = computer.stableId();
  m_model.resetGrid(m_model.columnCount(), m_model.rowCount());
  sync();
  m_canvas->centerSelected();
}
void DeviceWorkspace::editSelected()
{
  if (m_external || m_receiver || !Settings::isWritable())
    return;
  for (auto &computer : m_draft.m_computers)
    if (!computer.isNull() && computer.stableId() == m_selectedId) {
      auto edited = computer;
      ComputerSettingsDialog dialog(this, &edited, &m_draft.m_computers);
      dialog.setObjectName("workspaceRoot");
      applyTheme(&dialog);
      if (dialog.exec() == QDialog::Accepted) {
        computer = edited;
        beginEditing();
        sync();
      }
      break;
    }
}
void DeviceWorkspace::removeSelected()
{
  if (m_external || m_receiver || !Settings::isWritable())
    return;
  for (auto &computer : m_draft.m_computers)
    if (!computer.isNull() && computer.stableId() == m_selectedId && !computer.isServer()) {
      for (const auto &rule : m_draft.m_Hotkeys)
        for (const auto &action : rule.actions())
          if (action.switchComputerName() == computer.name() || action.typeComputerNames().contains(computer.name())) {
            showIssue(tr("快捷键仍引用这台电脑。请先在快捷键页重新指定目标，再移除。"));
            return;
          }
      if (QMessageBox::question(
              this, tr("移除电脑"), tr("从排列中移除 %1？信任记录会保留。").arg(computer.displayName()),
              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel
          ) != QMessageBox::Yes)
        return;
      computer = Computer();
      beginEditing();
      sync();
      return;
    }
}
bool DeviceWorkspace::validate(const ComputerList &computers, const HotkeyList &rules, QString &error) const
{
  QSet<QString> identifiers;
  for (const auto &computer : computers) {
    if (computer.isNull())
      continue;
    auto names = computer.aliases();
    names.prepend(computer.name());
    for (const auto &name : names) {
      const auto key = name.toCaseFolded();
      if (identifiers.contains(key)) {
        error = tr("标识与别名必须全局唯一：%1").arg(name);
        return false;
      }
      identifiers.insert(key);
    }
  }
  QSet<QString> shortcuts;
  for (const auto &rule : rules) {
    const auto combination = rule.keySequence().toString();
    if (shortcuts.contains(combination)) {
      error = tr("快捷键重复：%1").arg(combination);
      return false;
    }
    shortcuts.insert(combination);
    if (rule.actions().isEmpty()) {
      error = tr("每条规则至少需要一个有效动作。");
      return false;
    }
    for (const auto &action : rule.actions()) {
      if (!action.isValid()) {
        error = tr("规则包含无法识别的动作，原值保持保留。请先修正规则。");
        return false;
      }
      auto targets = action.typeComputerNames();
      if (!action.switchComputerName().isEmpty())
        targets.append(action.switchComputerName());
      for (const auto &target : targets)
        if (target != "*" && !identifiers.contains(target.toCaseFolded())) {
          error = tr("动作引用了不存在的电脑：%1。请重新指定目标。").arg(target);
          return false;
        }
    }
  }
  return true;
}
bool DeviceWorkspace::saveLayout()
{
  if (m_external || m_receiver || !Settings::isWritable())
    return false;
  if (m_base.m_computers != m_saved.m_computers || m_base.m_columns != m_saved.m_columns ||
      m_base.m_rows != m_saved.m_rows) {
    showIssue(tr("已保存排列在编辑期间发生变化。请保留当前草稿并重新核对，避免覆盖其他修改。"));
    return false;
  }
  QMap<QString, QString> renames;
  for (const auto &old : m_saved.m_computers)
    if (!old.isNull())
      for (const auto &current : m_draft.m_computers)
        if (!current.isNull() && old.stableId() == current.stableId() && old.name() != current.name())
          renames.insert(old.name(), current.name());
  auto rules = m_saved.m_Hotkeys;
  renameReferences(rules, renames);
  QString error;
  if (!validate(m_draft.m_computers, rules, error)) {
    showIssue(error);
    return false;
  }
  const auto unreachable = m_draft.unreachableComputers();
  if (!unreachable.isEmpty() && QMessageBox::question(
                                    this, tr("部分电脑没有边缘通路"),
                                    tr("以下电脑无法沿相邻边缘到达：%1。可调整排列，或明确保留用于其他切换规则。")
                                        .arg(unreachable.join(tr("、"))),
                                    QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel
                                ) != QMessageBox::Save)
    return false;
  const auto original = m_saved;
  const auto checkpoint = profileValues();
  m_saved.m_computers = m_draft.m_computers;
  m_saved.m_columns = m_draft.m_columns;
  m_saved.m_rows = m_draft.m_rows;
  m_saved.m_Hotkeys = rules;
  Settings::setValue(Settings::Server::GridWidth, m_saved.m_columns);
  Settings::setValue(Settings::Server::GridHeight, m_saved.m_rows);
  auto &profile = Settings::proxy().get();
  for (auto it = renames.cbegin(); it != renames.cend(); ++it) {
    const auto prefix = "computer_" + it.key() + "/";
    for (auto v = checkpoint.cbegin(); v != checkpoint.cend(); ++v)
      if (v.key().startsWith(prefix))
        profile.setValue("computer_" + it.value() + "/" + v.key().mid(prefix.size()), v.value());
  }
  for (const auto &old : original.m_computers)
    if (!old.isNull() &&
        std::none_of(m_saved.m_computers.cbegin(), m_saved.m_computers.cend(), [&old](const Computer &c) {
          return c.name() == old.name();
        }))
      profile.remove("computer_" + old.name());
  if (!m_persist()) {
    m_saved = original;
    restoreValues(checkpoint);
    showIssue(tr("保存未完成。当前草稿保留，未提交新的运行配置。"));
    return false;
  }
  renameReferences(m_draft.m_Hotkeys, renames);
  auto baseRules = m_base.m_Hotkeys;
  renameReferences(baseRules, renames);
  m_base = m_saved;
  m_base.m_Hotkeys = baseRules;
  m_editing = false;
  showIssue({});
  sync();
  updateRuleRows();
  return true;
}
bool DeviceWorkspace::saveRules()
{
  if (m_external || m_receiver || !Settings::isWritable())
    return false;
  QString error;
  if (!validate(m_saved.m_computers, m_draft.m_Hotkeys, error)) {
    showIssue(error);
    return false;
  }
  if (m_saved.m_Hotkeys != m_base.m_Hotkeys) {
    showIssue(tr("已保存的快捷键已变化，请先重新载入，避免覆盖其他更改。"));
    return false;
  }
  const auto checkpoint = profileValues();
  const auto original = m_saved.m_Hotkeys;
  m_saved.m_Hotkeys = m_draft.m_Hotkeys;
  if (!m_persist()) {
    m_saved.m_Hotkeys = original;
    restoreValues(checkpoint);
    showIssue(tr("快捷键保存失败，草稿保留。"));
    return false;
  }
  m_base.m_Hotkeys = m_saved.m_Hotkeys;
  updateRuleRows();
  return true;
}
QWidget *DeviceWorkspace::createRulesPage()
{
  auto *add = button(tr("新建规则"), true);
  auto *result = page(tr("高级快捷键动作"), tr("每条规则保留触发按键、按下／释放事件与完整动作列表。"), add);
  m_rulesContent = new QWidget(result);
  m_ruleRows = new QVBoxLayout(m_rulesContent);
  m_ruleRows->setContentsMargins(0, 0, 0, 0);
  m_ruleRows->setSpacing(12);
  contents(result)->addWidget(m_rulesContent);
  auto *actions = new QHBoxLayout;
  actions->addStretch();
  auto *cancel = button(tr("放弃快捷键更改"));
  auto *save = button(tr("保存快捷键配置"), true);
  save->setObjectName("saveRules");
  actions->addWidget(cancel);
  actions->addWidget(save);
  contents(result)->addLayout(actions);
  connect(add, &QPushButton::clicked, this, [this] { editRule(-1); });
  connect(save, &QPushButton::clicked, this, [this] {
    if (!saveRules())
      QMessageBox::warning(this, tr("快捷键尚未保存"), m_issue->text());
  });
  connect(cancel, &QPushButton::clicked, this, [this] {
    m_draft.m_Hotkeys = m_saved.m_Hotkeys;
    updateRuleRows();
  });
  updateRuleRows();
  return result;
}
void DeviceWorkspace::updateRuleRows()
{
  if (!m_ruleRows)
    return;
  while (auto *item = m_ruleRows->takeAt(0)) {
    if (item->widget())
      item->widget()->deleteLater();
    delete item;
  }
  if (m_draft.m_Hotkeys.isEmpty())
    m_ruleRows->addWidget(label(tr("还没有快捷键规则。新建规则后，可组合切换、按键与锁定等动作。"), "muted"));
  for (int i = 0; i < m_draft.m_Hotkeys.size(); ++i) {
    const auto &rule = m_draft.m_Hotkeys[i];
    auto *card = panel();
    auto *layout = new QHBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    auto *summary = new QVBoxLayout;
    summary->setSpacing(6);
    summary->addWidget(label(rule.displayName().isEmpty() ? tr("快捷键 %1").arg(i + 1) : rule.displayName(), "subtitle")
    );
    summary->addWidget(label(tr("%1 · %2个动作").arg(rule.keySequence().toString()).arg(rule.actions().size()), "muted")
    );
    QStringList actions;
    for (const auto &action : rule.actions())
      actions.append(actionSummary(action));
    summary->addWidget(label(actions.join(tr("；")), "muted"));
    layout->addLayout(summary, 1);
    auto *edit = button(tr("编辑…"));
    auto *remove = button(tr("删除"));
    remove->setProperty("danger", true);
    layout->addWidget(edit);
    layout->addWidget(remove);
    edit->setEnabled(!m_external && !m_receiver && Settings::isWritable());
    remove->setEnabled(edit->isEnabled());
    connect(edit, &QPushButton::clicked, this, [this, i] { editRule(i); });
    connect(remove, &QPushButton::clicked, this, [this, i] {
      if (i >= m_draft.m_Hotkeys.size())
        return;
      if (QMessageBox::question(
              this, tr("删除规则"), tr("从草稿中删除此规则？保存后才会更新配置。"),
              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel
          ) == QMessageBox::Yes) {
        m_draft.m_Hotkeys.removeAt(i);
        updateRuleRows();
      }
    });
    m_ruleRows->addWidget(card);
  }
}
void DeviceWorkspace::editRule(int index)
{
  if (m_external || m_receiver || !Settings::isWritable())
    return;
  Hotkey rule = index >= 0 && index < m_draft.m_Hotkeys.size() ? m_draft.m_Hotkeys[index] : Hotkey();
  QDialog dialog(this);
  dialog.setObjectName("workspaceRoot");
  dialog.setWindowTitle(index < 0 ? tr("新建快捷键规则") : tr("编辑快捷键规则"));
  dialog.resize(610, 570);
  auto *layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(24, 22, 24, 22);
  layout->setSpacing(16);
  layout->addWidget(label(dialog.windowTitle(), "title"));
  auto *name = new QLineEdit(rule.displayName());
  name->setPlaceholderText(tr("规则名称"));
  name->setAccessibleName(tr("规则名称"));
  layout->addWidget(name);
  auto *record = new KeySequenceWidget(&dialog, rule.keySequence());
  record->setAccessibleName(tr("录制触发组合"));
  auto *recordControls = new QWidget;
  auto *recordingRow = new QHBoxLayout(recordControls);
  recordingRow->setContentsMargins(0, 0, 0, 0);
  recordingRow->addWidget(record, 1);
  auto *cancelRecording = button(tr("取消录制"));
  recordingRow->addWidget(cancelRecording);
  connect(cancelRecording, &QPushButton::clicked, record, &KeySequenceWidget::cancelRecording);
  layout->addWidget(row(tr("触发组合"), tr("支持键盘与鼠标；取消录制恢复先前组合"), recordControls));
  auto *list = new QListWidget;
  list->setAccessibleName(tr("有序动作列表"));
  layout->addWidget(list, 1);
  auto refresh = [&rule, list] {
    const auto selected = list->currentRow();
    list->clear();
    for (const auto &action : rule.actions())
      list->addItem(actionSummary(action));
    if (list->count())
      list->setCurrentRow(std::clamp(selected, 0, list->count() - 1));
  };
  refresh();
  auto *actions = new QHBoxLayout;
  auto *add = button(tr("添加动作"));
  auto *edit = button(tr("编辑动作"));
  auto *remove = button(tr("移除动作"));
  auto *up = button(tr("上移"));
  auto *down = button(tr("下移"));
  for (auto *b : {add, edit, remove, up, down})
    actions->addWidget(b);
  layout->addLayout(actions);
  auto *error = label({}, "error");
  layout->addWidget(error);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  buttons->button(QDialogButtonBox::Save)->setText(tr("保存到规则草稿"));
  buttons->button(QDialogButtonBox::Save)->setProperty("primary", true);
  buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
  layout->addWidget(buttons);
  connect(add, &QPushButton::clicked, &dialog, [this, &dialog, &rule, record, refresh] {
    rule.setKeySequence(record->keySequence());
    Action action;
    ActionDialog editor(&dialog, m_draft, rule, action);
    editor.setObjectName("workspaceRoot");
    applyTheme(&editor);
    if (editor.exec() == QDialog::Accepted) {
      rule.addAction(action);
      refresh();
    }
  });
  connect(edit, &QPushButton::clicked, &dialog, [this, &dialog, &rule, list, refresh] {
    const auto selected = list->currentRow();
    if (selected < 0 || selected >= rule.actions().size())
      return;
    auto action = rule.actions()[selected];
    ActionDialog editor(&dialog, m_draft, rule, action);
    editor.setObjectName("workspaceRoot");
    applyTheme(&editor);
    if (editor.exec() == QDialog::Accepted) {
      rule.actionAt(selected) = action;
      refresh();
    }
  });
  connect(remove, &QPushButton::clicked, &dialog, [&rule, list, refresh] {
    const auto selected = list->currentRow();
    if (selected >= 0 && selected < rule.actions().size()) {
      rule.removeActionAt(selected);
      refresh();
    }
  });
  for (const auto &move : QList<QPair<QPushButton *, int>>{{up, -1}, {down, 1}})
    connect(move.first, &QPushButton::clicked, &dialog, [&rule, list, refresh, delta = move.second] {
      const auto selected = list->currentRow(), target = selected + delta;
      if (selected < 0 || target < 0 || target >= rule.actions().size())
        return;
      std::swap(rule.actionAt(selected), rule.actionAt(target));
      refresh();
      list->setCurrentRow(target);
    });
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, [this, index, &dialog, &rule, record, name, error] {
    if (!record->valid()) {
      error->setText(tr("请先录制有效的触发组合。"));
      return;
    }
    for (int i = 0; i < m_draft.m_Hotkeys.size(); ++i)
      if (i != index && m_draft.m_Hotkeys[i].keySequence() == record->keySequence()) {
        error->setText(tr("该组合已被其他规则使用。请更换组合或合并动作。"));
        return;
      }
    if (rule.actions().isEmpty()) {
      error->setText(tr("请至少添加一个有效动作。"));
      return;
    }
    bool restarts = false;
    for (const auto &action : rule.actions())
      restarts |= action.restartServer() || action.type() == static_cast<int>(Action::Type::restartAllConnections);
    if (restarts && QMessageBox::warning(
                        &dialog, tr("包含重启动作"), tr("该规则触发时会重启来源端连接。保存不会执行，确认保留此动作？"),
                        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel
                    ) != QMessageBox::Yes)
      return;
    rule.setKeySequence(record->keySequence());
    rule.setDisplayName(name->text().trimmed());
    dialog.accept();
  });
  applyTheme(&dialog);
  if (dialog.exec() == QDialog::Accepted) {
    if (index < 0)
      m_draft.m_Hotkeys.append(rule);
    else
      m_draft.m_Hotkeys[index] = rule;
    updateRuleRows();
  }
}
} // namespace deskflow::gui::workspace
