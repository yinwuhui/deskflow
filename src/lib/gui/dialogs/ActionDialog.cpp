/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ActionDialog.h"
#include "ui_ActionDialog.h"

#include "common/Action.h"
#include "common/Hotkey.h"
#include "common/KeySequence.h"
#include "config/ServerConfig.h"

#include "gui/workspace/WorkspaceUi.h"
#include <QTimer>
#include <QVBoxLayout>

ActionDialog::ActionDialog(QWidget *parent, const ServerConfig &config, Hotkey &hotkey, Action &action)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
      ui{std::make_unique<Ui::ActionDialog>()},
      m_hotkey(hotkey),
      m_action(action)
{
  ui->setupUi(this);
  connect(ui->keySequenceWidget, &KeySequenceWidget::keySequenceChanged, this, &ActionDialog::keySequenceChanged);
  connect(
      ui->comboActionType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ActionDialog::actionTypeChanged
  );
  connect(ui->listComputers, &QListWidget::itemChanged, this, &ActionDialog::itemToggled);
  connect(ui->buttonBox, &QDialogButtonBox::accepted, this, &ActionDialog::accept);
  connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &ActionDialog::reject);

  ui->keySequenceWidget->setText(m_action.keySequence().toString());
  ui->keySequenceWidget->setKeySequence(m_action.keySequence());

  ui->comboSwitchInDirection->setCurrentIndex(m_action.switchDirection());
  ui->comboLockCursorToComputer->setCurrentIndex(m_action.lockCursorMode());

  ui->comboActionType->setCurrentIndex(m_action.type());
  ui->comboTriggerOn->setCurrentIndex(m_action.activeOnRelease());

  for (const Computer &computer : config.computers()) {
    if (computer.isNull())
      continue;
    auto *newListItem = new QListWidgetItem(computer.displayName());
    newListItem->setData(Qt::UserRole, computer.name());
    newListItem->setToolTip(computer.name());
    newListItem->setCheckState(Qt::Checked);
    if ((m_action.typeComputerNames().indexOf(computer.name()) == -1) &&
        (m_action.haveComputers() && !m_action.typeComputerNames().isEmpty()))
      newListItem->setCheckState(Qt::Unchecked);

    ui->listComputers->addItem(newListItem);

    ui->comboSwitchToComputer->addItem(tr("%1（%2）").arg(computer.displayName(), computer.name()), computer.name());
    if (computer.name() == m_action.switchComputerName())
      ui->comboSwitchToComputer->setCurrentIndex(ui->comboSwitchToComputer->count() - 1);
  }

  ui->keySequenceWidget->setVisible(false);
  ui->groupComputers->setVisible(false);
  ui->listComputers->setEnabled(!ui->keySequenceWidget->keySequence().isMouseButton());
  ui->comboSwitchToComputer->setVisible(false);
  ui->comboSwitchInDirection->setVisible(false);
  ui->comboLockCursorToComputer->setVisible(false);

  rebuildLayout();
  actionTypeChanged(ui->comboActionType->currentIndex());
}

void ActionDialog::accept()
{
  if (!canSave())
    return;

  m_action.setKeySequence(ui->keySequenceWidget->keySequence());
  m_action.setType(ui->comboActionType->currentIndex());

  m_action.clearComputers();

  int computerCount = ui->listComputers->count();

  for (int i = 0; i < ui->listComputers->count(); i++) {
    const auto &item = ui->listComputers->item(i);
    const auto identifier = item->data(Qt::UserRole).toString();
    m_action.addComputer(identifier);
    if (item->checkState() == Qt::Unchecked) {
      computerCount--;
      m_action.removeComputer(identifier);
    }
  }

  if (computerCount == ui->listComputers->count())
    m_action.clearComputers();

  m_action.setHaveComputers(computerCount);

  m_action.setSwitchComputerName(ui->comboSwitchToComputer->currentData().toString());
  m_action.setSwitchDirection(ui->comboSwitchInDirection->currentIndex());
  m_action.setLockCursorMode(ui->comboLockCursorToComputer->currentIndex());
  m_action.setActiveOnRelease(ui->comboTriggerOn->currentIndex());
  m_action.setRestartServer(ui->comboActionType->currentIndex() == ActionTypes::RestartServer);

  QDialog::accept();
}

