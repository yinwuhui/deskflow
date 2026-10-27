// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "DeviceScreenInfo.h"
#include "HomeIcons.h"
#include <QAction>
#include <QButtonGroup>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#ifdef DESKFLOW_DISPLAY
#include "display/ScreenInventoryService.h"
#endif
using deskflow::gui::HomeIcon;
using deskflow::gui::homeIcon;
namespace {
QLabel *textLabel(const QString &text, QWidget *parent, const QString &name = {})
{
  auto label = new QLabel(text, parent);
  label->setObjectName(name);
  label->setTextFormat(Qt::PlainText);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  return label;
}
QString number(double value)
{
  return QString::number(value, 'f', qFuzzyCompare(value, double(qRound(value))) ? 0 : 1);
}
const auto screenStyle = R"(
 QWidget#deviceScreenInfo, QWidget#screenExtensionSettings {background:transparent;}
 QWidget#deviceScreenInfo QLabel, QWidget#screenExtensionSettings QLabel {background:transparent;border:none;color:#8390a1;font-size:12px;}
 QWidget#deviceScreenInfo QLabel[screenHeading="true"] {color:#303640;font-size:13px;font-weight:500;}
 QFrame#screenCard {background:#f6f8fb;border:1px solid #e5eaf1;border-radius:9px;}
 QWidget#deviceScreenInfo QLabel#screenName {color:#344054;font-size:13px;font-weight:500;}
 QWidget#deviceScreenInfo QLabel#screenPrimaryBadge {background:#e8f2ff;color:#087fff;border-radius:4px;padding:2px 6px;font-size:11px;}
 QToolButton#screenDetailsToggle {background:transparent;border:none;color:#087fff;padding:4px;font-size:12px;}
 QWidget#screenExtensionSettings QToolButton#preferredReceiverScreen {background:#f6f8fb;border:1px solid #e3e8ef;border-radius:7px;color:#46576b;padding:7px 12px;font-size:12px;text-align:left;}
 QToolButton#preferredReceiverScreen::menu-indicator {image:none;}
 QWidget#screenExtensionSettings QToolButton#preferredReceiverScreen:disabled {color:#a0a8b4;}
 QFrame#presentationSegments {background:#f3f6fa;border:1px solid #e3e8ef;border-radius:7px;}
 QWidget#screenExtensionSettings QPushButton {background:transparent;border:none;border-radius:5px;color:#758293;padding:5px 12px;font-size:12px;}
 QWidget#screenExtensionSettings QPushButton:checked {background:#087fff;color:white;}
 QWidget#screenExtensionSettings QPushButton:disabled {color:#a0a8b4;}
 QToolButton#streamModeInfo {border:none;background:transparent;color:#8390a1;}
 QToolButton:focus, QPushButton:focus {outline:none;border:1px solid #88bfff;}
)";
} // namespace
DeviceScreenInfo::DeviceScreenInfo(QWidget *parent, deskflow::display::ScreenInventoryService *service)
    : QWidget(parent)
{
  setObjectName("deviceScreenInfo");
  setStyleSheet(screenStyle);
  auto layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);
  auto header = new QHBoxLayout;
  auto heading = textLabel(tr("Screen information"), this);
  heading->setProperty("screenHeading", true);
  header->addWidget(heading, 1);
  m_state = textLabel({}, this, "screenInventoryState");
  m_state->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  header->addWidget(m_state, 1);
  layout->addLayout(header);
  m_cards = new QWidget(this);
  m_cardLayout = new QVBoxLayout(m_cards);
  m_cardLayout->setContentsMargins(0, 0, 0, 0);
  m_cardLayout->setSpacing(8);
  layout->addWidget(m_cards);
  m_preferences = new QWidget(this);
  m_preferences->setObjectName("screenExtensionSettings");
  m_preferences->setStyleSheet(screenStyle);
  auto prefs = new QGridLayout(m_preferences);
  prefs->setContentsMargins(0, 0, 0, 0);
  prefs->setHorizontalSpacing(16);
  prefs->setVerticalSpacing(8);
  prefs->setColumnStretch(1, 1);
  prefs->addWidget(textLabel(tr("Receiving screen"), m_preferences), 0, 0);
  m_target = new QToolButton(m_preferences);
  m_target->setObjectName("preferredReceiverScreen");
  m_target->setAccessibleName(tr("Screen to use for extended display"));
  m_target->setPopupMode(QToolButton::InstantPopup);
  m_target->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  m_target->setIcon(homeIcon(HomeIcon::ChevronDown));
  m_target->setLayoutDirection(Qt::RightToLeft);
  m_target->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  m_target->setFixedHeight(34);
  auto menu = new QMenu(m_target);
  menu->setStyleSheet(
      "QMenu {background:white;border:1px solid #e3e8ef;border-radius:8px;padding:5px;color:#46576b;font-size:12px;} "
      "QMenu::item {padding:8px 20px;border-radius:5px;} QMenu::item:selected {background:#edf5ff;color:#087fff;}"
  );
  m_target->setMenu(menu);
  prefs->addWidget(m_target, 0, 1);
  prefs->addWidget(textLabel(tr("Display style"), m_preferences), 1, 0);
  auto segments = new QFrame(m_preferences);
  segments->setObjectName("presentationSegments");
  segments->setMaximumWidth(184);
  auto segmentLayout = new QHBoxLayout(segments);
  segmentLayout->setContentsMargins(3, 3, 3, 3);
  segmentLayout->setSpacing(0);
  m_windowed = new QPushButton(tr("Window"), segments);
  m_windowed->setObjectName("preferredDisplayWindow");
  m_fullscreen = new QPushButton(tr("Full screen"), segments);
  m_fullscreen->setObjectName("preferredDisplayFullscreen");
  auto group = new QButtonGroup(this);
  for (auto button : {m_windowed, m_fullscreen}) {
    button->setCheckable(true);
    button->setFixedHeight(26);
    group->addButton(button);
    segmentLayout->addWidget(button, 1);
  }
  prefs->addWidget(segments, 1, 1);
  prefs->addWidget(textLabel(tr("Picture quality"), m_preferences), 2, 0);
  auto modeRow = new QHBoxLayout;
  m_mode = textLabel(tr("1920 × 1080 · 30 fps"), m_preferences, "screenSupportedMode");
  modeRow->addWidget(m_mode);
  auto info = new QToolButton(m_preferences);
  info->setObjectName("streamModeInfo");
  info->setAccessibleName(tr("About picture quality"));
  info->setIcon(homeIcon(HomeIcon::Help));
  info->setToolTip(
      tr("Current support: 1920 × 1080, 30 fps, H.264 SDR. Display specifications do not change the streaming limit.")
  );
  modeRow->addWidget(info);
  modeRow->addStretch();
  prefs->addLayout(modeRow, 2, 1);
  m_result = textLabel({}, m_preferences, "screenPreferenceResult");
  m_result->setWordWrap(true);
  prefs->addWidget(m_result, 3, 0, 1, 2);
  m_result->hide();
  m_resultTimer = new QTimer(this);
  m_resultTimer->setSingleShot(true);
  m_resultTimer->setInterval(4000);
  connect(m_resultTimer, &QTimer::timeout, m_result, &QWidget::hide);
  layout->addWidget(m_preferences);
  m_preferences->hide();
