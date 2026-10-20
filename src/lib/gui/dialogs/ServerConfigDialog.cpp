/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerConfigDialog.h"
#include "ui_ServerConfigDialog.h"

#include "common/Constants.h"
#include "common/NetworkProtocol.h"
#include "common/PlatformInfo.h"
#include "common/Settings.h"
#include "dialogs/ActionDialog.h"
#include "dialogs/ComputerSettingsDialog.h"
#include "dialogs/HotkeyDialog.h"
#include "gui/widgets/SettingsDialogButtonBox.h"

#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>

using enum ComputerConfig::SwitchCorner;

ServerConfigDialog::ServerConfigDialog(QWidget *parent, ServerConfig &config)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
      ui{std::make_unique<Ui::ServerConfigDialog>()},
      m_columns{config.gridWidth()},
      m_rows{config.gridHeight()},
      m_originalServerConfig(config),
      m_serverConfig(config),
      m_computerSetupModel(m_serverConfig.computers(), m_columns, m_rows),
      m_buttonBox{new SettingsDialogButtonBox(this)}
{
  ui->setupUi(this);
  ui->tabWidget->setCurrentIndex(0);
  layout()->addWidget(m_buttonBox);

  loadFromConfig();
  auto *gridControls = new QHBoxLayout;
  auto *columns = new QSpinBox(this);
  columns->setRange(1, 100);
  columns->setValue(m_columns);
  columns->setAccessibleName(tr("Grid columns"));
  auto *rows = new QSpinBox(this);
  rows->setRange(1, 100);
  rows->setValue(m_rows);
  rows->setAccessibleName(tr("Grid rows"));
  auto *resize = new QPushButton(tr("Resize layout"), this);
  resize->setProperty("graphEditor", true);
  columns->setProperty("graphEditor", true);
  rows->setProperty("graphEditor", true);
  gridControls->addWidget(new QLabel(tr("Columns")));
  gridControls->addWidget(columns);
  gridControls->addWidget(new QLabel(tr("Rows")));
  gridControls->addWidget(rows);
  gridControls->addWidget(resize);
  layout()->addItem(gridControls);
  auto *zoom = new QSpinBox(this);
  zoom->setRange(50, 200);
  zoom->setSingleStep(10);
  zoom->setSuffix("%");
  zoom->setValue(100);
  zoom->setAccessibleName(tr("排列缩放"));
  gridControls->addWidget(new QLabel(tr("缩放")));
  gridControls->addWidget(zoom);
  connect(zoom, &QSpinBox::valueChanged, ui->computerSetupView, &ComputerSetupView::setZoomPercent);

  connect(resize, &QPushButton::clicked, this, [this, columns, rows] {
    if (!model().resizeGrid(columns->value(), rows->value())) {
      QMessageBox::warning(
          this, tr("Cannot resize"), tr("Move computers inside the requested bounds before shrinking the layout.")
      );
      return;
    }
    m_columns = columns->value();
    m_rows = rows->value();
    m_serverConfig.m_columns = m_columns;
    m_serverConfig.m_rows = m_rows;
    setButtonBoxEnabledButtons();
  });
  auto *actionOrder = new QHBoxLayout;
  auto *up = new QPushButton(tr("Move selected action up"));
  auto *down = new QPushButton(tr("Move selected action down"));
  up->setProperty("graphEditor", true);
  down->setProperty("graphEditor", true);
  actionOrder->addWidget(up);
  actionOrder->addWidget(down);
  layout()->addItem(actionOrder);
  auto moveAction = [this](int delta) {
    const auto hotkeyIndex = ui->listHotkeys->currentRow();
    const auto actionIndex = ui->listActions->currentRow();
    if (hotkeyIndex < 0 || hotkeyIndex >= m_serverConfig.hotkeys().size())
      return;
    auto &hotkey = m_serverConfig.hotkeys()[hotkeyIndex];
    const auto target = actionIndex + delta;
    if (actionIndex < 0 || target < 0 || target >= hotkey.actions().size())
      return;
    std::swap(hotkey.actionAt(actionIndex), hotkey.actionAt(target));
    ui->listActions->clear();
    for (const auto &action : hotkey.actions())
      ui->listActions->addItem(action.text());
    ui->listActions->setCurrentRow(target);
    setButtonBoxEnabledButtons();
  };
  connect(up, &QPushButton::clicked, this, [moveAction] { moveAction(-1); });
  connect(down, &QPushButton::clicked, this, [moveAction] { moveAction(1); });
  auto *positions = new QTreeWidget(this);
  positions->setMaximumHeight(150);
  positions->setHeaderLabels({tr("Computer"), tr("Column"), tr("Row")});
  layout()->addWidget(positions);
  auto *coordinates = new QHBoxLayout;
  auto *targetColumn = new QSpinBox;
  auto *targetRow = new QSpinBox;
  targetColumn->setRange(0, 99);
  targetRow->setRange(0, 99);
  targetColumn->setAccessibleName(tr("目标列，从零开始"));
  targetRow->setAccessibleName(tr("目标行，从零开始"));
  auto *move = new QPushButton(tr("移动所选电脑（占用位置将交换）"));
  move->setProperty("graphEditor", true);
  coordinates->addWidget(new QLabel(tr("目标列")));
  coordinates->addWidget(targetColumn);
  coordinates->addWidget(new QLabel(tr("目标行")));
  coordinates->addWidget(targetRow);
  coordinates->addWidget(move);
  layout()->addItem(coordinates);
  connect(move, &QPushButton::clicked, this, [this, targetColumn, targetRow] {
    const auto selected = ui->computerSetupView->currentIndex();
    if (!selected.isValid() ||
        !model().moveComputer(selected.column(), selected.row(), targetColumn->value(), targetRow->value())) {
      QMessageBox::information(this, tr("无法移动"), tr("请先在排列或位置列表选择电脑，并填写当前网格内的坐标。"));
      return;
    }
    const auto destination = model().index(targetRow->value(), targetColumn->value());
    ui->computerSetupView->setCurrentIndex(destination);
    static_cast<QTableView *>(ui->computerSetupView)->scrollTo(destination, QAbstractItemView::PositionAtCenter);
  });
  auto populate = [this, positions] {
    positions->clear();
    for (int row = 0; row < model().rowCount(); ++row)
      for (int column = 0; column < model().columnCount(); ++column) {
        const auto name = model().data(model().index(row, column), Qt::DisplayRole).toString();
        if (!name.isEmpty())
          new QTreeWidgetItem(positions, {name, QString::number(column), QString::number(row)});
      }
  };
  populate();
  connect(&m_computerSetupModel, &ComputerSetupModel::computersChanged, this, populate);
  connect(positions, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
    const auto index = model().index(item->text(2).toInt(), item->text(1).toInt());
    ui->computerSetupView->setCurrentIndex(index);
    static_cast<QTableView *>(ui->computerSetupView)->scrollTo(index);
    ui->computerSetupView->setFocus();
  });

  ui->lblRemoveComputer->setPixmap(QIcon::fromTheme("user-trash").pixmap(QSize(64, 64)));
  ui->lblNewComputer->setEnabled(!model().isFull());
  ui->lblNewComputer->setPixmap(QIcon::fromTheme("video-display").pixmap(QSize(64, 64)));
  ui->btnBrowseConfigFile->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::DocumentOpen));

  if (!deskflow::platform::isWindows())
    ui->cbWin32KeepForeground->setVisible(false);
  initConnections();
}

