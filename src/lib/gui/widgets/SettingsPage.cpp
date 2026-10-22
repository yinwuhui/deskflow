/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "SettingsPage.h"

#include "common/I18N.h"
#include "common/LogLevel.h"
#include "common/NetworkProtocol.h"
#include "common/Settings.h"
#include "gui/TlsUtility.h"
#include "gui/core/NetworkMonitor.h"
#include "gui/dialogs/ActionDialog.h"
#include "gui/widgets/KeySequenceWidget.h"
#ifdef Q_OS_MACOS
#include "gui/OSXHelpers.h"
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

using namespace deskflow::gui;

namespace {
class SettingsSwitch : public QCheckBox
{
public:
  explicit SettingsSwitch(QWidget *parent) : QCheckBox(parent)
  {
    setFixedSize(38, 28);
  }

protected:
  bool hitButton(const QPoint &pos) const override
  {
    return rect().contains(pos);
  }
  void paintEvent(QPaintEvent *) override
  {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(isEnabled() ? 1 : .4);
    p.setPen(Qt::NoPen);
    p.setBrush(isChecked() ? QColor("#087fff") : QColor("#cdd2d9"));
    p.drawRoundedRect(QRectF(2, 5, 34, 19), 9.5, 9.5);
    p.setBrush(Qt::white);
    p.drawEllipse(QRectF(isChecked() ? 20 : 5, 8, 13, 13));
    if (hasFocus()) {
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(QColor("#087fff"), 1));
      p.drawRoundedRect(QRectF(0.5, 2.5, 37, 24), 12, 12);
    }
  }
};
constexpr auto kStartup = "@loginStartup";
} // namespace

SettingsPage::SettingsPage(ServerConfig &config, QWidget *parent, LoginStartup startup)
    : QWidget(parent),
      m_config(config),
      m_baseline(config),
      m_draft(config),
      m_startup(std::move(startup))
{
  setObjectName("settingsPage");
  setStyleSheet(QStringLiteral(R"(
    QWidget#settingsPage, QWidget#settingsPage QScrollArea, QWidget#settingsPage QWidget[settingsContent="true"] { background: white; border: none; }
    QWidget#settingsPage QLabel { color: #2b3037; font-size: 14px; background: transparent; }
    QWidget#settingsPage QLabel[role="muted"] { color: #89929f; font-size: 12px; }
    QWidget#settingsPage QLabel[role="heading"] { font-size: 17px; font-weight: 500; }
    QWidget#settingsPage QFrame[settingsGroup="true"] { background: white; border: 1px solid #e5e8ed; border-radius: 11px; }
    QWidget#settingsPage QWidget[settingsRow="true"] { border: none; border-bottom: 1px solid #e5e8ed; }
    QWidget#settingsPage QTabBar { background: white; border-bottom: 1px solid #e5e8ed; }
    QWidget#settingsPage QTabBar::tab { background: white; color: #838992; padding: 9px 9px 16px; border: none; border-bottom: 3px solid transparent; }
    QWidget#settingsPage QTabBar::tab:selected { color: #2b3037; border-bottom-color: #087fff; }
    QWidget#settingsPage QTabBar::tab:focus { color: #087fff; }
    QWidget#settingsPage QComboBox, QWidget#settingsPage QSpinBox, QWidget#settingsPage QDoubleSpinBox,
    QWidget#settingsPage QLineEdit { background: white; color: #59636f; border: 1px solid #dce1e7; padding: 8px 10px; border-radius: 7px; min-height: 18px; selection-background-color: #087fff; }
    QWidget#settingsPage QComboBox:focus, QWidget#settingsPage QLineEdit:focus { border-color: #087fff; }
    QWidget#settingsPage QComboBox::drop-down { border: none; width: 22px; }
    QWidget#settingsPage QPushButton { background: white; border: 1px solid #dce1e7; color: #397ebf; border-radius: 7px; padding: 8px 16px; font-size: 13px; }
    QWidget#settingsPage QPushButton:hover { background: #f2f7fe; }
    QWidget#settingsPage QPushButton[settingsPrimary="true"] { background: #087fff; border-color: #087fff; color: white; }
    QWidget#settingsPage QPushButton:disabled { color: #a5adb8; background: #f5f7fa; border-color: #edf0f4; }
    QWidget#settingsPage QListWidget { border: 1px solid #e5e8ed; border-radius: 7px; background: white; }
    QWidget#settingsPage QListWidget::item { padding: 10px; }
    QWidget#settingsPage QListWidget::item:selected { background: #eaf3ff; color: #267ed9; }
    QWidget#settingsPage QWidget#settingsFooter { border-top: 1px solid #e5e8ed; background: white; }
    QWidget#settingsPage QLabel[settingsError="true"] { color: #b45345; }
  )"));
  build();
  load();
  connect(Settings::instance(), &Settings::settingsWritableChanged, this, &SettingsPage::updateState);
}

QPushButton *SettingsPage::button(const QString &text, const QString &name)
{
  auto b = new QPushButton(text, this);
  b->setObjectName(name);
  return b;
}

QVBoxLayout *SettingsPage::category(const QString &title, const QString &description)
{
  auto scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto content = new QWidget(scroll);
  content->setProperty("settingsContent", true);
  auto layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 25, 0, 20);
  layout->setSpacing(18);
  auto heading = new QLabel(title, content);
  heading->setProperty("role", "heading");
  layout->addWidget(heading);
  auto intro = new QLabel(description, content);
  intro->setProperty("role", "muted");
  intro->setWordWrap(true);
  layout->addWidget(intro);
  scroll->setWidget(content);
  m_categories->addWidget(scroll);
  return layout;
}

QVBoxLayout *SettingsPage::group(QVBoxLayout *parent)
{
  auto frame = new QFrame(this);
  frame->setProperty("settingsGroup", true);
  auto layout = new QVBoxLayout(frame);
  layout->setContentsMargins(19, 0, 19, 0);
  layout->setSpacing(0);
  parent->addWidget(frame);
  return layout;
}