#ifdef DESKFLOW_DISPLAY
  m_service = service ? service : deskflow::display::ScreenInventoryService::applicationService();
  connect(m_service, &deskflow::display::ScreenInventoryService::changed, this, &DeviceScreenInfo::refresh);
#else
  Q_UNUSED(service)
#endif
  connect(menu, &QMenu::triggered, this, [this](QAction *action) {
    m_target->setProperty("selectedScreenId", action->data());
    save();
  });
  for (auto button : {m_windowed, m_fullscreen})
    connect(button, &QPushButton::clicked, this, [this] { save(); });
  refresh();
}
void DeviceScreenInfo::setDevice(const QString &device, bool local)
{
  m_device = device;
  m_local = local;
  m_result->hide();
  refresh();
}
void DeviceScreenInfo::setExtensionSelected(bool selected)
{
  m_extensionSelected = selected;
  refresh();
}
QString DeviceScreenInfo::screenName(const QJsonObject &screen, int index) const
{
  const auto name = screen.value("name").toString();
  if (name.startsWith("eDP", Qt::CaseInsensitive) || name.startsWith("LVDS", Qt::CaseInsensitive) ||
      name.startsWith("DSI", Qt::CaseInsensitive))
    return tr("Built-in display");
  return name.isEmpty() ? tr("Display %1").arg(index + 1) : name;
}
void DeviceScreenInfo::rebuildScreens(const QJsonArray &screens)
{
  if (m_screenData == screens)
    return;
  m_screenData = screens;
  while (auto item = m_cardLayout->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  for (int i = 0; i < screens.size(); ++i) {
    const auto s = screens[i].toObject();
    auto card = new QFrame(m_cards);
    card->setObjectName("screenCard");
    auto box = new QVBoxLayout(card);
    box->setContentsMargins(12, 10, 12, 10);
    box->setSpacing(8);
    auto row = new QHBoxLayout;
    row->setSpacing(10);
    auto icon = new QLabel(card);
    icon->setPixmap(homeIcon(HomeIcon::Monitor, QColor("#087fff")).pixmap(24, 24));
    icon->setFixedSize(32, 36);
    row->addWidget(icon);
    auto texts = new QVBoxLayout;
    texts->setSpacing(3);
    auto title = textLabel(screenName(s, i), card, "screenName");
    title->setToolTip(s.value("name").toString());
    texts->addWidget(title);
    QString summary = QStringLiteral("%1 × %2").arg(s.value("pixelWidth").toInt()).arg(s.value("pixelHeight").toInt());
    if (!s.value("refreshRate").isNull())
      summary += tr(" · %1 Hz").arg(number(s.value("refreshRate").toDouble()));
    if (s.value("pixelSizeSource") == "qtGeometryTimesDpr")
      summary += tr(" · Estimated");
    auto spec = textLabel(summary, card, i == 0 ? "screenInventoryDetails" : "screenSummary");
    spec->setWordWrap(true);
    texts->addWidget(spec);
    row->addLayout(texts, 1);
    if (s.value("primary").toBool()) {
      auto badge = textLabel(tr("Primary"), card, "screenPrimaryBadge");
      badge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
      row->addWidget(badge);
    }
    auto toggle = new QToolButton(card);
    toggle->setObjectName("screenDetailsToggle");
    toggle->setText(tr("Details"));
    toggle->setCheckable(true);
    toggle->setIcon(homeIcon(HomeIcon::ChevronDown));
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    row->addWidget(toggle);
    box->addLayout(row);
    auto details = new QWidget(card);
    details->setObjectName("screenTechnicalDetails");
    auto grid = new QGridLayout(details);
    grid->setContentsMargins(42, 4, 0, 2);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(1, 1);
    int n = 0;
    auto fact = [&](const QString &key, const QString &value) {
      grid->addWidget(textLabel(key, details), n, 0);
      auto label = textLabel(value, details);
      label->setWordWrap(true);
      label->setTextInteractionFlags(Qt::TextSelectableByMouse);
      grid->addWidget(label, n++, 1);
    };
    fact(tr("System name"), s.value("name").toString());
    fact(tr("Scaling"), number(s.value("scale").toDouble() * 100) + "%");
    fact(
        tr("Logical size"),
        QStringLiteral("%1 × %2").arg(s.value("logicalWidth").toInt()).arg(s.value("logicalHeight").toInt())
    );
    fact(tr("Orientation"), s.value("orientation") == "portrait" ? tr("Portrait") : tr("Landscape"));
    if (!s.value("physicalWidthMm").isNull() && !s.value("physicalHeightMm").isNull()) {
      fact(
          tr("Physical size"), tr("%1 × %2 mm")
                                   .arg(qRound(s.value("physicalWidthMm").toDouble()))
                                   .arg(qRound(s.value("physicalHeightMm").toDouble()))
      );
      details->setToolTip(tr("Physical size is reported by the system and has not been calibrated."));
    }
    box->addWidget(details);
    details->hide();
    connect(toggle, &QToolButton::toggled, details, &QWidget::setVisible);
    connect(toggle, &QToolButton::toggled, toggle, [toggle](bool expanded) {
      toggle->setIcon(homeIcon(expanded ? HomeIcon::ChevronRight : HomeIcon::ChevronDown));
    });
    m_cardLayout->addWidget(card);
  }
}
void DeviceScreenInfo::refresh()
{
#ifdef DESKFLOW_DISPLAY
  const auto info = m_local ? QJsonObject{{"inventory", m_service->localInventory()}, {"fresh", true}}
                            : m_service->forDevice(m_device);
  const auto inventory = info.value("inventory").toObject();
  const auto screens = inventory.value("screens").toArray();
  const bool known = !inventory.isEmpty(), fresh = info.value("fresh").toBool();
  m_state->setWordWrap(!known);
  if (!known)
    m_state->setText(tr("Connect to view"));
  else if (fresh)
    m_state->setText(tr("%n screen(s)", nullptr, screens.size()));
  else
    m_state->setText(tr("Last connection"));
  const auto time = QDateTime::fromString(info.value("lastSeenAt").toString(), Qt::ISODateWithMs).toLocalTime();
  m_state->setToolTip(
      !fresh && known
          ? tr("Cached information · %1").arg(time.isValid() ? time.toString("yyyy-MM-dd HH:mm") : tr("Unknown time"))
          : QString()
  );
  rebuildScreens(screens);
  m_cards->setVisible(!screens.isEmpty());
  m_preferences->setVisible(m_extensionSelected && !m_local && known);
  QSignalBlocker targetBlock(m_target), windowBlock(m_windowed), fullscreenBlock(m_fullscreen);
  const auto pref = info.value("preference").toObject();
  const auto selected = pref.value("screenId").toString();
  QStringList names{tr("Automatic (primary screen)")}, ids{QString()};
  for (int i = 0; i < screens.size(); ++i) {
    const auto s = screens[i].toObject();
    names << screenName(s, i);
    ids << s.value("screenId").toString();
  }
  auto index = ids.indexOf(selected);
  if (index < 0) {
    names << tr("Previously selected screen is unavailable");
    ids << selected;
    index = ids.size() - 1;
  }
  const auto actions = m_target->menu()->actions();
  bool changed = actions.size() != ids.size();
  for (int i = 0; !changed && i < ids.size(); ++i)
    changed = actions[i]->data().toString() != ids[i] || actions[i]->text() != names[i];
  // Preserve action objects during save and defer rebuilding a visible native menu.
  if (changed && m_target->menu()->isVisible()) {
    m_target->menu()->hide();
    QTimer::singleShot(0, this, &DeviceScreenInfo::refresh);
    return;
  }
  if (changed) {
    m_target->menu()->clear();
    for (int i = 0; i < ids.size(); ++i) {
      auto action = m_target->menu()->addAction(names[i]);
      action->setData(ids[i]);
      action->setCheckable(true);
    }
  }
  for (int i = 0; i < ids.size(); ++i)
    m_target->menu()->actions()[i]->setChecked(i == index);
  m_target->setText(names[index]);
  m_target->setToolTip(names[index]);
  m_target->setProperty("selectedScreenId", ids[index]);
  const bool fullscreen = pref.value("presentationMode") == "fullscreen";
  m_fullscreen->setChecked(fullscreen);
  m_windowed->setChecked(!fullscreen);
  for (auto control :
       {static_cast<QWidget *>(m_target), static_cast<QWidget *>(m_windowed), static_cast<QWidget *>(m_fullscreen)})
    control->setEnabled(fresh && !screens.isEmpty());
#else
  m_state->setText(tr("Unavailable in this build"));
  m_preferences->hide();
#endif
}
void DeviceScreenInfo::save()
{
#ifdef DESKFLOW_DISPLAY
  const auto info = m_service->forDevice(m_device);
  const bool saved = m_service->setPreference(
      info.value("peerId").toString(), m_target->property("selectedScreenId").toString(),
      m_fullscreen->isChecked() ? "fullscreen" : "window"
  );
  m_result->setText(
      saved ? tr("Applies to the next extension") : tr("Screen information changed. Select a screen again.")
  );
  m_result->show();
  if (saved)
    m_resultTimer->start();
  else
    m_resultTimer->stop();
#endif
}