ServerConfigDialog::~ServerConfigDialog() = default;

bool ServerConfigDialog::addClient(const QString &clientName)
{
  return addComputer(clientName, true);
}

void ServerConfigDialog::save()
{
  if (ui->groupExternalConfig->isChecked() && !QFile::exists(ui->lineConfigFile->text())) {

    auto selectedButton = QMessageBox::warning(
        this, "Filename invalid", "Please select a valid configuration file.", QMessageBox::Ok | QMessageBox::Ignore
    );

    if (selectedButton != QMessageBox::Ok || !browseConfigFile()) {
      return;
    }
  }
  QSet<QString> identifiers;
  for (const auto &computer : std::as_const(m_serverConfig).computers()) {
    if (computer.isNull())
      continue;
    auto names = computer.aliases();
    names.prepend(computer.name());
    for (const auto &name : names) {
      const auto key = name.toCaseFolded();
      if (identifiers.contains(key)) {
        QMessageBox::warning(
            this, tr("Duplicate identifier"),
            tr("Computer identifiers and aliases must be globally unique: %1").arg(name)
        );
        return;
      }
      identifiers.insert(key);
    }
  }
  if (!ui->groupExternalConfig->isChecked()) {
    const auto unreachable = m_serverConfig.unreachableComputers();
    if (!unreachable.isEmpty() &&
        QMessageBox::question(
            this, tr("部分电脑没有边缘通路"),
            tr("以下电脑无法从本机沿相邻边缘到达：\n%1\n如果计划使用快捷键或其他路由，可以明确保留。继续保存？")
                .arg(unreachable.join(", ")),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel
        ) != QMessageBox::Yes)
      return;
  }
  QSet<QString> shortcuts;
  for (const auto &hotkey : std::as_const(m_serverConfig).hotkeys()) {
    const auto combination = hotkey.keySequence().toString();
    if (shortcuts.contains(combination)) {
      QMessageBox::warning(
          this, tr("快捷键冲突"), tr("重复的快捷键：%1。请合并动作或更改组合后保存。").arg(combination)
      );
      return;
    }
    shortcuts.insert(combination);
  }
  QMap<QString, QString> renames;
  const auto originals = std::as_const(m_originalServerConfig).computers();
  for (const auto &oldComputer : originals) {
    if (oldComputer.isNull())
      continue;
    for (const auto &newComputer : std::as_const(m_serverConfig).computers()) {
      if (oldComputer.stableId() == newComputer.stableId() && !newComputer.isNull() &&
          oldComputer.name() != newComputer.name())
        renames.insert(oldComputer.name(), newComputer.name());
    }
  }
  for (auto &hotkey : m_serverConfig.hotkeys()) {
    for (int index = 0; index < hotkey.actions().size(); ++index) {
      auto &action = hotkey.actionAt(index);
      if (renames.contains(action.switchComputerName()))
        action.setSwitchComputerName(renames.value(action.switchComputerName()));
      const auto targets = action.typeComputerNames();
      for (const auto &target : targets)
        if (renames.contains(target)) {
          action.removeComputer(target);
          action.addComputer(renames.value(target));
        }
    }
  }
  for (const auto &hotkey : std::as_const(m_serverConfig).hotkeys())
    for (const auto &action : hotkey.actions()) {
      auto targets = action.typeComputerNames();
      if (!action.switchComputerName().isEmpty())
        targets.append(action.switchComputerName());
      for (const auto &target : targets)
        if (target != "*" && !identifiers.contains(target.toCaseFolded())) {
          QMessageBox::warning(
              this, tr("Referenced computer removed"),
              tr("A shortcut still references %1. Reassign or remove that action before saving.").arg(target)
          );
          return;
        }
    }
  Settings::setValue(Settings::Server::GridWidth, m_columns);
  Settings::setValue(Settings::Server::GridHeight, m_rows);
  auto &profile = Settings::proxy().get();
  for (auto rename = renames.cbegin(); rename != renames.cend(); ++rename) {
    const auto oldPrefix = "computer_" + rename.key() + "/", newPrefix = "computer_" + rename.value() + "/";
    for (const auto &key : profile.allKeys())
      if (key.startsWith(oldPrefix))
        profile.setValue(newPrefix + key.mid(oldPrefix.size()), profile.value(key));
  }
  // now that the dialog has been accepted, copy the new server config to the
  // original one, which is a reference to the one in MainWindow.
  setOriginalServerConfig(serverConfig());
  if (Settings::value(Settings::Server::EnableClipboard).toBool() != m_enableClipboard) {
    Settings::setValue(Settings::Server::XdpRestoreToken, QString());
    Settings::setValue(Settings::Server::XdpClipboardRetried, false);
  }
  Settings::setValue(Settings::Server::Protocol, networkProtocolToOption(m_protocol));
  Settings::setValue(Settings::Server::EnableClipboard, m_enableClipboard);
  Settings::setValue(Settings::Server::ClipboardSize, m_clipboardSize);
  Settings::setValue(Settings::Server::EnableHeartbeat, m_enableHeartbeat);
  Settings::setValue(Settings::Server::Heartbeat, m_heartbeatRate);
  Settings::setValue(Settings::Server::EnableSwitchDelay, m_enableSwitchDelay);
  Settings::setValue(Settings::Server::SwitchDelay, m_switchDelay);
  Settings::setValue(Settings::Server::DefaultLockToComputerState, m_defaultLockToComputerState);
  Settings::setValue(Settings::Server::DisableLockToComputer, m_disableLockToComputer);
  Settings::setValue(Settings::Server::EnableSwitchDoubleTap, m_enableSwitchDoubleTap);
  Settings::setValue(Settings::Server::SwitchDoubleTap, m_switchDoubleTap);
  Settings::setValue(Settings::Server::RelativeMouseMoves, m_relativeMouseMoves);
  Settings::setValue(Settings::Server::Win32KeepForeground, m_win32keepForeground);
  Settings::setValue(Settings::Server::ExternalConfig, ui->groupExternalConfig->isChecked());
  Settings::setValue(Settings::Server::ExternalConfigFile, ui->lineConfigFile->text());

  QStringList computerNames;
  const auto computerList = m_computerSetupModel.m_computers;
  for (const auto &computer : computerList) {
    const auto &computerName = computer.name();
    if (computerName.isEmpty())
      continue;
    computerNames.append(QStringLiteral("computer_%1").arg(computerName));
    Settings::setValue(Settings::Computer::Aliases.arg(computerName), computer.aliases());
    Settings::setValue(Settings::Computer::Name.arg(computerName), computerName);
  }
  // Remove only explicitly deleted/renamed objects from this edit session.
  // Unknown and non-rendered legacy device groups must survive an unrelated save.
  for (const auto &oldComputer : originals) {
    if (!oldComputer.isNull() && !computerNames.contains("computer_" + oldComputer.name()))
      profile.remove("computer_" + oldComputer.name());
  }
  if (m_embedded) {
    Q_EMIT accepted();
  } else
    QDialog::accept();
}