QWidget *SettingsPage::row(QVBoxLayout *parent, const QString &title, const QString &description, QWidget *control)
{
  auto widget = new QWidget(this);
  widget->setProperty("settingsRow", true);
  widget->setMinimumHeight(78);
  auto layout = new QHBoxLayout(widget);
  layout->setContentsMargins(0, 13, 0, 13);
  layout->setSpacing(15);
  auto copy = new QVBoxLayout;
  copy->setSpacing(3);
  auto label = new QLabel(title, widget);
  label->setWordWrap(true);
  label->setBuddy(control);
  copy->addWidget(label);
  if (!description.isEmpty()) {
    auto detail = new QLabel(description, widget);
    detail->setProperty("role", "muted");
    detail->setWordWrap(true);
    copy->addWidget(detail);
  }
  layout->addLayout(copy, 1);
  if (control) {
    control->setAccessibleName(title);
    layout->addWidget(control, 0, Qt::AlignVCenter);
  }
  parent->addWidget(widget);
  return widget;
}

void SettingsPage::bind(
    const QString &key, QWidget *control, std::function<QVariant()> read, std::function<void(const QVariant &)> write,
    bool restart
)
{
  control->setObjectName(key);
  m_bindings.append({key, control, std::move(read), std::move(write), {}, restart});
}

QCheckBox *SettingsPage::toggle(
    QVBoxLayout *parent, const QString &key, const QString &title, const QString &description, bool restart
)
{
  auto control = new SettingsSwitch(this);
  row(parent, title, description, control);
  bind(
      key, control, [control] { return control->isChecked(); },
      [control](const QVariant &v) { control->setChecked(v.toBool()); }, restart
  );
  connect(control, &QCheckBox::toggled, this, &SettingsPage::updateState);
  return control;
}

QComboBox *SettingsPage::choice(
    QVBoxLayout *parent, const QString &key, const QString &title, const QString &description,
    const QList<QPair<QString, QVariant>> &items, bool restart
)
{
  auto control = new QComboBox(this);
  control->setFixedWidth(158);
  for (const auto &[text, value] : items)
    control->addItem(text, value);
  row(parent, title, description, control);
  bind(
      key, control, [control] { return control->currentData(); },
      [control](const QVariant &v) {
        auto index = control->findData(v);
        if (index < 0) {
          control->addItem(v.toString(), v);
          index = control->count() - 1;
        }
        control->setCurrentIndex(index);
      },
      restart
  );
  connect(control, &QComboBox::currentIndexChanged, this, &SettingsPage::updateState);
  return control;
}

QWidget *SettingsPage::number(
    QVBoxLayout *parent, const QString &key, const QString &title, const QString &description, int minimum, int maximum,
    const QString &suffix, bool decimal
)
{
  QWidget *control;
  if (decimal) {
    auto spin = new QDoubleSpinBox(this);
    spin->setRange(.1, 10);
    spin->setSingleStep(.1);
    spin->setDecimals(1);
    spin->setSuffix(suffix);
    control = spin;
    bind(
        key, spin, [spin] { return spin->value(); }, [spin](const QVariant &v) { spin->setValue(v.toDouble()); }, true
    );
    connect(spin, &QDoubleSpinBox::valueChanged, this, &SettingsPage::updateState);
  } else {
    auto spin = new QSpinBox(this);
    spin->setRange(minimum, maximum);
    spin->setSuffix(suffix);
    control = spin;
    bind(key, spin, [spin] { return spin->value(); }, [spin](const QVariant &v) { spin->setValue(v.toInt()); }, true);
    connect(spin, &QSpinBox::valueChanged, this, &SettingsPage::updateState);
  }
  control->setFixedWidth(125);
  return row(parent, title, description, control);
}

QWidget *SettingsPage::path(QVBoxLayout *parent, const QString &key, const QString &title, const QString &description)
{
  auto block = new QWidget(this);
  auto layout = new QVBoxLayout(block);
  layout->setContentsMargins(0, 13, 0, 13);
  auto label = new QLabel(title, block);
  layout->addWidget(label);
  auto detail = new QLabel(description, block);
  detail->setProperty("role", "muted");
  detail->setWordWrap(true);
  layout->addWidget(detail);
  auto controls = new QHBoxLayout;
  auto edit = new QLineEdit(block);
  edit->setAccessibleName(title);
  auto browse = button(tr("Browse"));
  controls->addWidget(edit, 1);
  controls->addWidget(browse);
  layout->addLayout(controls);
  parent->addWidget(block);
  bind(
      key, edit, [edit] { return edit->text().trimmed(); }, [edit](const QVariant &v) { edit->setText(v.toString()); },
      true
  );
  connect(edit, &QLineEdit::textChanged, this, &SettingsPage::updateState);
  connect(browse, &QPushButton::clicked, this, [this, edit, title] {
    const auto result =
        QFileDialog::getSaveFileName(this, title, edit->text(), {}, nullptr, QFileDialog::DontConfirmOverwrite);
    if (!result.isEmpty())
      edit->setText(result);
  });
  return block;
}

