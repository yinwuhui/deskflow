/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ComputerSettingsDialog.h"
#include "gui/workspace/WorkspaceUi.h"
#include "ui_ComputerSettingsDialog.h"
#include <QButtonGroup>
#include <QStackedWidget>

#include "gui/config/Computer.h"
#include "validators/AliasValidator.h"
#include "validators/ComputerLineNameValidator.h"
#include "validators/ValidationError.h"

#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QVBoxLayout>

#include <common/Settings.h>

using enum ComputerConfig::SwitchCorner;
using enum ComputerConfig::Fix;

ComputerSettingsDialog::~ComputerSettingsDialog() = default;

ComputerSettingsDialog::ComputerSettingsDialog(QWidget *parent, Computer *computer, const ComputerList *computers)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
      ui{std::make_unique<Ui::ComputerSettingsDialog>()},
      m_computer(computer)
{

  ui->setupUi(this);
  m_displayName = new QLineEdit(m_computer->displayName(), this);
  m_displayName->setAccessibleName(tr("显示名称"));
  m_displayName->setMaxLength(255);
  ui->buttonBox->button(QDialogButtonBox::Cancel)->setFocus();

  ui->lineNameEdit->setText(m_computer->name());

  const auto valNameError = new validators::ValidationError(this, ui->lblNameError);
  const auto valName = new validators::ComputerLineNameValidator(ui->lineNameEdit, valNameError, computers);
  ui->lineNameEdit->setValidator(valName);

  const auto valAliasError = new validators::ValidationError(this, ui->lblAliasError);
  const auto valAlias = new validators::AliasValidator(ui->lineAddAlias, valAliasError);
  ui->lineAddAlias->setValidator(valAlias);

  for (int i = 0; i < m_computer->aliases().count(); i++)
    new QListWidgetItem(m_computer->aliases()[i], ui->listAliases);

  ui->comboShift->setCurrentIndex(m_computer->modifier(static_cast<int>(KeyboardModifier::Shift)));
  ui->comboCtrl->setCurrentIndex(m_computer->modifier(static_cast<int>(KeyboardModifier::Ctrl)));
  ui->comboAlt->setCurrentIndex(m_computer->modifier(static_cast<int>(KeyboardModifier::Alt)));
  ui->comboMeta->setCurrentIndex(m_computer->modifier(static_cast<int>(KeyboardModifier::Meta)));
  ui->comboSuper->setCurrentIndex(m_computer->modifier(static_cast<int>(KeyboardModifier::Super)));
  ui->comboAltGr->setCurrentIndex(m_computer->modifier(static_cast<int>(KeyboardModifier::AltGr)));

  ui->chkDeadTopLeft->setChecked(m_computer->switchCorner(static_cast<int>(TopLeft)));
  ui->chkDeadTopRight->setChecked(m_computer->switchCorner(static_cast<int>(TopRight)));
  ui->chkDeadBottomLeft->setChecked(m_computer->switchCorner(static_cast<int>(BottomLeft)));
  ui->chkDeadBottomRight->setChecked(m_computer->switchCorner(static_cast<int>(BottomRight)));
  ui->sbSwitchCornerSize->setValue(m_computer->switchCornerSize());

  ui->chkWeakX11Focus->setChecked(m_computer->weakX11Focus());

  ui->chkFixCapsLock->setChecked(m_computer->fix(CapsLock));
  ui->chkFixNumLock->setChecked(m_computer->fix(NumLock));
  ui->chkFixScrollLock->setChecked(m_computer->fix(ScrollLock));
  ui->chkFixXTest->setChecked(m_computer->fix(XTest));

  connect(ui->buttonBox, &QDialogButtonBox::accepted, this, &ComputerSettingsDialog::accept);
  connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &ComputerSettingsDialog::reject);
  connect(ui->btnAddAlias, &QPushButton::clicked, this, &ComputerSettingsDialog::addAlias);
  connect(ui->btnRemoveAlias, &QPushButton::clicked, this, &ComputerSettingsDialog::removeAlias);
  connect(ui->lineAddAlias, &QLineEdit::textChanged, this, &ComputerSettingsDialog::checkNewAliasName);
  connect(ui->listAliases, &QListWidget::itemSelectionChanged, this, &ComputerSettingsDialog::aliasSelected);
  rebuildLayout();
}

