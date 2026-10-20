// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "SettingsWorkspace.h"
#include "WorkspaceUi.h"
#include "common/I18N.h"
#include "common/LogLevel.h"
#include "common/NetworkProtocol.h"
#include "common/Settings.h"
#include "gui/TlsUtility.h"
#include "gui/core/NetworkMonitor.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSystemTrayIcon>
#include <QTextStream>
namespace deskflow::gui::workspace {
namespace {
void group(QWidget *page, const QList<QWidget *> &rows)
{
  auto *box = panel(page);
  auto *layout = new QVBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  for (int i = 0; i < rows.size(); ++i) {
    if (i) {
      auto *line = new QFrame;
      line->setProperty("separator", true);
      layout->addWidget(line);
    }
    layout->addWidget(rows[i]);
  }
  contents(page)->addWidget(box);
}
} // namespace
SettingsWorkspace::SettingsWorkspace(QWidget *parent) : QWidget(parent)
{
  setObjectName("settingsWorkspace");
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(18);
  m_headers = new QStackedWidget;
  layout->addWidget(m_headers);
  auto *tabs = new QHBoxLayout;
  tabs->setSpacing(5);
  for (const auto &item : QList<QPair<QString, QString>>{
           {"general", tr("通用")},
           {"sharing", tr("共享与切换")},
           {"rules", tr("快捷键")},
           {"receiver", tr("接收端输入")},
           {"network", tr("网络")},
           {"trust", tr("设备信任")},
           {"advanced", tr("高级")}
       }) {
    auto *tab = button(item.second);
    tab->setProperty("tab", true);
    tab->setCheckable(true);
    tab->setObjectName("settingsTab_" + item.first);
    tabs->addWidget(tab);
    m_tabs.insert(item.first, tab);
    connect(tab, &QPushButton::clicked, this, [this, key = item.first] { navigate(key); });
  }
  tabs->addStretch();
  layout->addLayout(tabs);
  auto *separator = new QFrame;
  separator->setProperty("separator", true);
  layout->addWidget(separator);
  m_stack = new QStackedWidget;
  layout->addWidget(m_stack, 1);
  buildPages();
  navigate("general");
  connect(Settings::instance(), &Settings::settingsWritableChanged, this, [this] {
    setManagedExternally(m_external, m_receiver);
  });
}
void SettingsWorkspace::bind(
    const QString &section, const QString &key, QWidget *widget, std::function<QVariant()> read,
    std::function<void(const QVariant &)> write
)
{
  Field field{section, Settings::value(key), Settings::value(key), widget, std::move(read), std::move(write)};
  field.write(field.baseline);
  field.baseline = field.read();
  widget->setObjectName("preference_" + key);
  m_fields.insert(key, std::move(field));
}
QCheckBox *SettingsWorkspace::check(const QString &section, const QString &key)
{
  auto *field = new Switch;
  bind(
      section, key, field, [field] { return QVariant(field->isChecked()); },
      [field](const QVariant &v) {
        const QSignalBlocker block(field);
        field->setChecked(v.toBool());
      }
  );
  connect(field, &QCheckBox::toggled, this, [this, key] { changed(key); });
  return field;
}
QComboBox *
SettingsWorkspace::choice(const QString &section, const QString &key, const QList<QPair<QString, QVariant>> &items)
{
  auto *field = new QComboBox;
  field->setMinimumWidth(160);
  for (const auto &item : items)
    field->addItem(item.first, item.second);
  bind(
      section, key, field, [field] { return field->currentData(); },
      [field](const QVariant &v) {
        const QSignalBlocker block(field);
        auto index = field->findData(v);
        if (index < 0) {
          field->addItem(v.toString().isEmpty() ? tr("跟随系统") : v.toString(), v);
          index = field->count() - 1;
        }
        field->setCurrentIndex(index);
      }
  );
  connect(field, &QComboBox::currentIndexChanged, this, [this, key] { changed(key); });
  return field;
}
QWidget *SettingsWorkspace::number(
    const QString &section, const QString &key, int minimum, int maximum, int step, const QString &suffix
)
{
  auto *field = new QSpinBox;
  field->setRange(minimum, maximum);
  field->setSingleStep(step);
  field->setSuffix(suffix);
  field->setMinimumWidth(112);
  bind(
      section, key, field, [field] { return QVariant(field->value()); },
      [field, minimum, maximum](const QVariant &v) {
        const QSignalBlocker block(field);
        const auto value = v.toInt();
        field->setRange(std::min(minimum, value), std::max(maximum, value));
        field->setValue(value);
      }
  );
  connect(field, &QSpinBox::valueChanged, this, [this, key] { changed(key); });
  return field;
}
QLineEdit *SettingsWorkspace::text(const QString &section, const QString &key)
{
  auto *field = new QLineEdit;
  bind(
      section, key, field, [field] { return QVariant(field->text()); },
      [field](const QVariant &v) {
        const QSignalBlocker block(field);
        field->setText(v.toString());
      }
  );
  connect(field, &QLineEdit::textEdited, this, [this, key] { changed(key); });
  return field;
}
void SettingsWorkspace::changed(const QString &key)
{
  if (m_loading)
    return;
  const auto section = m_fields.value(key).section;
  if (section == "general")
    saveSection(section);
  updateFieldAvailability();
}
void SettingsWorkspace::updateFieldAvailability()
{
  const bool writable = Settings::isWritable();
  for (auto &field : m_fields)
    field.widget->setEnabled(writable && !(field.section == "sharing" && (m_external || m_receiver)));
  for (const auto &pair : QList<QPair<QString, QString>>{
           {Settings::Server::EnableClipboard, Settings::Server::ClipboardSize},
           {Settings::Server::EnableSwitchDelay, Settings::Server::SwitchDelay},
           {Settings::Server::EnableSwitchDoubleTap, Settings::Server::SwitchDoubleTap},
           {Settings::Server::EnableHeartbeat, Settings::Server::Heartbeat},
           {Settings::Log::ToFile, Settings::Log::File},
           {Settings::Core::EnableEnterCommand, Settings::Core::ComputerEnterCommand},
           {Settings::Core::EnableExitCommand, Settings::Core::ComputerExitCommand}
       })
    if (auto dependent = m_fields.find(pair.second); dependent != m_fields.end())
      dependent->widget->setEnabled(
          writable && draftValue(pair.first).toBool() &&
          !(dependent->section == "sharing" && (m_external || m_receiver))
      );
}

QVariant SettingsWorkspace::draftValue(const QString &key) const
{
  auto it = m_fields.constFind(key);
  return it == m_fields.cend() ? Settings::value(key) : it->read();
}
void SettingsWorkspace::setDraftValue(const QString &key, const QVariant &value)
{
  auto it = m_fields.find(key);
  if (it != m_fields.end()) {
    it->write(value);
    changed(key);
  }
}
bool SettingsWorkspace::hasUnsavedChanges() const
{
  for (const auto &field : m_fields)
    if (field.read() != field.baseline)
      return true;
  return false;
}
void SettingsWorkspace::reloadSaved()
{
  m_loading = true;
  for (auto it = m_fields.begin(); it != m_fields.end(); ++it) {
    it->baseline = Settings::value(it.key());
    it->persisted = it->baseline;
    it->write(it->baseline);
    it->baseline = it->read();
  }
  m_loading = false;
  setManagedExternally(m_external, m_receiver);
}
void SettingsWorkspace::addPage(const QString &section, QWidget *content)
{
  auto *header = new QWidget;
  if (auto *item = contents(content)->takeAt(0)) {
    if (auto *headerLayout = item->layout()) {
      headerLayout->setParent(nullptr);
      header->setLayout(headerLayout);
    } else
      delete item;
  }
  if (header->layout())
    header->layout()->setContentsMargins(0, 0, 0, 0);
  m_headers->addWidget(header);
  contents(content)->addStretch(1);
  auto *scrolled = workspace::scroll(content);
  scrolled->setObjectName("settingsPage_" + section);
  m_sections.insert(section, scrolled);
  m_stack->addWidget(scrolled);
}
void SettingsWorkspace::installPage(const QString &section, QWidget *content)
{
  if (auto *previous = m_sections.take(section)) {
    const auto index = m_stack->indexOf(previous);
    auto *header = m_headers->widget(index);
    m_headers->removeWidget(header);
    header->deleteLater();
    m_stack->removeWidget(previous);
    previous->deleteLater();
  }
  addPage(section, content);
}
void SettingsWorkspace::navigate(const QString &requested)
{
  const auto section = requested == "advanced" ? QString("maintenance") : requested;
  if (!m_sections.contains(section))
    return;
  auto *target = m_sections.value(section);
  m_stack->setCurrentWidget(target);
  m_headers->setCurrentIndex(m_stack->currentIndex());
  const bool advanced = section == "certificates" || section == "config" || section == "maintenance";
  for (auto it = m_tabs.begin(); it != m_tabs.end(); ++it)
    it.value()->setChecked(it.key() == (advanced ? "advanced" : section));
  if (m_advancedLinks)
    m_advancedLinks->setVisible(advanced);
  updateHeaderHeight();
  if (section == "trust")
    Q_EMIT requestTrust();
}
void SettingsWorkspace::updateHeaderHeight()
{
  if (auto *header = m_headers->currentWidget()) {
    const auto h = header->heightForWidth(std::max(1, width()));
    m_headers->setFixedHeight(std::max(header->minimumSizeHint().height(), h > 0 ? h : header->sizeHint().height()));
  }
}
void SettingsWorkspace::resizeEvent(QResizeEvent *event)
{
  QWidget::resizeEvent(event);
  updateHeaderHeight();
}

void SettingsWorkspace::addSave(QWidget *content, const QString &section, const QString &caption)
{
  auto *error = label({}, "error");
  error->setObjectName("settingsError_" + section);
  error->hide();
  contents(content)->addWidget(error);
  m_errors.insert(section, error);
  auto *actions = new QHBoxLayout;
  actions->addStretch();
  auto *discard = button(tr("放弃本页更改"));
  auto *save = button(caption, true);
  save->setObjectName("saveSection_" + section);
  actions->addWidget(discard);
  actions->addWidget(save);
  contents(content)->addLayout(actions);
  connect(save, &QPushButton::clicked, this, [this, section] { saveSection(section); });
  connect(discard, &QPushButton::clicked, this, [this, section] {
    for (auto it = m_fields.begin(); it != m_fields.end(); ++it)
      if (it->section == section) {
        it->persisted = Settings::value(it.key());
        it->write(it->persisted);
        it->baseline = it->read();
      }
    showError(section, {});
    setManagedExternally(m_external, m_receiver);
  });
}
void SettingsWorkspace::showError(const QString &section, const QString &message)
{
  if (auto *label = m_errors.value(section)) {
    label->setVisible(!message.isEmpty());
    setStatusText(label, message);
    label->setAccessibleName(message);
  }
}
void SettingsWorkspace::setManagedExternally(bool external, bool receiver)
{
  m_external = external;
  m_receiver = receiver;
  if (m_receiverNotice)
    m_receiverNotice->setVisible(!receiver);
  updateFieldAvailability();
  if (m_managementNotice) {
    m_managementNotice->setText(
        receiver   ? tr("此项由键鼠来源管理。这台电脑的接收端输入选项仍可编辑。")
        : external ? tr("当前生效配置由外部文件管理；退出文件模式并应用后可编辑。")
                   : QString()
    );
    m_managementNotice->setVisible(external || receiver);
  }
  for (const auto &name : {"sharing", "receiver", "network", "certificates", "config", "maintenance", "logs"})
    if (auto *save = findChild<QPushButton *>("saveSection_" + QString(name)))
      save->setEnabled(Settings::isWritable() && !(QString(name) == "sharing" && (external || receiver)));
}
bool SettingsWorkspace::resetOrdinaryPreferences()
{
  for (const auto &key :
       {Settings::Core::Language, Settings::Gui::AutoUpdateCheck, Settings::Core::PreventSleep,
        Settings::Gui::SymbolicTrayIcon, Settings::Gui::Autohide, Settings::Gui::CloseToTray,
        Settings::Gui::CloseReminder, Settings::Gui::ShowVersionInTitle}) {
    auto field = m_fields.find(key);
    if (field != m_fields.end())
      field->write(Settings::defaultValue(key));
  }
  return saveSection("general");
}
bool SettingsWorkspace::saveSection(const QString &section)
{
  if (!Settings::isWritable() || (section == "sharing" && (m_external || m_receiver))) {
    showError(section, tr("当前配置不可写，修改保持为草稿。"));
    return false;
  }
  QMap<QString, QVariant> changes;
  for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it)
    if (it->section == section && it->read() != it->baseline)
      changes.insert(it.key(), it->read());
  if (changes.isEmpty())
    return true;
  for (auto it = changes.cbegin(); it != changes.cend(); ++it)
    if (Settings::value(it.key()) != m_fields.value(it.key()).persisted) {
      showError(section, tr("这些设置在编辑期间已被其他操作修改。请保留草稿并重新载入该分类，避免覆盖新值。"));
      return false;
    }
  const QMap<QString, QPair<int, int>> ranges{
      {Settings::Server::ClipboardSize, {0, 10000}},
      {Settings::Server::SwitchDelay, {10, 10000}},
      {Settings::Server::SwitchDoubleTap, {10, 10000}},
      {Settings::Server::Heartbeat, {1000, 30000}}
  };
  for (auto it = ranges.cbegin(); it != ranges.cend(); ++it)
    if (changes.contains(it.key()) &&
        (changes.value(it.key()).toInt() < it->first || changes.value(it.key()).toInt() > it->second)) {
      showError(section, tr("新数值必须在 %1–%2 之间。已有导入值在未修改时保持保留。").arg(it->first).arg(it->second));
      return false;
    }
  if (section == "maintenance") {
    for (const auto &pair : QList<QPair<QString, QString>>{
             {Settings::Core::EnableEnterCommand, Settings::Core::ComputerEnterCommand},
             {Settings::Core::EnableExitCommand, Settings::Core::ComputerExitCommand}
         })
      if (draftValue(pair.first).toBool() && draftValue(pair.second).toString().trimmed().isEmpty()) {
        showError(section, tr("已启用的本机命令不能为空；保存不会执行命令。"));
        return false;
      }
  }
  if (section == "network") {
    if (changes.contains(Settings::Core::Port) && draftValue(Settings::Core::Port).toInt() == 0 &&
        Settings::value(Settings::Core::Port).toInt() != 0) {
      showError(section, tr("新连接端口应在1–65535之间；已有导入值保持保留。"));
      return false;
    }
    for (const auto &address : draftValue(Settings::Client::RemoteHost).toString().split(','))
      if (address.contains("://")) {
        showError(section, tr("来源地址请填写主机名或 IP，不要填写带协议的 URL。"));
        return false;
      }
  }
  if (section == "certificates") {
    const auto path = draftValue(Settings::Security::Certificate).toString();
    if (changes.contains(Settings::Security::Certificate) && !TlsUtility::isCertValid(path)) {
      showError(section, tr("证书不可用或私钥不匹配。当前身份保持不变，请选择有效的 PEM 文件。"));
      return false;
    }
    const bool tlsOff = Settings::value(Settings::Security::TlsEnabled).toBool() &&
                        !draftValue(Settings::Security::TlsEnabled).toBool();
    const bool authOff = Settings::value(Settings::Security::CheckPeers).toBool() &&
                         !draftValue(Settings::Security::CheckPeers).toBool();
    if ((tlsOff || authOff) && QMessageBox::warning(
                                   this, tr("确认兼容安全设置"),
                                   tlsOff ? tr("应用后键鼠和剪贴板将不受 TLS 加密保护。确认保存此兼容配置？")
                                          : tr("应用后不再核对接收端证书。确认保存此兼容配置？"),
                                   QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel
                               ) != QMessageBox::Yes)
      return false;
  }
  if (section == "config" && draftValue(Settings::Server::ExternalConfig).toBool()) {
    QFile file(draftValue(Settings::Server::ExternalConfigFile).toString());
    if (!file.open(QIODevice::ReadOnly)) {
      showError(section, tr("无法读取配置文件。草稿保留，当前生效配置未改变。"));
      return false;
    }
  }
  QMap<QString, QVariant> previous;
  for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
    previous.insert(it.key(), Settings::value(it.key()));
    Settings::setValue(it.key(), it.value());
  }
  Settings::save();
  Settings::proxy().sync();
  if (Settings::proxy().get().status() != QSettings::NoError) {
    for (auto it = previous.cbegin(); it != previous.cend(); ++it)
      Settings::setValue(it.key(), it.value());
    showError(section, tr("写入失败，修改保持为草稿。请检查配置目录权限。"));
    return false;
  }
  for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
    m_fields[it.key()].baseline = it.value();
    m_fields[it.key()].persisted = it.value();
  }
  showError(section, {});
  Q_EMIT saved(section);
  return true;
}
void SettingsWorkspace::setIdentityGenerationState(bool busy, const QString &message)
{
  if (auto *page = m_sections.value("certificates"))
    page->setEnabled(!busy);
  if (!busy)
    setManagedExternally(m_external, m_receiver);
  showError("certificates", message);
}
void SettingsWorkspace::setFileValidationResult(const QString &path, bool success, const QString &reason)
{
  if (path != draftValue(Settings::Server::ExternalConfigFile).toString())
    return;
  if (auto *status = findChild<QLabel *>("fileValidationStatus")) {
    status->setProperty("kind", success ? "muted" : "error");
    status->setText(
        success ? tr("检查通过。保存不会改变当前共享；应用后才接管配置。") : tr("检查失败：%1").arg(reason)
    );
    status->style()->unpolish(status);
    status->style()->polish(status);
  }
}
void SettingsWorkspace::buildPages()
{
  auto *general = page(tr("通用选项"), tr("语言、启动方式、显示与电源偏好。"));
  QList<QPair<QString, QVariant>> languages{{tr("跟随系统"), QString()}};
  for (const auto &native : I18N::detectedLanguages())
    languages.append({native, I18N::nativeTo639Name(native)});
  if (languages.size() == 1)
    languages.append({"English", QString("en")});
  group(
      general,
      {row(tr("界面语言"), tr("语言切换后保留当前页面与草稿"), choice("general", Settings::Core::Language, languages)),
       row(tr("启动时检查更新"), tr("只检查，不自动下载或安装"), check("general", Settings::Gui::AutoUpdateCheck)),
       row(tr("共享时防止本机睡眠"), tr("保存偏好后按运行配置应用"), check("general", Settings::Core::PreventSleep)),
       row(tr("后台图标样式"), tr("菜单栏与托盘图标"),
           choice("general", Settings::Gui::SymbolicTrayIcon, {{tr("单色"), true}, {tr("彩色"), false}})),
       row(tr("启动时显示"), tr("后台入口不可用时保持窗口可返回"),
           choice("general", Settings::Gui::Autohide, {{tr("显示主窗口"), false}, {tr("转入后台"), true}})),
       row(tr("关闭主窗口时"), tr("明确退出始终与关闭窗口分开"),
           choice(
               "general", Settings::Gui::CloseToTray, {{tr("隐藏到菜单栏／托盘"), true}, {tr("停止共享并退出"), false}}
           )),
       row(tr("标题显示版本号"), tr("便于反馈问题"), check("general", Settings::Gui::ShowVersionInTitle))}
  );
  contents(general)->addWidget(label(tr("普通显示偏好更改后自动保存。运行参数仍需明确应用。"), "muted"));
  auto *role = button(tr("更改这台电脑的使用方式…"));
  contents(general)->addWidget(row(tr("使用方式"), tr("更改角色会说明连接影响，并在应用时生效"), role));
  connect(role, &QPushButton::clicked, this, &SettingsWorkspace::requestRoleChange);
  addPage("general", general);

  auto *sharing = page(tr("共享与切换"), tr("独立配置剪贴板和边缘切换；关闭开关时保留已填写的数值。"));
  m_managementNotice = label({}, "muted");
  contents(sharing)->addWidget(m_managementNotice);
  contents(sharing)->addWidget(label(tr("剪贴板"), "subtitle"));
  group(
      sharing, {row(tr("共享剪贴板"), tr("与键鼠共享分开控制"), check("sharing", Settings::Server::EnableClipboard)),
                row(tr("剪贴板容量上限"), tr("保留既有容量语义；导入的0不会被静默更改"),
                    number("sharing", Settings::Server::ClipboardSize, 0, 10000, 1, " MB"))}
  );
  contents(sharing)->addWidget(label(tr("边缘切换"), "subtitle"));
  group(
      sharing,
      {row(tr("停留后切换"), tr("可与连续触碰同时启用"), check("sharing", Settings::Server::EnableSwitchDelay)),
       row(tr("停留时间"), tr("10–10000毫秒"), number("sharing", Settings::Server::SwitchDelay, 10, 10000, 10, " ms")),
       row(tr("连续触碰两次切换"), tr("独立保存，不覆盖停留设置"),
           check("sharing", Settings::Server::EnableSwitchDoubleTap)),
       row(tr("连续触碰间隔"), tr("10–10000毫秒"),
           number("sharing", Settings::Server::SwitchDoubleTap, 10, 10000, 10, " ms"))}
  );
  contents(sharing)->addWidget(label(tr("高级输入与连接检测"), "subtitle"));
  group(
      sharing, {row(tr("禁用 Scroll Lock 锁定"), tr("保留原设置的开关语义"),
                    check("sharing", Settings::Server::DisableLockToComputer)),
                row(tr("启动时锁定在本机"), {}, check("sharing", Settings::Server::DefaultLockToComputerState)),
                row(tr("相对鼠标移动"), tr("按应用兼容性选择"), check("sharing", Settings::Server::RelativeMouseMoves)),
                row(tr("定期检查接收电脑"), tr("连接心跳"), check("sharing", Settings::Server::EnableHeartbeat)),
                row(tr("检查间隔"), {}, number("sharing", Settings::Server::Heartbeat, 1000, 30000, 1000, " ms"))}
  );
#ifdef Q_OS_WIN
  group(
      sharing, {row(tr("不夺取前台窗口"), tr("Windows 专用"), check("sharing", Settings::Server::Win32KeepForeground))}
  );
#endif
  addSave(sharing, "sharing", tr("保存共享配置"));
  addPage("sharing", sharing);
  addPage("rules", page(tr("高级快捷键动作"), tr("完整动作、目标与触发时机。")));
  auto *receiver = page(tr("接收端输入"), tr("这些选项只影响这台接收电脑，不覆盖键鼠来源的全局配置。"));
  m_receiverNotice = label(tr("这里可以预设本机接收输入时的选项；当前提供键鼠时不会使用这些预设。"), "muted");
  contents(receiver)->addWidget(m_receiverNotice);
  group(
      receiver, {row(tr("动态重连间隔"), tr("多次失败后逐步调整等待时间"),
                     check("receiver", Settings::Client::DynamicConnectionRetry)),
                 row(tr("跟随来源电脑的键盘语言"), tr("需要两边安装匹配的键盘布局"),
                     check("receiver", Settings::Client::LanguageSync))}
  );
  QList<QWidget *> wheelRows;
  for (const auto &axis : {QString("X"), QString("Y")}) {
    const bool x = axis == "X";
    const auto scaleKey = x ? Settings::Client::XScrollScale : Settings::Client::YScrollScale;
    const auto invertKey = x ? Settings::Client::InvertXScroll : Settings::Client::InvertYScroll;
    auto *scale = new QDoubleSpinBox;
    scale->setRange(.1, 10);
    scale->setSingleStep(.1);
    scale->setDecimals(1);
    scale->setMinimumWidth(112);
    bind(
        "receiver", scaleKey, scale, [scale] { return QVariant(scale->value()); },
        [scale](const QVariant &v) {
          const QSignalBlocker b(scale);
          scale->setValue(v.toDouble());
        }
    );
    connect(scale, &QDoubleSpinBox::valueChanged, this, [this, scaleKey] { changed(scaleKey); });
    wheelRows.append(row(
        x ? tr("水平滚动反向") : tr("垂直滚动反向"), tr("只改变本机 %1 轴方向").arg(axis), check("receiver", invertKey)
    ));
    wheelRows.append(row(x ? tr("水平滚动倍率") : tr("垂直滚动倍率"), tr("0.1–10.0"), scale));
  }
  contents(receiver)->addWidget(label(tr("滚轮调整"), "subtitle"));
  group(receiver, wheelRows);
  auto *defaults = button(tr("恢复本页默认值"));
  contents(receiver)->addWidget(defaults);
  connect(defaults, &QPushButton::clicked, this, [this] {
    for (auto it = m_fields.begin(); it != m_fields.end(); ++it)
      if (it->section == "receiver")
        it->write(Settings::defaultValue(it.key()));
  });
  addSave(receiver, "receiver", tr("保存接收端设置"));
  addPage("receiver", receiver);
  auto *network = page(tr("网络与候选地址"), tr("监听接口、端口和键鼠来源。保存不会中断当前共享。"));
  QList<QPair<QString, QVariant>> interfaces{{tr("自动／所有适用接口"), QString()}};
  for (const auto &address : NetworkMonitor::validAddresses())
    interfaces.append({address, address});
  group(
      network, {row(tr("监听接口"), tr("接口失效时保留原值，应用后检查实际地址"),
                    choice("network", Settings::Core::Interface, interfaces)),
                row(tr("监听端口"), tr("新连接使用1–65535；保留旧配置的导入值"),
                    number("network", Settings::Core::Port, 0, 65535))}
  );
  auto *addresses = new QPlainTextEdit;
  addresses->setMaximumHeight(120);
  addresses->setAccessibleName(tr("候选来源地址，每行一个"));
  bind(
      "network", Settings::Client::RemoteHost, addresses,
      [addresses] { return QVariant(addresses->toPlainText().replace('\n', ',')); },
      [addresses](const QVariant &v) {
        const QSignalBlocker b(addresses);
        addresses->setPlainText(QString(v.toString()).replace(',', '\n'));
      }
  );
  connect(addresses, &QPlainTextEdit::textChanged, this, [this] { changed(Settings::Client::RemoteHost); });
  auto *addressIntro = new QWidget;
  auto *addressIntroLayout = new QVBoxLayout(addressIntro);
  addressIntroLayout->setContentsMargins(0, 0, 0, 0);
  addressIntroLayout->setSpacing(4);
  addressIntroLayout->addWidget(label(tr("候选键鼠来源地址"), "subtitle"));
  addressIntroLayout->addWidget(label(tr("每行一个主机名或 IP。保留顺序与 IPv6；连接时由核心解析。"), "muted"));
  contents(network)->addWidget(addressIntro);
  contents(network)->addWidget(addresses);
  addSave(network, "network", tr("保存网络配置"));
  addPage("network", network);
  addPage("trust", page(tr("设备信任"), tr("身份、连接和输入验证分别记录。")));

  auto *certificates = page(tr("证书与兼容安全"), tr("本机身份、证书文件与连接验证策略。"));
  group(
      certificates,
      {row(tr("TLS 加密连接"), tr("关闭加密需要单独确认，诊断不会自动降级"),
           check("certificates", Settings::Security::TlsEnabled)),
       row(tr("要求接收端提供证书"), tr("键鼠来源验证对端身份"), check("certificates", Settings::Security::CheckPeers))}
  );
  contents(certificates)->addWidget(label(tr("本机证书"), "subtitle"));
  auto *certPath = text("certificates", Settings::Security::Certificate);
  contents(certificates)->addWidget(certPath);
  auto *chooseCertificate = button(tr("选择证书文件…"));
  contents(certificates)->addWidget(chooseCertificate, 0, Qt::AlignLeft);
  connect(chooseCertificate, &QPushButton::clicked, this, [this, certPath] {
    const auto path =
        QFileDialog::getOpenFileName(this, tr("选择证书"), certPath->text(), tr("PEM证书 (*.pem);;所有文件 (*)"));
    if (!path.isEmpty()) {
      certPath->setText(path);
      changed(Settings::Security::Certificate);
    }
  });
  auto *fingerprint = button(tr("查看完整指纹…"));
  connect(fingerprint, &QPushButton::clicked, this, &SettingsWorkspace::requestFingerprint);
  group(
      certificates, {row(tr("新证书密钥长度"), tr("用于明确重新生成本机身份"),
                         choice("certificates", Settings::Security::KeySize, {{"2048", 2048}, {"4096", 4096}})),
                     row(tr("当前本机身份指纹"), tr("完整 SHA-256 与同一摘要生成的图案"), fingerprint)}
  );
  auto *generate = button(tr("重新生成当前本机身份…"));
  contents(certificates)->addWidget(generate);
  connect(generate, &QPushButton::clicked, this, [this] {
    if (QMessageBox::warning(
            this, tr("更换本机身份"),
            tr("将替换当前保存的本机证书并停止共享。所有对端需要重新核对；本地对远端的信任保留。尚未保存的证书路径不会"
               "被使用。"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel
        ) != QMessageBox::Yes)
      return;
    Q_EMIT requestIdentityGeneration(draftValue(Settings::Security::KeySize).toInt());
  });
  addSave(certificates, "certificates", tr("保存证书配置"));
  addPage("certificates", certificates);
  auto *config = page(tr("配置文件与兼容协议"), tr("图形排列与自定义文件保持单一生效来源。"));
  group(
      config, {row(tr("来源端网络协议"), tr("接收端按来源自动适配"),
                   choice(
                       "config", Settings::Server::Protocol,
                       {{"Synergy", networkProtocolToOption(NetworkProtocol::Synergy)},
                        {"Barrier", networkProtocolToOption(NetworkProtocol::Barrier)}}
                   )),
               row(tr("使用自定义来源配置"), tr("实际应用后，相关图形编辑才变为只读"),
                   check("config", Settings::Server::ExternalConfig))}
  );
  auto *configPath = text("config", Settings::Server::ExternalConfigFile);
  contents(config)->addWidget(configPath);
  auto *fileActions = new QHBoxLayout;
  auto *chooseFile = button(tr("选择配置文件…"));
  auto *validate = button(tr("检查文件"));
  fileActions->addWidget(chooseFile);
  fileActions->addWidget(validate);
  fileActions->addStretch();
  contents(config)->addLayout(fileActions);
  auto *preview = new QPlainTextEdit;
  preview->setReadOnly(true);
  preview->setMinimumHeight(180);
  preview->setAccessibleName(tr("配置文件内容，只读预览"));
  auto loadPreview = [configPath, preview] {
    QFile file(configPath->text());
    if (!file.open(QIODevice::ReadOnly)) {
      preview->setPlainText(tr("文件尚不可读取。"));
      return;
    }
    auto bytes = file.read(1024 * 1024);
    preview->setPlainText(
        QString::fromUtf8(bytes) + (file.atEnd() ? QString() : tr("\n预览仅显示前1MiB；检查将解析完整文件。"))
    );
  };
  connect(configPath, &QLineEdit::textChanged, this, [loadPreview] { loadPreview(); });
  loadPreview();
  contents(config)->addWidget(preview);
  auto *validationStatus = label(tr("检查使用核心真实解析器；不会启动共享。"), "muted");
  validationStatus->setObjectName("fileValidationStatus");
  contents(config)->addWidget(validationStatus);
  connect(validate, &QPushButton::clicked, this, [this, configPath, validationStatus] {
    validationStatus->setText(tr("正在检查文件…"));
    Q_EMIT requestValidateFile(configPath->text());
  });
  connect(chooseFile, &QPushButton::clicked, this, [this, configPath] {
    const auto path = QFileDialog::getOpenFileName(
        this, tr("选择来源配置"), configPath->text(), tr("配置文件 (*.conf);;所有文件 (*)")
    );
    if (!path.isEmpty()) {
      configPath->setText(path);
      changed(Settings::Server::ExternalConfigFile);
    }
  });
  auto *exportButton = button(tr("导出完整配置包…"));
  contents(config)->addWidget(label(
      tr("完整配置包包含设备、别名、全部映射、角落、修复、规则、链接、共享和协议选项，可在维护页恢复；尚未保存的草稿不"
         "包含在内。"),
      "muted"
  ));
  contents(config)->addWidget(exportButton, 0, Qt::AlignLeft);
  auto *routingExport = button(tr("导出核心路由文本…"));
  contents(config)->addWidget(row(
      tr("当前 profile 的路由文件"), tr("仅链接与动作，用于当前设备定义下的外部模式，不包含完整设备设置"), routingExport
  ));
  connect(routingExport, &QPushButton::clicked, this, &SettingsWorkspace::requestRoutingExport);
  connect(exportButton, &QPushButton::clicked, this, &SettingsWorkspace::requestGraphExport);
  addSave(config, "config", tr("保存并等待应用"));
  addPage("config", config);
  auto *maintenance = page(tr("维护与自动化"), tr("服务生命周期、本机命令、配置恢复与危险清除分开处理。"));
#ifdef Q_OS_WIN
  if (!Settings::isPortableMode())
    group(
        maintenance,
        {row(tr("运行方式"), tr("服务是否已启动以实际回执为准"),
             choice(
                 "maintenance", Settings::Core::ProcessMode,
                 {{tr("桌面进程"), QVariant::fromValue(Settings::ProcessMode::Desktop)},
                  {tr("后台服务"), QVariant::fromValue(Settings::ProcessMode::Service)}}
             )),
         row(tr("守护进程提权"), tr("仅在已安装且服务可用时适用"), check("maintenance", Settings::Daemon::Elevate))}
    );
#else
  contents(maintenance)->addWidget(label(tr("当前平台不显示 Windows 服务选项。"), "muted"));
#endif
  contents(maintenance)->addWidget(label(tr("本机进入／离开命令"), "subtitle"));
  contents(maintenance)->addWidget(label(tr("命令在这台电脑执行；编辑、保存和预览均不会试运行。"), "muted"));
  for (const auto &item : QList<QPair<QString, QString>>{
           {Settings::Core::EnableEnterCommand, Settings::Core::ComputerEnterCommand},
           {Settings::Core::EnableExitCommand, Settings::Core::ComputerExitCommand}
       }) {
    const bool enter = item.first == Settings::Core::EnableEnterCommand;
    group(
        maintenance, {row(
                         enter ? tr("进入这台电脑时执行") : tr("离开这台电脑时执行"),
                         enter ? tr("指针切入本机时") : tr("指针切出本机时"), check("maintenance", item.first)
                     )}
    );
    contents(maintenance)->addWidget(text("maintenance", item.second));
  }
  addSave(maintenance, "maintenance", tr("保存维护配置"));
  contents(maintenance)->addWidget(label(tr("备份、恢复与清除"), "subtitle"));
  auto *backup = button(tr("导出备份…"));
  auto *restore = button(tr("恢复备份…"));
  auto *resetPreferences = button(tr("恢复普通偏好…"));
  connect(resetPreferences, &QPushButton::clicked, this, [this] {
    if (QMessageBox::question(
            this, tr("恢复普通偏好"),
            tr("恢复语言、窗口、后台图标、更新检查和电源偏好？设备、规则、信任、证书、网络及外部文件保持保留。"),
            QMessageBox::RestoreDefaults | QMessageBox::Cancel, QMessageBox::Cancel
        ) == QMessageBox::RestoreDefaults)
      resetOrdinaryPreferences();
  });
  auto *clear = button(tr("清除配置…"));
  clear->setProperty("danger", true);
  group(
      maintenance, {row(tr("恢复普通偏好"), tr("仅重置显示、窗口、更新检查和电源偏好"), resetPreferences),
                    row(tr("完整配置备份"), tr("包含公共信任记录，不包含私钥"), backup),
                    row(tr("恢复已保存的配置"), tr("停止共享后恢复；恢复完成不自动连接"), restore),
                    row(tr("清除当前应用配置"), tr("先预览文件范围；不删除外部文件或卸载系统服务"), clear)}
  );
  connect(backup, &QPushButton::clicked, this, &SettingsWorkspace::requestBackup);
  connect(restore, &QPushButton::clicked, this, &SettingsWorkspace::requestRestore);
  connect(clear, &QPushButton::clicked, this, &SettingsWorkspace::requestClear);
  addPage("maintenance", maintenance);
  auto *logs = page(tr("技术日志设置"), tr("日志偏好独立保存，运行中的核心参数仍需应用。"));
  QList<QPair<QString, QVariant>> levels;
  for (int i = 0; i < 6; ++i)
    levels.append({LogLevel::logLevelNames()[i], LogLevel::toOption(i)});
  group(
      logs, {row(tr("日志等级"), tr("Debug 与 Verbose 可能影响性能"), choice("logs", Settings::Log::Level, levels)),
             row(tr("GUI 调试信息"), tr("与共享核心日志分开"), check("logs", Settings::Log::GuiDebug)),
             row(tr("记录到文件"), tr("写入失败时保留界面日志"), check("logs", Settings::Log::ToFile))}
  );
  auto *logPath = text("logs", Settings::Log::File);
  auto *chooseLog = button(tr("选择位置…"));
  auto *logLocation = new QHBoxLayout;
  logLocation->addWidget(logPath, 1);
  logLocation->addWidget(chooseLog);
  contents(logs)->addLayout(logLocation);
  connect(chooseLog, &QPushButton::clicked, this, [this, logPath] {
    const auto path =
        QFileDialog::getSaveFileName(this, tr("选择日志位置"), logPath->text(), tr("日志文件 (*.log);;所有文件 (*)"));
    if (!path.isEmpty()) {
      logPath->setText(path);
      changed(Settings::Log::File);
    }
  });
  addSave(logs, "logs", tr("保存日志设置"));
  addPage("logs", logs);
  m_advancedLinks = new QWidget;
  auto *advancedLayout = new QHBoxLayout(m_advancedLinks);
  advancedLayout->setContentsMargins(0, 0, 0, 0);
  advancedLayout->addStretch();
  for (const auto &link : QList<QPair<QString, QString>>{
           {"certificates", tr("本机证书")}, {"config", tr("配置文件")}, {"maintenance", tr("维护与自动化")}
       }) {
    auto *action = button(link.second);
    advancedLayout->addWidget(action);
    connect(action, &QPushButton::clicked, this, [this, section = link.first] { navigate(section); });
  }
  qobject_cast<QVBoxLayout *>(layout())->addWidget(m_advancedLinks);
  setManagedExternally(false, false);
}
} // namespace deskflow::gui::workspace