void SettingsPage::build()
{
  auto outer = new QVBoxLayout(this);
  outer->setContentsMargins(29, 12, 29, 22);
  outer->setSpacing(0);
  m_tabs = new QTabBar(this);
  m_tabs->setObjectName("settingsTabs");
  m_tabs->setExpanding(true);
  m_tabs->setUsesScrollButtons(true);
  for (const auto &name :
       {tr("Basic settings"), tr("Input settings"), tr("Hotkeys"), tr("Security"), tr("Network"), tr("Diagnostics"),
        tr("More")})
    m_tabs->addTab(name);
  outer->addWidget(m_tabs);
  m_categories = new QStackedWidget(this);
  m_categories->setObjectName("settingsCategories");
  outer->addWidget(m_categories, 1);
  connect(m_tabs, &QTabBar::currentChanged, m_categories, &QStackedWidget::setCurrentIndex);

  auto basic =
      category(tr("Basic settings"), tr("Adjust application display, window behavior and everyday preferences."));
  auto g = group(basic);
  m_startupToggle = toggle(
      g, kStartup, tr("Start automatically at login"),
      tr("Run busymouse automatically after signing in to this computer."), false
  );
  choice(
      g, Settings::Gui::Autohide, tr("When the application starts"),
      tr("Choose whether to show the main window or run in the background."),
      {{tr("Show main window"), false}, {tr("Run in background"), true}}, false
  );
  choice(
      g, Settings::Gui::CloseToTray, tr("When the main window closes"),
      tr("A background application remains available from the tray or menu bar."),
      {{tr("Run in background"), true}, {tr("Exit application"), false}}, false
  );
  toggle(
      g, Settings::Core::PreventSleep, tr("Prevent this computer from sleeping"),
      tr("Avoid interrupting input sharing when the system goes to sleep.")
  );
  g = group(basic);
  I18N::reDetectLanguages();
  QList<QPair<QString, QVariant>> languages{{QStringLiteral("English"), QStringLiteral("en")}};
  for (const auto &name : I18N::detectedLanguages())
    languages.append({name, I18N::nativeTo639Name(name)});
  choice(
      g, Settings::Core::Language, tr("Interface language"), tr("Select the language used by busymouse."), languages,
      false
  );
  choice(
      g, Settings::Gui::SymbolicTrayIcon, tr("Tray icon style"),
      tr("Choose an appearance for the tray or menu bar icon."), {{tr("Monochrome"), true}, {tr("Colorful"), false}},
      false
  );
  const bool updatesAvailable = !Settings::value(Settings::Gui::UpdateCheckUrl).toString().trimmed().isEmpty();
  auto updateCheck = toggle(
      g, Settings::Gui::AutoUpdateCheck, tr("Check for updates on startup"),
      updatesAvailable ? tr("Check whether a new application version is available.")
                       : tr("Automatic update checks are unavailable in this build."),
      false
  );
  updateCheck->setEnabled(updatesAvailable);
  auto extra = button(tr("Other display options"));
  basic->addWidget(extra, 0, Qt::AlignLeft);
  auto other = new QWidget(this);
  auto otherLayout = new QVBoxLayout(other);
  toggle(otherLayout, Settings::Gui::ShowVersionInTitle, tr("Show version in window title"), {}, false);
  basic->addWidget(other);
  other->hide();
  connect(extra, &QPushButton::clicked, other, [other] { other->setVisible(other->isHidden()); });
  basic->addStretch();

  auto input = category(
      tr("Input settings"),
      tr("Sharing and receiving settings are separate. Device-specific settings are saved per computer.")
  );
  g = group(input);
  auto clipboard = toggle(
      g, Settings::Server::EnableClipboard, tr("Share clipboard"),
      tr("Synchronize clipboard contents between connected computers.")
  );
  auto clipLimit = number(
      g, Settings::Server::ClipboardSize, tr("Clipboard size limit"),
      tr("Limit the amount of clipboard data sent at once."), 1, 9999, tr(" MB")
  );
  connect(clipboard, &QCheckBox::toggled, clipLimit, &QWidget::setEnabled);
  toggle(
      g, Settings::Server::RelativeMouseMoves, tr("Use relative mouse movement"),
      tr("For applications that require relative pointer movement.")
  );
  g = group(input);
  auto delay = toggle(
      g, Settings::Server::EnableSwitchDelay, tr("Wait before switching screens"),
      tr("Pause at the screen edge before switching to another computer.")
  );
  auto delayTime = number(g, Settings::Server::SwitchDelay, tr("Edge delay"), {}, 1, 10000, tr(" ms"));
  connect(delay, &QCheckBox::toggled, delayTime, &QWidget::setVisible);
  auto doubleTap = toggle(
      g, Settings::Server::EnableSwitchDoubleTap, tr("Double tap the edge to switch"),
      tr("Touch the edge twice within the selected interval.")
  );
  auto doubleTime = number(g, Settings::Server::SwitchDoubleTap, tr("Double tap interval"), {}, 1, 10000, tr(" ms"));
  connect(doubleTap, &QCheckBox::toggled, doubleTime, &QWidget::setVisible);
  toggle(
      g, Settings::Server::DefaultLockToComputerState, tr("Lock to this computer at startup"),
      tr("Prevent cross-screen movement when sharing first starts.")
  );
  toggle(
      g, Settings::Server::DisableLockToComputer, tr("Disable Scroll Lock locking"),
      tr("Disable using Scroll Lock to lock the pointer to a computer.")
  );
#ifdef Q_OS_WIN
  toggle(g, Settings::Server::Win32KeepForeground, tr("Keep the foreground window"), tr("Windows only."));
#endif
  auto receiving = button(tr("Receiving: keyboard and scrolling"));
  input->addWidget(receiving, 0, Qt::AlignLeft);
  auto receive = new QWidget(this);
  auto receiveLayout = new QVBoxLayout(receive);
  toggle(
      receiveLayout, Settings::Client::LanguageSync, tr("Use the sharing computer's keyboard language"),
      tr("Applies when this computer receives input from another computer.")
  );
  toggle(receiveLayout, Settings::Client::InvertXScroll, tr("Invert horizontal scrolling"), {});
  number(receiveLayout, Settings::Client::XScrollScale, tr("Horizontal scroll scale"), {}, 0, 10, tr(" ×"), true);
  toggle(receiveLayout, Settings::Client::InvertYScroll, tr("Invert vertical scrolling"), {});
  number(receiveLayout, Settings::Client::YScrollScale, tr("Vertical scroll scale"), {}, 0, 10, tr(" ×"), true);
  input->addWidget(receive);
  receive->hide();
  connect(receiving, &QPushButton::clicked, receive, [receive] { receive->setVisible(receive->isHidden()); });
  buildDeviceEditor(input);
  input->addStretch();

  auto hotkeys = category(
      tr("Hotkeys"), tr("Configure keyboard or mouse triggers and the actions performed by the sharing computer.")
  );
  buildHotkeys(hotkeys);
  hotkeys->addStretch();

  auto security =
      category(tr("Security"), tr("Manage connection encryption, certificates and system input-control permissions."));
  g = group(security);
  auto tls = toggle(
      g, Settings::Security::TlsEnabled, tr("Enable TLS encryption"),
      tr("Encrypt input and clipboard data transmitted between computers.")
  );
  auto certCheck = toggle(
      g, Settings::Security::CheckPeers, tr("Require client certificates"),
      tr("The sharing computer verifies the identity of connecting computers.")
  );
  g = group(security);
  auto certPath =
      path(g, Settings::Security::Certificate, tr("Certificate file"), tr("Choose the local PEM certificate file."));
  auto keySize = choice(
      g, Settings::Security::KeySize, tr("Certificate key length"),
      tr("Used when generating a new certificate; existing certificates are not replaced automatically."),
      {{tr("2048 bits"), 2048}, {tr("4096 bits"), 4096}}
  );
  auto regen = button(tr("Regenerate certificate"), "regenerateCertificate");
  row(g, tr("Regenerate certificate"), tr("Other computers will need to verify and trust the new certificate."), regen);
  auto fingerprint = new QLabel(this);
  fingerprint->setWordWrap(true);
  fingerprint->setTextInteractionFlags(Qt::TextSelectableByMouse);
  fingerprint->setProperty("role", "muted");
  fingerprint->setObjectName("settingsFingerprint");
  security->addWidget(fingerprint);
  connect(regen, &QPushButton::clicked, this, [this] {
    m_regenerate = !m_regenerate;
    updateState();
    message(
        m_regenerate ? tr("A new certificate will be generated when you save. Click again to cancel.")
                     : tr("Certificate regeneration cancelled.")
    );
  });
  connect(tls, &QCheckBox::toggled, this, [this, certCheck, certPath, keySize, regen](bool enabled) {
    for (auto w :
         {static_cast<QWidget *>(certCheck), certPath, static_cast<QWidget *>(keySize), static_cast<QWidget *>(regen)})
      w->setEnabled(enabled && Settings::isWritable());
    certCheck->setEnabled(
        enabled && Settings::isWritable() && Settings::value(Settings::Core::CoreMode) != Settings::Client
    );
  });
#ifdef Q_OS_MACOS
  g = group(security);
  auto permissions = button(tr("Open system permissions"));
  row(g, tr("Input-control permission"),
      tr("macOS manages this permission. Input sharing requires your authorization."), permissions);
  connect(permissions, &QPushButton::clicked, this, [] {
    QDesktopServices::openUrl(QUrl("x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"));
  });
#endif
  security->addStretch();

  auto network = category(tr("Network"), tr("Configure the listening address, port and connection compatibility."));
  g = group(network);
  QList<QPair<QString, QVariant>> interfaces{{tr("Automatic"), QString()}};
  for (const auto &ip : NetworkMonitor::validAddresses())
    interfaces.append({ip, ip});
  choice(
      g, Settings::Core::Interface, tr("Listening address"),
      tr("Select a local address when this computer has multiple network interfaces."), interfaces
  );
  number(g, Settings::Core::Port, tr("Connection port"), tr("Both computers must use the same port."), 1, 65535);
  g = group(network);
  choice(
      g, Settings::Server::Protocol, tr("Compatibility protocol"),
      tr("The receiving computer selects the sharing computer's protocol automatically."),
      {{QStringLiteral("Synergy"), QStringLiteral("synergy")}, {QStringLiteral("Barrier"), QStringLiteral("barrier")}}
  );
  auto heartbeat = toggle(
      g, Settings::Server::EnableHeartbeat, tr("Check connection response"),
      tr("Periodically check whether connected computers are responding.")
  );
  auto rate = number(g, Settings::Server::Heartbeat, tr("Check interval"), {}, 1, 1000000, tr(" ms"));
  connect(heartbeat, &QCheckBox::toggled, rate, &QWidget::setVisible);
  g = group(network);
  toggle(
      g, Settings::Client::DynamicConnectionRetry, tr("Use a dynamic reconnect interval"),
      tr("Receiving mode: extend repeated failed reconnect attempts up to five minutes.")
  );
  auto networkHint = new QLabel(
      tr("Configure the sharing computer's address on Home. Saving network changes restarts an active sharing service."
      ),
      this
  );
  networkHint->setProperty("role", "muted");
  networkHint->setWordWrap(true);
  network->addWidget(networkHint);
  network->addStretch();

  auto logs =
      category(tr("Diagnostics"), tr("Keep connection diagnostics to investigate startup and connection problems."));
  g = group(logs);
  QList<QPair<QString, QVariant>> levels;
  const auto names = LogLevel::logLevelNames();
  const auto options = LogLevel::logLevelOptions();
  for (int i = 0; i < names.size(); ++i)
    levels.append({names[i], options[i]});
  choice(
      g, Settings::Log::Level, tr("Log level"),
      tr("Use Info for everyday use. More detailed logs may affect performance."), levels
  );
  toggle(
      g, Settings::Log::GuiDebug, tr("Include GUI debug messages"),
      tr("Record diagnostics from the application interface.")
  );
  g = group(logs);
  auto toFile =
      toggle(g, Settings::Log::ToFile, tr("Write logs to a file"), tr("Keep log output for later inspection."));
  auto logPath = path(g, Settings::Log::File, tr("Log file"), tr("Select where to save the log output."));
  connect(toFile, &QCheckBox::toggled, logPath, &QWidget::setVisible);
  g = group(logs);
  auto showLogs = button(tr("View runtime logs"));
  row(g, tr("Runtime logs"), tr("Expand the existing log viewer without leaving this page."), showLogs);
  connect(showLogs, &QPushButton::clicked, this, &SettingsPage::showLogsRequested);
  logs->addStretch();

  auto more = category(tr("More settings"), tr("Cross-screen automation commands and configuration maintenance."));
  g = group(more);
  auto externalConfig = toggle(
      g, Settings::Server::ExternalConfig, tr("Use an external server configuration"),
      tr("An external file replaces the arrangement canvas and its generated links.")
  );
  auto externalPath = path(
      g, Settings::Server::ExternalConfigFile, tr("Server configuration file"),
      tr("Choose an existing readable busymouse server configuration file.")
  );
  connect(externalConfig, &QCheckBox::toggled, externalPath, &QWidget::setVisible);
  g = group(more);
  auto enter = toggle(
      g, Settings::Core::EnableEnterCommand, tr("Run a command when entering this computer"),
      tr("Run when the pointer switches from another computer to this one.")
  );
  auto enterCmd = path(
      g, Settings::Core::ComputerEnterCommand, tr("Enter command"), tr("Only enter a local command you understand.")
  );
  // Commands are text, not files: remove the file picker from these rows.
  enterCmd->findChild<QPushButton *>()->hide();
  connect(enter, &QCheckBox::toggled, enterCmd, &QWidget::setVisible);
  auto exit = toggle(
      g, Settings::Core::EnableExitCommand, tr("Run a command when leaving this computer"),
      tr("Run when the pointer switches away from this computer.")
  );
  auto exitCmd = path(
      g, Settings::Core::ComputerExitCommand, tr("Exit command"), tr("Only enter a local command you understand.")
  );
  exitCmd->findChild<QPushButton *>()->hide();
  connect(exit, &QCheckBox::toggled, exitCmd, &QWidget::setVisible);
#ifdef Q_OS_WIN
  if (!Settings::isPortableMode()) {
    g = group(more);
    choice(
        g, Settings::Core::ProcessMode, tr("Run as a background service"),
        tr("Windows service mode supports running outside the desktop session."),
        {{tr("Desktop application"), Settings::Desktop}, {tr("Background service"), Settings::Service}}
    );
    toggle(
        g, Settings::Daemon::Elevate, tr("Always run the service as system"),
        tr("Allow operation at the login screen and UAC prompts.")
    );
  }
#endif
  g = group(more);
  auto defaults = button(tr("Restore defaults"), "restoreSettingsDefaults");
  row(g, tr("Restore settings defaults"), tr("Load default values into this page, then save to apply them."), defaults);
  connect(defaults, &QPushButton::clicked, this, &SettingsPage::restoreDefaults);
  auto clear = button(tr("Clear all settings"), "clearAllSettings");
  row(g, tr("Clear all configuration"), tr("Remove the local configuration and restart. This cannot be undone."),
      clear);
  auto confirm = new QWidget(this);
  auto confirmLayout = new QVBoxLayout(confirm);
  auto warning = new QLabel(tr("Clear all settings and restart busymouse?"), confirm);
  warning->setWordWrap(true);
  confirmLayout->addWidget(warning);
  auto yes = button(tr("Confirm clear and restart"));
  auto no = button(tr("Cancel"));
  confirmLayout->addWidget(yes);
  confirmLayout->addWidget(no);
  g->addWidget(confirm);
  confirm->hide();
  connect(clear, &QPushButton::clicked, confirm, &QWidget::show);
  connect(no, &QPushButton::clicked, confirm, &QWidget::hide);
  connect(yes, &QPushButton::clicked, this, [this] {
    QString error;
    if (!m_startup.setEnabled(false, error)) {
      message(error, true);
      return;
    }
    Q_EMIT clearSettingsRequested();
  });
  more->addStretch();

  auto footer = new QWidget(this);
  footer->setObjectName("settingsFooter");
  auto footerLayout = new QHBoxLayout(footer);
  footerLayout->setContentsMargins(0, 18, 0, 0);
  auto text = new QVBoxLayout;
  m_state = new QLabel(this);
  m_state->setObjectName("settingsSaveState");
  m_state->setWordWrap(true);
  m_state->setProperty("role", "muted");
  m_hint = new QLabel(this);
  m_hint->setProperty("role", "muted");
  m_hint->setWordWrap(true);
  text->addWidget(m_state);
  text->addWidget(m_hint);
  footerLayout->addLayout(text, 1);
  m_discard = button(tr("Discard changes"), "discardSettings");
  m_save = button(tr("Save settings"), "saveSettings");
  m_save->setProperty("settingsPrimary", true);
  footerLayout->addWidget(m_discard);
  footerLayout->addWidget(m_save);
  outer->addWidget(footer);
  connect(m_save, &QPushButton::clicked, this, [this] { save(); });
  connect(m_discard, &QPushButton::clicked, this, &SettingsPage::discard);
}