void ComputerSettingsDialog::rebuildLayout()
{
  using namespace deskflow::gui::workspace;
  const auto oldChildren = findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
  for (auto *child : oldChildren)
    child->hide();
  delete layout();
  setObjectName("workspaceRoot");
  setWindowTitle(tr("设备高级设置"));
  resize(740, 620);
  setMinimumSize(600, 480);
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(24, 24, 24, 20);
  outer->setSpacing(18);
  outer->addWidget(label(tr("设备高级设置"), "title"));
  outer->addWidget(label(tr("更改保留在排列草稿中；保存完整排列后，再明确应用到共享。"), "muted"));
  auto *tabs = new QHBoxLayout;
  auto *group = new QButtonGroup(this);
  group->setExclusive(true);
  auto *pages = new QStackedWidget;
  pages->setObjectName("deviceDetailPages");
  const QStringList names{tr("名称与标识"), tr("修饰键"), tr("屏幕角落"), tr("兼容选项")};
  for (int i = 0; i < names.size(); ++i) {
    auto *tab = button(names[i]);
    tab->setCheckable(true);
    tab->setProperty("tab", true);
    tab->setChecked(i == 0);
    group->addButton(tab, i);
    tabs->addWidget(tab);
  }
  tabs->addStretch();
  outer->addLayout(tabs);
  outer->addWidget(pages, 1);
  connect(group, &QButtonGroup::idClicked, pages, &QStackedWidget::setCurrentIndex);
  auto makePage = [pages] {
    auto *body = new QWidget;
    auto *v = new QVBoxLayout(body);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(14);
    pages->addWidget(deskflow::gui::workspace::scroll(body));
    return v;
  };
  auto addRow = [](QVBoxLayout *v, const QString &title, const QString &description, QWidget *control) {
    v->addWidget(row(title, description, control));
    control->show();
  };
  auto *identity = makePage();
  addRow(identity, tr("显示名称"), tr("用于界面展示，不改变连接标识"), m_displayName);
  addRow(identity, tr("连接标识"), tr("必须与对端设置一致；重命名会更新图形配置中的引用"), ui->lineNameEdit);
  identity->addWidget(ui->lblNameError);
  ui->lblNameError->show();
  auto *stable = label(tr("本地设备 ID：%1").arg(m_computer->stableId()), "muted");
  stable->setTextInteractionFlags(Qt::TextSelectableByMouse);
  identity->addWidget(stable);
  identity->addWidget(label(tr("连接别名"), "subtitle"));
  identity->addWidget(label(tr("保留历史标识，避免旧连接和规则引用失效。别名不能与其他电脑标识冲突。"), "muted"));
  auto *aliases = new QHBoxLayout;
  aliases->addWidget(ui->lineAddAlias, 1);
  aliases->addWidget(ui->btnAddAlias);
  identity->addLayout(aliases);
  ui->lineAddAlias->show();
  ui->btnAddAlias->setText(tr("添加别名"));
  ui->btnAddAlias->show();
  identity->addWidget(ui->lblAliasError);
  ui->lblAliasError->show();
  ui->listAliases->setMinimumHeight(90);
  identity->addWidget(ui->listAliases);
  ui->listAliases->show();
  identity->addWidget(ui->btnRemoveAlias, 0, Qt::AlignRight);
  ui->btnRemoveAlias->setText(tr("移除所选别名"));
  ui->btnRemoveAlias->show();
  identity->addStretch();
  auto *modifiers = makePage();
  modifiers->addWidget(label(tr("按键映射仅作用于此设备。各项独立保存；不按本机平台猜测目标键盘布局。"), "muted"));
  const QList<QPair<QString, QComboBox *>> mappings{{"Shift", ui->comboShift}, {"Ctrl", ui->comboCtrl},
                                                    {"Alt", ui->comboAlt},     {"Meta", ui->comboMeta},
                                                    {"Super", ui->comboSuper}, {"Alt Gr", ui->comboAltGr}};
  for (const auto &mapping : mappings) {
    mapping.second->setItemText(6, tr("无映射"));
    addRow(modifiers, mapping.first, tr("映射到目标修饰键"), mapping.second);
  }
  modifiers->addStretch();
  auto *corners = makePage();
  corners->addWidget(label(tr("在指定角落停留时阻止跨屏切换。角落大小使用既有配置的数值范围。"), "muted"));
  const QList<QPair<QString, QCheckBox *>> cornerControls{
      {tr("左上角"), ui->chkDeadTopLeft},
      {tr("右上角"), ui->chkDeadTopRight},
      {tr("左下角"), ui->chkDeadBottomLeft},
      {tr("右下角"), ui->chkDeadBottomRight}
  };
  for (const auto &corner : cornerControls) {
    corner.second->setText(corner.first);
    corner.second->setLayoutDirection(Qt::LeftToRight);
    corners->addWidget(corner.second);
    corner.second->show();
  }
  addRow(corners, tr("角落大小"), tr("像素"), ui->sbSwitchCornerSize);
  corners->addStretch();
  auto *compatibility = makePage();
  compatibility->addWidget(label(tr("目标平台尚未核验。XTest 与弱焦点选项只对 X11 生效；既有值会完整保留。"), "muted"));
  const QList<QPair<QString, QCheckBox *>> fixes{
      {tr("修正 Caps Lock 状态"), ui->chkFixCapsLock},
      {tr("修正 Num Lock 状态"), ui->chkFixNumLock},
      {tr("修正 Scroll Lock 状态"), ui->chkFixScrollLock},
      {tr("XTest / Xinerama 兼容"), ui->chkFixXTest},
      {tr("减弱 X11 焦点接管"), ui->chkWeakX11Focus}
  };
  for (const auto &fix : fixes) {
    fix.second->setText(fix.first);
    compatibility->addWidget(fix.second);
    fix.second->show();
  }
  compatibility->addStretch();
  outer->addWidget(ui->buttonBox);
  ui->buttonBox->show();
  ui->buttonBox->button(QDialogButtonBox::Save)->setText(tr("保留到排列草稿"));
  ui->buttonBox->button(QDialogButtonBox::Save)->setProperty("primary", true);
  ui->buttonBox->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
  applyTheme(this);
}