void ServerConfigDialog::cancel()
{
  // Cancel must not write Settings.
  if (m_embedded) {
    m_serverConfig = m_originalServerConfig;
    resetFromSettings();
    loadFromConfig();
  } else
    QDialog::reject();
}

void ServerConfigDialog::addHotkey()
{
  Hotkey hotkey;
  HotkeyDialog dlg(this, hotkey);
  if (dlg.exec() == QDialog::Accepted) {
    serverConfig().hotkeys().append(hotkey);
    ui->listHotkeys->addItem(hotkey.text());
    setButtonBoxEnabledButtons();
  }
}

void ServerConfigDialog::editHotkey()
{
  int row = ui->listHotkeys->currentRow();
  if (row < 0 || row >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to editing out of bounds hotkey row: " << row;
    return;
  }

  Hotkey &hotkey = serverConfig().hotkeys()[row];
  HotkeyDialog dlg(this, hotkey);
  if (dlg.exec() == QDialog::Accepted) {
    ui->listHotkeys->currentItem()->setText(hotkey.text());
    setButtonBoxEnabledButtons();
  }
}

void ServerConfigDialog::removeHotkey()
{
  int row = ui->listHotkeys->currentRow();
  if (row < 0 || row >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to remove out of bounds hotkey row: " << row;
    return;
  }

  serverConfig().hotkeys().removeAt(row);
  ui->listActions->clear();
  delete ui->listHotkeys->item(row);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::listHotkeysSelectionChanged(const QItemSelection &selected, const QItemSelection &)
{
  bool itemsSelected = !selected.isEmpty();
  ui->btnEditHotkey->setEnabled(itemsSelected);
  ui->btnRemoveHotkey->setEnabled(itemsSelected);
  ui->btnNewAction->setEnabled(itemsSelected);

  if (itemsSelected && !serverConfig().hotkeys().isEmpty()) {
    ui->listActions->clear();
    const Hotkey &hotkey = serverConfig().hotkeys().at(selected.indexes().first().row());
    for (const Action &action : hotkey.actions())
      ui->listActions->addItem(action.text());
  }
}

void ServerConfigDialog::addAction()
{
  int row = ui->listHotkeys->currentRow();
  if (row < 0 || row >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to add action to out of bounds hotkey row: " << row;
    return;
  }

  Hotkey &hotkey = serverConfig().hotkeys()[row];
  Action action;
  ActionDialog dlg(this, serverConfig(), hotkey, action);
  if (dlg.exec() == QDialog::Accepted) {
    hotkey.addAction(action);
    ui->listActions->addItem(action.text());
    setButtonBoxEnabledButtons();
  }
}

void ServerConfigDialog::editAction()
{
  int hotkeyRow = ui->listHotkeys->currentRow();
  if (hotkeyRow < 0 || hotkeyRow >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to edit action from out of bounds hotkey row: " << hotkeyRow;
    return;
  }
  Hotkey &hotkey = serverConfig().hotkeys()[hotkeyRow];

  int actionRow = ui->listActions->currentRow();
  if (actionRow < 0 || actionRow >= hotkey.actions().size()) {
    qDebug() << "Attempt to remove out of bounds action row: " << actionRow;
    return;
  }
  Action &action = hotkey.actionAt(actionRow);

  ActionDialog dlg(this, serverConfig(), hotkey, action);
  if (dlg.exec() == QDialog::Accepted) {
    ui->listActions->currentItem()->setText(action.text());
    setButtonBoxEnabledButtons();
  }
}

void ServerConfigDialog::removeAction()
{
  int hotkeyRow = ui->listHotkeys->currentRow();
  if (hotkeyRow < 0 || hotkeyRow >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to remove action from out of bounds hotkey row: " << hotkeyRow;
    return;
  }
  Hotkey &hotkey = serverConfig().hotkeys()[hotkeyRow];

  int actionRow = ui->listActions->currentRow();
  if (actionRow < 0 || actionRow >= hotkey.actions().size()) {
    qDebug() << "Attempt to remove out of bounds action row: " << actionRow;
    return;
  }

  hotkey.removeActionAt(actionRow);
  delete ui->listActions->currentItem();
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleClipboard(bool enabled)
{
  if (m_enableClipboard == enabled)
    return;

  m_enableClipboard = enabled;

  ui->sbClipboardSizeLimit->setEnabled(enabled);
  if (enabled && !ui->sbClipboardSizeLimit->value()) {
    m_clipboardSize = Settings::defaultValue(Settings::Server::ClipboardSize).toUInt();
    ui->sbClipboardSizeLimit->setValue(m_clipboardSize ? m_clipboardSize : 1);
  }
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setClipboardLimit(int limit)
{
  if (m_clipboardSize == limit)
    return;

  m_clipboardSize = limit;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleHeartbeat(bool enabled)
{
  m_enableHeartbeat = enabled;
  ui->sbHeartbeat->setEnabled(enabled);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setHeartbeat(int rate)
{
  if (rate == m_heartbeatRate)
    return;
  m_heartbeatRate = rate;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleRelativeMouseMoves(bool enabled)
{
  if (m_relativeMouseMoves == enabled)
    return;
  m_relativeMouseMoves = enabled;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleProtocol()
{
  m_protocol = ui->rbProtocolBarrier->isChecked() ? NetworkProtocol::Barrier : NetworkProtocol::Synergy;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::listActionsSelectionChanged(const QItemSelection &selected, const QItemSelection &)
{
  bool enabled = !selected.isEmpty();
  ui->btnEditAction->setEnabled(enabled);
  ui->btnRemoveAction->setEnabled(enabled);
}

void ServerConfigDialog::toggleSwitchDoubleTap(bool enable)
{
  m_enableSwitchDoubleTap = enable;
  ui->sbSwitchDoubleTap->setEnabled(enable);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setSwitchDoubleTap(int within)
{
  if (m_switchDoubleTap == within)
    return;
  m_switchDoubleTap = within;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleSwitchDelay(bool enable)
{
  m_enableSwitchDelay = enable;
  ui->sbSwitchDelay->setEnabled(enable);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setSwitchDelay(int delay)
{
  if (m_switchDelay == delay)
    return;
  m_switchDelay = delay;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleDefaultLockToComputerState(bool state)
{
  if (m_defaultLockToComputerState == state)
    return;
  m_defaultLockToComputerState = state;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleLockToComputer(bool disabled)
{
  if (m_disableLockToComputer == disabled)
    return;
  m_disableLockToComputer = disabled;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleWin32Foreground(bool enabled)
{
  if (m_win32keepForeground == enabled)
    return;
  m_win32keepForeground = enabled;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::addClient()
{
  addComputer("", false);
}

void ServerConfigDialog::onComputerRemoved()
{
  ui->lblNewComputer->setEnabled(true);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleExternalConfig(bool checked)
{
  ui->widgetExternalConfigControls->setEnabled(checked);
  // A saved file-mode draft does not take ownership of the active graph.
  // Draft only; external mode takes ownership after explicit application.
  setButtonBoxEnabledButtons();
}

bool ServerConfigDialog::browseConfigFile()
{
  //: %1 is replaced with the application names
  //: (*.conf) and (*.*) should not be translated
  const auto deskflowConfigFilter = tr("%1 Configurations (*.conf);;All files (*.*)");

  QString fileName =
      QFileDialog::getOpenFileName(this, tr("Browse for a config file"), "", deskflowConfigFilter.arg(kAppName));

  if (!fileName.isEmpty()) {
    ui->lineConfigFile->setText(fileName);
    setServerConfig();
    return true;
  }

  return false;
}

void ServerConfigDialog::loadFromConfig()
{
  m_protocol = Settings::networkProtocol();
  m_enableHeartbeat = Settings::value(Settings::Server::EnableHeartbeat).toBool();
  m_heartbeatRate = Settings::value(Settings::Server::Heartbeat).toInt();
  m_relativeMouseMoves = Settings::value(Settings::Server::RelativeMouseMoves).toBool();
  m_win32keepForeground = Settings::value(Settings::Server::Win32KeepForeground).toBool();
  m_enableSwitchDelay = Settings::value(Settings::Server::EnableSwitchDelay).toBool();
  m_switchDelay = Settings::value(Settings::Server::SwitchDelay).toInt();
  m_enableSwitchDoubleTap = Settings::value(Settings::Server::EnableSwitchDoubleTap).toBool();
  m_switchDoubleTap = Settings::value(Settings::Server::SwitchDoubleTap).toInt();
  m_defaultLockToComputerState = Settings::value(Settings::Server::DefaultLockToComputerState).toBool();
  m_disableLockToComputer = Settings::value(Settings::Server::DisableLockToComputer).toBool();
  m_enableClipboard = Settings::value(Settings::Server::EnableClipboard).toBool();
  m_clipboardSize = Settings::value(Settings::Server::ClipboardSize).toUInt();

  ui->lineConfigFile->setText(serverConfig().configFile());
  ui->groupExternalConfig->setChecked(serverConfig().useExternalConfig());

  refreshControls();

  ui->listHotkeys->clear();
  for (const Hotkey &hotkey : std::as_const(serverConfig().hotkeys()))
    ui->listHotkeys->addItem(hotkey.text());

  ui->computerSetupView->setModel(&m_computerSetupModel);

  auto &computers = serverConfig().computers();
  auto server = std::ranges::find_if(computers, [this](const Computer &computer) {
    return (computer.name() == serverConfig().getServerName());
  });

  if (server == computers.end()) {
    Computer serverComputer(serverConfig().getServerName());
    serverComputer.markAsServer();
    model().computer(m_columns / 2, m_rows / 2) = serverComputer;
  } else {
    server->markAsServer();
  }
  updateControls();
}

void ServerConfigDialog::resetFromSettings()
{
  m_serverConfig = m_originalServerConfig;
  m_columns = m_originalServerConfig.gridWidth();
  m_rows = m_originalServerConfig.gridHeight();
  m_computerSetupModel.resetGrid(m_columns, m_rows);
  // File mode and path are drafts in the controls.
  loadFromConfig();
  if (ui->tabWidget->currentWidget() == ui->tabComputers) {
    ui->computerSetupView->reset();
    ui->computerSetupView->update();
  }
}

void ServerConfigDialog::refreshControls()
{
  ui->rbProtocolSynergy->setChecked(m_protocol == NetworkProtocol::Synergy);
  ui->rbProtocolBarrier->setChecked(m_protocol == NetworkProtocol::Barrier);
  ui->cbHeartbeat->setChecked(m_enableHeartbeat);
  ui->sbHeartbeat->setEnabled(ui->cbHeartbeat->isChecked());
  ui->sbHeartbeat->setValue(m_heartbeatRate);
  ui->cbRelativeMouseMoves->setChecked(m_relativeMouseMoves);
  ui->cbWin32KeepForeground->setChecked(m_win32keepForeground);
  ui->cbSwitchDelay->setChecked(m_enableSwitchDelay);
  ui->sbSwitchDelay->setEnabled(ui->cbSwitchDelay->isChecked());
  ui->sbSwitchDelay->setValue(m_switchDelay);
  ui->cbSwitchDoubleTap->setChecked(m_enableSwitchDoubleTap);
  ui->sbSwitchDoubleTap->setEnabled(ui->cbSwitchDoubleTap->isChecked());
  ui->sbSwitchDoubleTap->setValue(m_switchDoubleTap);
  ui->widgetExternalConfigControls->setEnabled(ui->groupExternalConfig->isChecked());
  toggleExternalConfig(ui->groupExternalConfig->isChecked());
  ui->cbDefaultLockToComputerState->setChecked(m_defaultLockToComputerState);
  ui->cbDisableLockToComputer->setChecked(m_disableLockToComputer);
  ui->cbEnableClipboard->setChecked(m_enableClipboard);
  ui->sbClipboardSizeLimit->setEnabled(m_enableClipboard);
  ui->sbClipboardSizeLimit->setValue(m_clipboardSize);
}

void ServerConfigDialog::initConnections() const
{
  connect(m_buttonBox, &SettingsDialogButtonBox::accepted, this, &ServerConfigDialog::save);
  connect(m_buttonBox, &SettingsDialogButtonBox::rejected, this, &ServerConfigDialog::cancel);
  connect(m_buttonBox, &SettingsDialogButtonBox::reset, this, &ServerConfigDialog::resetFromSettings);
  connect(m_buttonBox, &SettingsDialogButtonBox::restoreDefault, this, &ServerConfigDialog::restoreFromDefaults);
  connect(ui->tabWidget, &QTabWidget::currentChanged, this, &ServerConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->lblRemoveComputer, &RemoveComputerWidget::computerRemoved, this, &ServerConfigDialog::onComputerRemoved);
  connect(ui->btnNewHotkey, &QPushButton::clicked, this, &ServerConfigDialog::addHotkey);
  connect(ui->btnEditHotkey, &QPushButton::clicked, this, &ServerConfigDialog::editHotkey);
  connect(ui->btnRemoveHotkey, &QPushButton::clicked, this, &ServerConfigDialog::removeHotkey);
  connect(ui->listHotkeys, &QListView::doubleClicked, this, &ServerConfigDialog::editHotkey);
  connect(
      ui->listHotkeys->selectionModel(), &QItemSelectionModel::selectionChanged, this,
      &ServerConfigDialog::listHotkeysSelectionChanged
  );

  connect(ui->btnNewAction, &QPushButton::clicked, this, &ServerConfigDialog::addAction);
  connect(ui->btnEditAction, &QPushButton::clicked, this, &ServerConfigDialog::editAction);
  connect(ui->btnRemoveAction, &QPushButton::clicked, this, &ServerConfigDialog::removeAction);
  connect(ui->listActions, &QListView::doubleClicked, this, &ServerConfigDialog::editAction);
  connect(
      ui->listActions->selectionModel(), &QItemSelectionModel::selectionChanged, this,
      &ServerConfigDialog::listActionsSelectionChanged
  );

  connect(ui->rbProtocolBarrier, &QRadioButton::toggled, this, &ServerConfigDialog::toggleProtocol);
  connect(ui->cbHeartbeat, &QCheckBox::toggled, this, &ServerConfigDialog::toggleHeartbeat);
  connect(ui->sbHeartbeat, QOverload<int>::of(&QSpinBox::valueChanged), this, &ServerConfigDialog::setHeartbeat);
  connect(ui->cbWin32KeepForeground, &QCheckBox::toggled, this, &ServerConfigDialog::toggleWin32Foreground);
  connect(ui->cbSwitchDelay, &QCheckBox::toggled, this, &ServerConfigDialog::toggleSwitchDelay);
  connect(ui->sbSwitchDelay, QOverload<int>::of(&QSpinBox::valueChanged), this, &ServerConfigDialog::setSwitchDelay);
  connect(ui->cbSwitchDoubleTap, &QCheckBox::toggled, this, &ServerConfigDialog::toggleSwitchDoubleTap);
  connect(
      ui->sbSwitchDoubleTap, QOverload<int>::of(&QSpinBox::valueChanged), this, &ServerConfigDialog::setSwitchDoubleTap
  );

  connect(ui->cbRelativeMouseMoves, &QCheckBox::toggled, this, &ServerConfigDialog::toggleRelativeMouseMoves);
  connect(ui->cbEnableClipboard, &QCheckBox::toggled, this, &ServerConfigDialog::toggleClipboard);
  connect(ui->btnBrowseConfigFile, &QPushButton::clicked, this, &ServerConfigDialog::browseConfigFile);
  connect(ui->groupExternalConfig, &QGroupBox::toggled, this, &ServerConfigDialog::toggleExternalConfig);
  connect(ui->lineConfigFile, &QLineEdit::textChanged, this, &ServerConfigDialog::setServerConfig);

  connect(
      ui->sbClipboardSizeLimit, QOverload<int>::of(&QSpinBox::valueChanged), this,
      &ServerConfigDialog::setClipboardLimit
  );
  connect(
      ui->cbDefaultLockToComputerState, &QCheckBox::toggled, this, &ServerConfigDialog::toggleDefaultLockToComputerState
  );
  connect(ui->cbDisableLockToComputer, &QCheckBox::toggled, this, &ServerConfigDialog::toggleLockToComputer);
  connect(
      &m_computerSetupModel, &ComputerSetupModel::computersChanged, this,
      &ServerConfigDialog::setButtonBoxEnabledButtons
  );
  connect(Settings::instance(), &Settings::settingsWritableChanged, this, &ServerConfigDialog::updateControls);
}

void ServerConfigDialog::updateControls() const
{
  const bool writable = Settings::isWritable();
  ui->cbDefaultLockToComputerState->setEnabled(writable);
  ui->cbDisableLockToComputer->setEnabled(writable);
  ui->cbEnableClipboard->setEnabled(writable);
  ui->sbClipboardSizeLimit->setEnabled(writable);
  ui->rbProtocolBarrier->setEnabled(writable);
  ui->rbProtocolSynergy->setEnabled(writable);
  ui->cbHeartbeat->setEnabled(writable);
  ui->cbRelativeMouseMoves->setEnabled(writable);
  ui->cbSwitchDelay->setEnabled(writable);
  ui->cbWin32KeepForeground->setEnabled(writable);
  ui->cbSwitchDoubleTap->setEnabled(writable);
  ui->sbSwitchDoubleTap->setEnabled(writable && ui->cbSwitchDoubleTap->isChecked());
  ui->sbSwitchDelay->setEnabled(writable && ui->cbSwitchDelay->isChecked());
  ui->groupExternalConfig->setEnabled(writable);
  ui->tabComputers->setEnabled(writable && !m_activeFileMode);
  ui->tabHotkeys->setEnabled(writable && !m_activeFileMode);
  ui->groupMisc->setEnabled(writable && !m_activeFileMode);
  ui->groupSwitch->setEnabled(writable && !m_activeFileMode);
  ui->groupNetwork->setEnabled(writable && !m_activeFileMode);
  for (auto *widget : findChildren<QWidget *>())
    if (widget->property("graphEditor").toBool())
      widget->setEnabled(writable && !m_activeFileMode);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::restoreFromDefaults()
{
  m_protocol = networkProtocolFromString(Settings::defaultValue(Settings::Server::Protocol).toString());
  m_enableHeartbeat = Settings::defaultValue(Settings::Server::EnableHeartbeat).toBool();
  m_heartbeatRate = Settings::defaultValue(Settings::Server::Heartbeat).toInt();
  m_relativeMouseMoves = Settings::defaultValue(Settings::Server::RelativeMouseMoves).toBool();
  m_win32keepForeground = Settings::defaultValue(Settings::Server::Win32KeepForeground).toBool();
  m_enableSwitchDelay = Settings::defaultValue(Settings::Server::EnableSwitchDelay).toBool();
  m_switchDelay = Settings::defaultValue(Settings::Server::SwitchDelay).toInt();
  m_enableSwitchDoubleTap = Settings::defaultValue(Settings::Server::EnableSwitchDoubleTap).toBool();
  m_switchDoubleTap = Settings::defaultValue(Settings::Server::SwitchDoubleTap).toInt();
  m_defaultLockToComputerState = Settings::defaultValue(Settings::Server::DefaultLockToComputerState).toBool();
  m_disableLockToComputer = Settings::defaultValue(Settings::Server::DisableLockToComputer).toBool();
  m_enableClipboard = Settings::defaultValue(Settings::Server::EnableClipboard).toBool();
  m_clipboardSize = Settings::defaultValue(Settings::Server::ClipboardSize).toUInt();

  ui->groupExternalConfig->setChecked(Settings::defaultValue(Settings::Server::ExternalConfig).toBool());
  ui->lineConfigFile->setText(Settings::defaultValue(Settings::Server::ExternalConfigFile).toString());

  refreshControls();
  updateControls();
}

void ServerConfigDialog::setServerConfig()
{
  const auto configFile = ui->lineConfigFile->text();
  if (!QFile::exists(configFile)) {
    m_buttonBox->enableSave(false);
    return;
  }
  // Draft path remains in the line edit until Save.
  setButtonBoxEnabledButtons();
}

bool ServerConfigDialog::addComputer(const QString &clientName, bool doSilent)
{
  bool isAccepted = false;
  Computer newComputer(clientName);

  if (ComputerSettingsDialog dlg(this, &newComputer, &model().m_computers);
      doSilent || dlg.exec() == QDialog::Accepted) {
    model().addComputer(newComputer);
    isAccepted = true;
  }

  ui->lblNewComputer->setEnabled(!model().isFull());
  return isAccepted;
}

bool ServerConfigDialog::isGeneralConfigModified() const
{
  return ui->groupExternalConfig->isChecked() != Settings::value(Settings::Server::ExternalConfig).toBool() ||
         ui->lineConfigFile->text() != Settings::value(Settings::Server::ExternalConfigFile).toString() ||
         m_protocol != Settings::networkProtocol() ||
         m_enableClipboard != Settings::value(Settings::Server::EnableClipboard).toBool() ||
         m_clipboardSize != Settings::value(Settings::Server::ClipboardSize).toUInt() ||
         m_enableHeartbeat != Settings::value(Settings::Server::EnableHeartbeat).toBool() ||
         m_heartbeatRate != Settings::value(Settings::Server::Heartbeat).toInt() ||
         m_enableSwitchDelay != Settings::value(Settings::Server::EnableSwitchDelay).toBool() ||
         m_switchDelay != Settings::value(Settings::Server::SwitchDelay).toInt() ||
         m_enableSwitchDoubleTap != Settings::value(Settings::Server::EnableSwitchDoubleTap).toBool() ||
         m_switchDoubleTap != Settings::value(Settings::Server::SwitchDoubleTap).toInt() ||
         m_relativeMouseMoves != Settings::value(Settings::Server::RelativeMouseMoves).toBool() ||
         m_win32keepForeground != Settings::value(Settings::Server::Win32KeepForeground).toBool() ||
         m_disableLockToComputer != Settings::value(Settings::Server::DisableLockToComputer).toBool() ||
         m_defaultLockToComputerState != Settings::value(Settings::Server::DefaultLockToComputerState).toBool();
}

bool ServerConfigDialog::isGeneralConfigDefault() const
{
  return ui->groupExternalConfig->isChecked() == Settings::defaultValue(Settings::Server::ExternalConfig).toBool() &&
         ui->lineConfigFile->text() == Settings::defaultValue(Settings::Server::ExternalConfigFile).toString() &&
         m_protocol == networkProtocolFromString(Settings::defaultValue(Settings::Server::Protocol).toString()) &&
         m_enableClipboard == Settings::defaultValue(Settings::Server::EnableClipboard).toBool() &&
         m_clipboardSize == Settings::defaultValue(Settings::Server::ClipboardSize).toUInt() &&
         m_enableHeartbeat == Settings::defaultValue(Settings::Server::EnableHeartbeat).toBool() &&
         m_heartbeatRate == Settings::defaultValue(Settings::Server::Heartbeat).toInt() &&
         m_enableSwitchDelay == Settings::defaultValue(Settings::Server::EnableSwitchDelay).toBool() &&
         m_switchDelay == Settings::defaultValue(Settings::Server::SwitchDelay).toInt() &&
         m_enableSwitchDoubleTap == Settings::defaultValue(Settings::Server::EnableSwitchDoubleTap).toBool() &&
         m_switchDoubleTap == Settings::defaultValue(Settings::Server::SwitchDoubleTap).toInt() &&
         m_relativeMouseMoves == Settings::defaultValue(Settings::Server::RelativeMouseMoves).toBool() &&
         m_win32keepForeground == Settings::defaultValue(Settings::Server::Win32KeepForeground).toBool() &&
         m_disableLockToComputer == Settings::defaultValue(Settings::Server::DisableLockToComputer).toBool() &&
         m_defaultLockToComputerState == Settings::defaultValue(Settings::Server::DefaultLockToComputerState).toBool();
}

void ServerConfigDialog::setButtonBoxEnabledButtons() const
{
  const bool writable = Settings::isWritable();
  m_buttonBox->enableSave(writable && (isGeneralConfigModified() || !(m_originalServerConfig == m_serverConfig)));
  m_buttonBox->enableReset(writable && (isGeneralConfigModified() || !(m_originalServerConfig == m_serverConfig)));
  m_buttonBox->enableRestoreDefaults(writable && !isGeneralConfigDefault() && ui->tabWidget->currentIndex() == 2);
}

void ServerConfigDialog::setActiveFileMode(bool active)
{
  if (m_activeFileMode == active)
    return;
  m_activeFileMode = active;
  updateControls();
}