bool SettingsPage::serverDraftModified() const
{
  return !(m_draft == m_baseline);
}

bool SettingsPage::isModified() const
{
  if (m_regenerate || serverDraftModified())
    return true;
  for (const auto &b : m_bindings)
    if (b.read() != b.original)
      return true;
  return false;
}

void SettingsPage::load()
{
  m_loading = true;
  clearEditor();
  m_baseline = m_config;
  m_draft = m_config;
  m_startupOriginal = m_startup.isEnabled();
  m_regenerate = false;
  for (auto &b : m_bindings) {
    b.original = b.key == kStartup ? QVariant(m_startupOriginal) : Settings::value(b.key);
    if (b.key == Settings::Core::Interface)
      b.original = b.original.toString();
    if (b.key == Settings::Core::Language && b.original.toString().isEmpty())
      b.original = I18N::currentLanguage();
    b.write(b.original);
    b.original = b.read();
  }
  m_devices->clear();
  for (int i = 0; i < m_draft.computers().size(); ++i)
    if (!m_draft.computers()[i].isNull())
      m_devices->addItem(m_draft.computers()[i].name(), i);
  selectDevice(m_devices->currentIndex());
  refreshHotkeys();
  auto fingerprint = findChild<QLabel *>("settingsFingerprint");
  const auto bytes = TlsUtility::certFingerprint();
  fingerprint->setText(
      bytes.isEmpty() ? tr("No valid local certificate is available.")
                      : tr("Local SHA-256 fingerprint: %1").arg(QString::fromLatin1(bytes.toHex(':')))
  );
  // Emit dependency signals even when the loaded value matches the initial unchecked state.
  for (const auto &b : m_bindings)
    if (auto check = qobject_cast<QCheckBox *>(b.control))
      Q_EMIT check->toggled(check->isChecked());
  for (auto w : m_serverControls)
    w->setEnabled(!m_config.useExternalConfig() && Settings::isWritable());
  m_loading = false;
  updateState();
}