void ComputerSettingsDialog::accept()
{
  if (ui->lineNameEdit->text().isEmpty()) {
    QMessageBox::warning(this, tr("连接标识为空"), tr("请填写连接标识，或取消本次编辑。"));
    return;
  }
  if (!ui->lblNameError->text().isEmpty()) {
    return;
  }

  QStringList aliases;
  for (int i = 0; i < ui->listAliases->count(); ++i) {
    const auto alias = ui->listAliases->item(i)->text();
    if (alias.compare(ui->lineNameEdit->text(), Qt::CaseInsensitive) == 0) {
      QMessageBox::warning(this, tr("连接标识与别名重复"), tr("请移除重复别名或更改连接标识。"));
      return;
    }
    if (!aliases.contains(alias))
      aliases.append(alias);
  }
  m_computer->m_displayName = m_displayName->text().trimmed();
  m_computer->setName(ui->lineNameEdit->text());
  m_computer->aliases() = aliases;

  m_computer->setModifier(KeyboardModifier::Shift, ui->comboShift->currentIndex());
  m_computer->setModifier(KeyboardModifier::Ctrl, ui->comboCtrl->currentIndex());
  m_computer->setModifier(KeyboardModifier::Alt, ui->comboAlt->currentIndex());
  m_computer->setModifier(KeyboardModifier::Meta, ui->comboMeta->currentIndex());
  m_computer->setModifier(KeyboardModifier::Super, ui->comboSuper->currentIndex());
  m_computer->setModifier(KeyboardModifier::AltGr, ui->comboAltGr->currentIndex());

  m_computer->setSwitchCorner(TopLeft, ui->chkDeadTopLeft->isChecked());
  m_computer->setSwitchCorner(TopRight, ui->chkDeadTopRight->isChecked());
  m_computer->setSwitchCorner(BottomLeft, ui->chkDeadBottomLeft->isChecked());
  m_computer->setSwitchCorner(BottomRight, ui->chkDeadBottomRight->isChecked());
  m_computer->setSwitchCornerSize(ui->sbSwitchCornerSize->value());

  m_computer->setFix(CapsLock, ui->chkFixCapsLock->isChecked());
  m_computer->setFix(NumLock, ui->chkFixNumLock->isChecked());
  m_computer->setFix(ScrollLock, ui->chkFixScrollLock->isChecked());
  m_computer->setFix(XTest, ui->chkFixXTest->isChecked());

  m_computer->m_weakX11Focus = ui->chkWeakX11Focus->isChecked();

  QDialog::accept();
}

void ComputerSettingsDialog::addAlias()
{
  if (!ui->lineAddAlias->text().isEmpty() &&
      ui->listAliases->findItems(ui->lineAddAlias->text(), Qt::MatchFixedString).isEmpty()) {
    new QListWidgetItem(ui->lineAddAlias->text(), ui->listAliases);
    ui->lineAddAlias->clear();
  }
}

void ComputerSettingsDialog::removeAlias() const
{
  QList<QListWidgetItem *> items = ui->listAliases->selectedItems();
  qDeleteAll(items);
}

void ComputerSettingsDialog::checkNewAliasName(const QString &text)
{
  ui->btnAddAlias->setEnabled(!text.isEmpty() && ui->lblAliasError->text().isEmpty());
}

void ComputerSettingsDialog::aliasSelected()
{
  ui->btnRemoveAlias->setEnabled(!ui->listAliases->selectedItems().isEmpty());
}