void ActionDialog::rebuildLayout()
{
  using namespace deskflow::gui::workspace;
  delete layout();
  setObjectName("workspaceRoot");
  setWindowTitle(tr("编辑快捷键动作"));
  resize(640, 540);
  setMinimumSize(540, 420);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(24, 24, 24, 20);
  layout->setSpacing(18);
  layout->addWidget(label(tr("编辑动作"), "title"));
  layout->addWidget(label(tr("设置触发时机、动作与目标。保留到规则后，仍需保存快捷键配置；这里不会执行动作。"), "muted")
  );
  ui->comboTriggerOn->setItemText(0, tr("组合按下时"));
  ui->comboTriggerOn->setItemText(1, tr("组合释放时"));
  const QStringList types{tr("按下按键"),   tr("释放按键"),         tr("按下并释放按键"), tr("切换到指定电脑"),
                          tr("按方向切换"), tr("切换到下一台电脑"), tr("更改光标锁定"),   tr("重启来源端")};
  for (int i = 0; i < types.size(); ++i)
    ui->comboActionType->setItemText(i, types[i]);
  const QStringList directions{tr("左侧"), tr("右侧"), tr("上方"), tr("下方")};
  for (int i = 0; i < directions.size(); ++i)
    ui->comboSwitchInDirection->setItemText(i, directions[i]);
  const QStringList locks{tr("切换锁定状态"), tr("锁定在当前电脑"), tr("解除光标锁定")};
  for (int i = 0; i < locks.size(); ++i)
    ui->comboLockCursorToComputer->setItemText(i, locks[i]);
  layout->addWidget(row(tr("触发时机"), tr("按下与释放事件独立保留"), ui->comboTriggerOn));
  layout->addWidget(row(tr("动作类型"), tr("同一规则可按顺序组合多个动作"), ui->comboActionType));
  auto *parameters = new QVBoxLayout;
  parameters->setSpacing(12);
  parameters->addWidget(ui->comboSwitchToComputer);
  parameters->addWidget(ui->comboSwitchInDirection);
  parameters->addWidget(ui->comboLockCursorToComputer);
  auto *record = new QHBoxLayout;
  record->addWidget(ui->keySequenceWidget, 1);
  auto *cancel = m_cancelRecording = button(tr("取消录制"));
  record->addWidget(cancel);
  connect(cancel, &QPushButton::clicked, ui->keySequenceWidget, &KeySequenceWidget::cancelRecording);
  parameters->addLayout(record);
  layout->addLayout(parameters);
  ui->groupComputers->setTitle(tr("接收此按键事件的电脑"));
  ui->listComputers->setMinimumHeight(100);
  layout->addWidget(ui->groupComputers, 1);
  auto *notice = label(tr("重启动作会中断共享；保存前会再次说明影响。"), "muted");
  layout->addWidget(notice);
  layout->addWidget(ui->buttonBox);
  ui->buttonBox->button(QDialogButtonBox::Ok)->setText(tr("保留到规则"));
  ui->buttonBox->button(QDialogButtonBox::Ok)->setProperty("primary", true);
  ui->buttonBox->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
  applyTheme(this);
}
void ActionDialog::updateSize()
{
  updateGeometry();
}

void ActionDialog::keySequenceChanged()
{
  ui->listComputers->setEnabled(!ui->keySequenceWidget->keySequence().isMouseButton());
  ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(canSave());
}

void ActionDialog::itemToggled() const
{
  ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(canSave());
}

void ActionDialog::actionTypeChanged(int index)
{
  ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(canSave());
  ui->keySequenceWidget->setVisible(isKeyAction(index));
  if (m_cancelRecording)
    m_cancelRecording->setVisible(isKeyAction(index));
  ui->groupComputers->setVisible(isKeyAction(index));
  ui->listComputers->setEnabled(!ui->keySequenceWidget->keySequence().isMouseButton());
  ui->comboSwitchToComputer->setVisible(index == ActionTypes::SwitchTo);
  ui->comboSwitchInDirection->setVisible(index == ActionTypes::SwitchInDirection);
  ui->comboLockCursorToComputer->setVisible(index == ActionTypes::ModifyCursorLock);
  QTimer::singleShot(1, this, &ActionDialog::updateSize);
}

bool ActionDialog::isKeyAction(int index) const
{
  return ((index == ActionTypes::PressKey) || (index == ActionTypes::ReleaseKey) || (index == ActionTypes::ToggleKey));
}

bool ActionDialog::canSave() const
{
  if (isKeyAction(ui->comboActionType->currentIndex())) {
    const QList<QListWidgetItem *> items = ui->listComputers->findItems("*", Qt::MatchWildcard);
    int totalChecked = 0;
    for (const auto &item : items) {
      if (item->checkState() == Qt::Checked)
        totalChecked++;
    }
    return (!ui->keySequenceWidget->keySequence().toString().isEmpty() && (totalChecked > 0));
  }
  return true;
}

ActionDialog::~ActionDialog() = default;