void SettingsPage::activate(Category category)
{
  if (!isModified())
    load();
  m_tabs->setCurrentIndex(category);
}

void SettingsPage::discard()
{
  load();
}

void SettingsPage::activateComputer(const QString &name)
{
  activate(Input);
  const int index = m_devices->findText(name, Qt::MatchExactly);
  if (index >= 0) {
    m_devices->setCurrentIndex(index);
    m_deviceFields->show();
    m_devices->setFocus();
  }
}

void SettingsPage::updateState()
{
  if (m_loading || !m_save)
    return;
  const bool writable = Settings::isWritable();
  const bool dirty = isModified();
  m_save->setEnabled(writable && dirty && !m_editor && m_triggerEditor->isHidden());
  m_discard->setEnabled(dirty || m_editor || !m_triggerEditor->isHidden());
  m_categories->setEnabled(writable);
  m_state->setProperty("settingsError", false);
  m_state->setText(
      !writable ? tr("Settings are read-only.")
      : dirty   ? tr("There are unsaved changes")
                : tr("All changes saved")
  );
  m_hint->setText(
      (m_editor || !m_triggerEditor->isHidden()) ? tr("Finish or cancel the inline editor before saving.")
      : dirty ? tr("Switching categories keeps your changes. Save to apply them together.")
              : tr("Changes take effect after you save.")
  );
}

void SettingsPage::message(const QString &text, bool error)
{
  m_state->setText(text);
  m_state->setProperty("settingsError", error);
  m_state->style()->unpolish(m_state);
  m_state->style()->polish(m_state);
}

void SettingsPage::restoreDefaults()
{
  m_loading = true;
  clearEditor();
  for (const auto &b : m_bindings)
    b.write(b.key == kStartup ? QVariant(false) : Settings::defaultValue(b.key));
  m_regenerate = false;
  m_loading = false;
  updateState();
  message(tr("Default values loaded. Save to apply them."));
}

bool SettingsPage::save()
{
  if (!Settings::isWritable() || m_editor || !m_triggerEditor->isHidden())
    return false;
  if (serverDraftModified() && !(m_config == m_baseline)) {
    message(tr("Computer configuration changed while editing. Discard and reopen settings before saving."), true);
    return false;
  }
  for (const auto &b : m_bindings) {
    if (auto spin = qobject_cast<QAbstractSpinBox *>(b.control); spin && !spin->hasAcceptableInput()) {
      message(tr("Enter a valid value before saving."), true);
      b.control->setFocus();
      return false;
    }
    if ((b.key == Settings::Core::EnableEnterCommand || b.key == Settings::Core::EnableExitCommand) &&
        b.read().toBool()) {
      const auto commandKey = b.key == Settings::Core::EnableEnterCommand ? Settings::Core::ComputerEnterCommand
                                                                          : Settings::Core::ComputerExitCommand;
      for (const auto &cmd : m_bindings)
        if (cmd.key == commandKey && cmd.read().toString().isEmpty()) {
          message(tr("Enabled automation commands cannot be empty."), true);
          return false;
        }
    }
  }
  for (const auto &h : m_draft.hotkeys())
    if (h.actions().isEmpty()) {
      message(tr("Each hotkey needs at least one action."), true);
      m_tabs->setCurrentIndex(Hotkeys);
      return false;
    }
  auto value = [this](const QString &key) {
    for (const auto &b : m_bindings)
      if (b.key == key)
        return b.read();
    return QVariant();
  };
  if (value(Settings::Log::ToFile).toBool() && value(Settings::Log::File).toString().isEmpty()) {
    message(tr("Choose a log file before saving."), true);
    return false;
  }
  if (value(Settings::Security::TlsEnabled).toBool()) {
    const auto cert = value(Settings::Security::Certificate).toString();
    if (cert.isEmpty() || (!m_regenerate && !TlsUtility::isCertValid(cert))) {
      message(tr("Choose a valid certificate or select Regenerate certificate before saving."), true);
      m_tabs->setCurrentIndex(Security);
      return false;
    }
    if (!m_regenerate &&
        value(Settings::Security::KeySize).toInt() != Settings::value(Settings::Security::KeySize).toInt()) {
      message(tr("Select Regenerate certificate to apply a new key length."), true);
      return false;
    }
  }
  QString error;
  if (value(Settings::Server::ExternalConfig).toBool()) {
    QFile file(value(Settings::Server::ExternalConfigFile).toString());
    if (!file.open(QIODevice::ReadOnly)) {
      message(tr("Choose a readable external configuration file before saving."), true);
      return false;
    }
  }
  if (m_startupToggle->isChecked() != m_startupOriginal && !m_startup.setEnabled(m_startupToggle->isChecked(), error)) {
    message(error, true);
    return false;
  }
  bool restart = serverDraftModified() || m_regenerate;
  QMap<QString, QVariant> old;
  QSignalBlocker settingsBlocker(Settings::instance());
  for (const auto &b : m_bindings)
    if (b.key != kStartup && b.read() != b.original) {
      old.insert(b.key, Settings::value(b.key));
      restart |= b.restart;
      Settings::setValue(b.key, b.read());
    }
  if (m_regenerate && !TlsUtility::generateCertificate()) {
    for (auto it = old.cbegin(); it != old.cend(); ++it)
      Settings::setValue(it.key(), it.value());
    if (m_startupToggle->isChecked() != m_startupOriginal)
      m_startup.setEnabled(m_startupOriginal, error);
    message(tr("Could not generate the certificate. Changes were not applied."), true);
    return false;
  }
  if (old.contains(Settings::Server::EnableClipboard)) {
    Settings::setValue(Settings::Server::XdpRestoreToken, QString());
    Settings::setValue(Settings::Server::XdpClipboardRetried, false);
  }
  if (serverDraftModified())
    m_config = m_draft;
  m_config.commit();
  Settings::save();
  settingsBlocker.unblock();
  for (auto it = old.cbegin(); it != old.cend(); ++it)
    Q_EMIT Settings::instance()->settingsChanged(it.key());
  // Load before emitting: language changes may rebuild the page during the receiver's applyConfig.
  load();
  Q_EMIT saved(restart);
  const auto language = value(Settings::Core::Language).toString();
  if (!language.isEmpty() && language != I18N::currentLanguage())
    I18N::setLanguage(language);
  return true;
}

void SettingsPage::changeEvent(QEvent *event)
{
  QWidget::changeEvent(event);
  if (event->type() != QEvent::LanguageChange || !layout())
    return;
  // Defer rebuilding until all synchronous LanguageChange delivery is complete.
  QTimer::singleShot(0, this, [this] {
    const auto index = m_tabs->currentIndex();
    m_loading = true;
    clearEditor();
    m_bindings.clear();
    m_serverControls.clear();
    const auto children = findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (auto child : children)
      delete child;
    delete layout();
    m_save = nullptr;
    build();
    load();
    m_tabs->setCurrentIndex(index);
  });
}

void SettingsPage::clearEditor()
{
  if (m_triggerEditor)
    m_triggerEditor->hide();
  if (!m_editor)
    return;
  m_editor->hide();
  m_editor->deleteLater();
  m_editor = nullptr;
}

void SettingsPage::buildHotkeys(QVBoxLayout *layout)
{
  auto content = new QFrame(this);
  content->setProperty("settingsGroup", true);
  auto rows = new QVBoxLayout(content);
  rows->setContentsMargins(19, 15, 19, 15);
  m_serverControls.append(content);
  layout->addWidget(content);
  auto note = new QLabel(
      tr("Rules are managed by the sharing computer. External configuration files must be edited separately."), content
  );
  note->setProperty("role", "muted");
  note->setWordWrap(true);
  rows->addWidget(note);
  m_hotkeyEmpty =
      new QLabel(tr("No hotkeys configured. Add a rule for frequently used cross-screen actions."), content);
  m_hotkeyEmpty->setProperty("role", "muted");
  m_hotkeyEmpty->setWordWrap(true);
  m_hotkeyEmpty->setAlignment(Qt::AlignCenter);
  m_hotkeyEmpty->setMinimumHeight(150);
  rows->addWidget(m_hotkeyEmpty);
  m_hotkeys = new QListWidget(content);
  m_hotkeys->setObjectName("settingsHotkeys");
  m_hotkeys->setFixedHeight(140);
  rows->addWidget(m_hotkeys);
  auto controls = new QHBoxLayout;
  auto add = button(tr("Add rule"), "addHotkeyRule");
  auto edit = button(tr("Update trigger"));
  auto remove = button(tr("Remove rule"));
  controls->addStretch();
  controls->addWidget(add);
  controls->addWidget(edit);
  controls->addWidget(remove);
  rows->addLayout(controls);
  m_triggerEditor = new QWidget(content);
  auto triggerLayout = new QVBoxLayout(m_triggerEditor);
  triggerLayout->setContentsMargins(0, 12, 0, 12);
  auto triggerLabel = new QLabel(tr("Click to record a key combination or mouse button."), content);
  triggerLabel->setWordWrap(true);
  triggerLayout->addWidget(triggerLabel);
  m_sequence = new KeySequenceWidget(m_triggerEditor);
  m_sequence->setObjectName("settingsKeySequence");
  m_sequence->setAccessibleName(tr("Trigger key or mouse button"));
  triggerLayout->addWidget(m_sequence);
  auto triggerButtons = new QHBoxLayout;
  auto confirm = button(tr("Confirm rule"), "confirmHotkeyRule"), cancel = button(tr("Cancel"));
  triggerButtons->addStretch();
  triggerButtons->addWidget(confirm);
  triggerButtons->addWidget(cancel);
  triggerLayout->addLayout(triggerButtons);
  rows->addWidget(m_triggerEditor);
  m_triggerEditor->hide();
  m_ruleActions = new QWidget(content);
  auto actionsLayout = new QVBoxLayout(m_ruleActions);
  actionsLayout->setContentsMargins(0, 12, 0, 0);
  actionsLayout->addWidget(new QLabel(tr("Actions for the selected rule"), content));
  m_actions = new QListWidget(content);
  m_actions->setObjectName("settingsHotkeyActions");
  m_actions->setFixedHeight(110);
  actionsLayout->addWidget(m_actions);
  auto actionControls = new QHBoxLayout;
  auto addAction = button(tr("Add action"), "addHotkeyAction"), editActionButton = button(tr("Edit action")),
       removeAction = button(tr("Remove action"));
  actionControls->addStretch();
  actionControls->addWidget(addAction);
  actionControls->addWidget(editActionButton);
  actionControls->addWidget(removeAction);
  actionsLayout->addLayout(actionControls);
  m_actionEditor = new QWidget(content);
  auto editorLayout = new QVBoxLayout(m_actionEditor);
  editorLayout->setContentsMargins(0, 0, 0, 0);
  actionsLayout->addWidget(m_actionEditor);
  rows->addWidget(m_ruleActions);
  m_ruleActions->hide();
  connect(m_hotkeys, &QListWidget::currentRowChanged, this, [this, edit, remove](int index) {
    clearEditor();
    m_actions->clear();
    const bool valid = index >= 0 && index < m_draft.hotkeys().size();
    edit->setVisible(valid);
    remove->setVisible(valid);
    m_ruleActions->setVisible(valid);
    if (valid)
      for (const auto &a : m_draft.hotkeys()[index].actions())
        m_actions->addItem(a.text());
    updateState();
  });
  connect(m_actions, &QListWidget::currentRowChanged, this, [editActionButton, removeAction](int index) {
    editActionButton->setEnabled(index >= 0);
    removeAction->setEnabled(index >= 0);
  });
  connect(add, &QPushButton::clicked, this, [this] {
    clearEditor();
    m_triggerEditIndex = -1;
    m_sequence->setKeySequence(KeySequence());
    m_triggerEditor->show();
    updateState();
  });
  connect(edit, &QPushButton::clicked, this, [this] {
    const auto index = m_hotkeys->currentRow();
    if (index < 0)
      return;
    clearEditor();
    m_triggerEditIndex = index;
    m_sequence->setKeySequence(m_draft.hotkeys()[index].keySequence());
    m_triggerEditor->show();
    updateState();
  });
  connect(cancel, &QPushButton::clicked, this, [this] {
    m_triggerEditor->hide();
    updateState();
  });
  connect(confirm, &QPushButton::clicked, this, [this] {
    if (!m_sequence->valid()) {
      message(tr("Record a valid trigger first."), true);
      return;
    }
    for (int i = 0; i < m_draft.hotkeys().size(); ++i)
      if (i != m_triggerEditIndex && m_draft.hotkeys()[i].keySequence() == m_sequence->keySequence()) {
        message(tr("This trigger already exists."), true);
        return;
      }
    int index = m_triggerEditIndex;
    if (index < 0) {
      Hotkey h;
      h.setKeySequence(m_sequence->keySequence());
      m_draft.hotkeys().append(h);
      index = m_draft.hotkeys().size() - 1;
    } else
      m_draft.hotkeys()[index].setKeySequence(m_sequence->keySequence());
    m_triggerEditor->hide();
    refreshHotkeys();
    m_hotkeys->setCurrentRow(index);
    updateState();
  });
  connect(remove, &QPushButton::clicked, this, [this] {
    const auto index = m_hotkeys->currentRow();
    if (index < 0)
      return;
    clearEditor();
    m_draft.hotkeys().removeAt(index);
    refreshHotkeys();
    updateState();
  });
  connect(addAction, &QPushButton::clicked, this, [this] { editAction(false); });
  connect(editActionButton, &QPushButton::clicked, this, [this] { editAction(true); });
  connect(removeAction, &QPushButton::clicked, this, [this] {
    const auto h = m_hotkeys->currentRow(), a = m_actions->currentRow();
    if (h < 0 || a < 0)
      return;
    m_draft.hotkeys()[h].removeActionAt(a);
    refreshHotkeys();
    m_hotkeys->setCurrentRow(h);
    updateState();
  });
  edit->hide();
  remove->hide();
  editActionButton->setEnabled(false);
  removeAction->setEnabled(false);
}

void SettingsPage::refreshHotkeys()
{
  m_hotkeys->setVisible(!m_draft.hotkeys().isEmpty());
  m_hotkeyEmpty->setVisible(m_draft.hotkeys().isEmpty());
  m_hotkeys->clear();
  m_actions->clear();
  for (const auto &hotkey : m_draft.hotkeys())
    m_hotkeys->addItem(hotkey.text());
}

void SettingsPage::editAction(bool existing)
{
  const auto h = m_hotkeys->currentRow(), a = m_actions->currentRow();
  if (h < 0 || (existing && a < 0))
    return;
  clearEditor();
  m_editHotkey = m_draft.hotkeys()[h];
  m_editAction = existing ? m_editHotkey.actions()[a] : Action();
  if (!existing)
    m_editAction.setRestartServer(false);
  auto editor = new ActionDialog(m_actionEditor, m_draft, m_editHotkey, m_editAction);
  editor->setWindowFlags(Qt::Widget);
  editor->setMinimumWidth(0);
  editor->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
  m_actionEditor->layout()->addWidget(editor);
  m_editor = editor;
  connect(editor, &QDialog::accepted, this, [this, h, a, existing] {
    if (existing)
      m_draft.hotkeys()[h].actionAt(a) = m_editAction;
    else
      m_draft.hotkeys()[h].addAction(m_editAction);
    clearEditor();
    refreshHotkeys();
    m_hotkeys->setCurrentRow(h);
    updateState();
  });
  connect(editor, &QDialog::rejected, this, [this] {
    clearEditor();
    updateState();
  });
  editor->show();
  updateState();
}

void SettingsPage::buildDeviceEditor(QVBoxLayout *layout)
{
  auto g = group(layout);
  m_devices = new QComboBox(this);
  m_devices->setFixedWidth(158);
  m_devices->setObjectName("settingsDeviceSelector");
  row(g, tr("Per-computer input settings"),
      tr("Modifier keys, corner boundaries and compatibility fixes are saved for the selected computer."), m_devices);
  auto reveal = button(tr("Edit selected computer"));
  g->addWidget(reveal, 0, Qt::AlignLeft);
  m_deviceFields = new QWidget(this);
  m_deviceFields->setObjectName("settingsDeviceFields");
  auto fields = new QVBoxLayout(m_deviceFields);
  fields->setContentsMargins(0, 0, 0, 0);
  g->addWidget(m_deviceFields);
  m_serverControls.append(m_devices);
  m_serverControls.append(m_deviceFields);
  connect(m_devices, &QComboBox::currentIndexChanged, this, &SettingsPage::selectDevice);
  connect(reveal, &QPushButton::clicked, this, [this] { m_deviceFields->setVisible(m_deviceFields->isHidden()); });
  m_serverControls.append(reveal);
}

void SettingsPage::selectDevice(int index)
{
  auto fields = static_cast<QVBoxLayout *>(m_deviceFields->layout());
  while (auto item = fields->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  if (index < 0)
    return;
  const int computerIndex = m_devices->itemData(index).toInt();
  const auto &computer = m_draft.computers()[computerIndex];
  const QStringList modifiers{
      QStringLiteral("Shift"),
      QStringLiteral("Ctrl"),
      QStringLiteral("Alt"),
      QStringLiteral("Meta"),
      QStringLiteral("Super"),
      QStringLiteral("Alt Gr"),
      tr("None")
  };
  for (int m = 0; m < 6; ++m) {
    auto combo = new QComboBox(this);
    combo->addItems(modifiers);
    combo->setFixedWidth(130);
    combo->setCurrentIndex(computer.modifier(m));
    row(fields, modifiers[m], tr("Map this modifier key on the selected computer."), combo);
    connect(combo, &QComboBox::currentIndexChanged, this, [this, computerIndex, m](int value) {
      m_draft.computers()[computerIndex].setModifier(static_cast<KeyboardModifier>(m), value);
      updateState();
    });
  }
  const QStringList corners{
      tr("Top-left corner"), tr("Top-right corner"), tr("Bottom-left corner"), tr("Bottom-right corner")
  };
  for (int c = 0; c < 4; ++c) {
    auto check = new SettingsSwitch(this);
    check->setChecked(computer.switchCorner(c));
    row(fields, corners[c], tr("Prevent switching from this corner."), check);
    connect(check, &QCheckBox::toggled, this, [this, computerIndex, c](bool value) {
      m_draft.computers()[computerIndex].setSwitchCorner(static_cast<ComputerConfig::SwitchCorner>(c), value);
      updateState();
    });
  }
  auto size = new QSpinBox(this);
  size->setRange(0, 9999);
  size->setSuffix(tr(" px"));
  size->setValue(computer.switchCornerSize());
  size->setFixedWidth(125);
  row(fields, tr("Corner boundary size"), {}, size);
  connect(size, &QSpinBox::valueChanged, this, [this, computerIndex](int value) {
    m_draft.computers()[computerIndex].setSwitchCornerSize(value);
    updateState();
  });
  const QStringList fixes{
      tr("Caps Lock compatibility"), tr("Num Lock compatibility"), tr("Scroll Lock compatibility"),
      tr("XTest / Xinerama compatibility")
  };
  for (int f = 0; f < 4; ++f) {
    auto check = new SettingsSwitch(this);
    check->setChecked(computer.fix(static_cast<ComputerConfig::Fix>(f)));
    row(fields, fixes[f], {}, check);
    connect(check, &QCheckBox::toggled, this, [this, computerIndex, f](bool value) {
      m_draft.computers()[computerIndex].setFix(static_cast<ComputerConfig::Fix>(f), value);
      updateState();
    });
  }
  // This editor is intentionally collapsed, like the prototype's per-computer entry.
  m_deviceFields->hide();
}
