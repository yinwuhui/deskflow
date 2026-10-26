// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "DeviceConnectionInfo.h"
#include "gui/core/PeerUsageModel.h"
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonValue>
#include <QLabel>
#include <QLocale>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

DeviceConnectionInfo::DeviceConnectionInfo(QWidget *parent, PeerUsageModel *model)
    : QWidget(parent),
      m_model(model ? model : PeerUsageModel::applicationModel())
{
  setObjectName("deviceConnectionInfo");
  setStyleSheet(R"(
    QWidget#deviceConnectionInfo {background:transparent;}
    QWidget#deviceConnectionInfo QLabel {background:transparent;color:#758293;font-size:12px;}
    QWidget#deviceConnectionInfo QLabel[heading="true"] {color:#303640;font-size:13px;font-weight:500;}
    QWidget#deviceConnectionInfo QLabel[value="true"] {color:#46576b;}
    QWidget#deviceConnectionInfo QLabel[status="connected"] {color:#208b69;}
    QWidget#deviceConnectionInfo QLabel[status="pending"] {color:#a2752a;}
    QWidget#deviceConnectionInfo QWidget#connectionChannels {background:#f6f8fb;border:1px solid #e5eaf1;border-radius:9px;}
  )");
  auto layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);
  const auto label = [this](const QString &text, const QString &name) {
    auto value = new QLabel(text, this);
    value->setObjectName(name);
    value->setTextFormat(Qt::PlainText);
    return value;
  };
  auto heading = label(tr("Current usage"), "connectionUsageHeading");
  heading->setProperty("heading", true);
  layout->addWidget(heading);
  auto usages = new QGridLayout;
  usages->setHorizontalSpacing(12);
  usages->setVerticalSpacing(6);
  usages->addWidget(label(tr("Keyboard & mouse"), {}), 0, 0);
  usages->addWidget(label(tr("Extended display"), {}), 1, 0);
  m_input = label({}, "inputTransportUsage");
  m_display = label({}, "displayTransportUsage");
  for (auto value : {m_input, m_display}) {
    value->setProperty("value", true);
    value->setWordWrap(false);
    value->setFixedHeight(20);
    value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  }
  usages->addWidget(m_input, 0, 1);
  usages->addWidget(m_display, 1, 1);
  usages->setColumnStretch(1, 1);
  layout->addLayout(usages);
  auto connections = label(tr("Connections"), "connectionChannelsHeading");
  connections->setProperty("heading", true);
  layout->addWidget(connections);
  auto channels = new QWidget(this);
  channels->setObjectName("connectionChannels");
  auto rows = new QGridLayout(channels);
  rows->setContentsMargins(12, 10, 12, 10);
  rows->setHorizontalSpacing(10);
  rows->setVerticalSpacing(5);
  for (int lane = 0; lane < 2; ++lane) {
    const QString prefix = lane == 0 ? "network" : "bluetooth";
    auto name = label(lane == 0 ? tr("IP network") : tr("Bluetooth"), prefix + "ChannelName");
    name->setProperty("value", true);
    m_states[lane] = label({}, prefix + "ChannelState");
    m_latency[lane] = label({}, prefix + "ChannelLatency");
    m_latency[lane]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_latency[lane]->setFixedWidth(170);
    m_addresses[lane] = label({}, prefix + "ChannelAddress");
    m_addresses[lane]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_rates[lane] = label({}, prefix + "ChannelRates");
    m_rates[lane]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_rates[lane]->setFixedWidth(170);
    rows->addWidget(name, lane * 3, 0);
    rows->addWidget(m_states[lane], lane * 3, 1);
    rows->addWidget(m_latency[lane], lane * 3, 2);
    rows->addWidget(m_addresses[lane], lane * 3 + 1, 0, 1, 2);
    rows->addWidget(m_rates[lane], lane * 3 + 1, 2);
    if (lane == 0) {
      auto line = new QFrame(channels);
      line->setFrameShape(QFrame::HLine);
      line->setStyleSheet("color:#e5eaf1;background:#e5eaf1;max-height:1px;border:none;");
      rows->addWidget(line, 2, 0, 1, 3);
    }
  }
  rows->setColumnStretch(1, 1);
  layout->addWidget(channels);
  connect(m_model, &PeerUsageModel::changed, this, &DeviceConnectionInfo::refresh);
  auto timer = new QTimer(this);
  timer->setInterval(1000);
  connect(timer, &QTimer::timeout, this, &DeviceConnectionInfo::refresh);
  timer->start();
  refresh();
}
void DeviceConnectionInfo::setDevice(const QString &key, const QString &savedAddress)
{
  m_device = key;
  m_savedAddress = savedAddress;
  refresh();
}
QString DeviceConnectionInfo::metric(const QJsonValue &value, bool rate) const
{
  if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0)
    return QString::fromUtf8("—");
  const auto number = value.toDouble();
  if (!rate)
    return tr("%1 ms").arg(QLocale().toString(number, 'f', number < 10 ? 1 : 0));
  if (number < 1024)
    return tr("%1 B/s").arg(QLocale().toString(number, 'f', 0));
  if (number < 1024 * 1024)
    return tr("%1 KB/s").arg(QLocale().toString(number / 1024, 'f', 1));
  return tr("%1 MB/s").arg(QLocale().toString(number / (1024 * 1024), 'f', 1));
}
void DeviceConnectionInfo::refresh()
{
  const bool connected = m_model->connected(m_device);
  const auto usage = m_model->snapshot(m_device);
  const auto transport = m_model->transportForDevice(m_device);
  const auto display = m_model->displayForDevice(m_device);
  const auto effective = usage.value("effectiveMode").toString();
  const bool mediaActive =
      effective == "extended" && display.value("state") == "Streaming" && display.value("firstFramePresented").toBool();
  QString inputLane;
  for (const auto &item : transport.value("channels").toArray()) {
    const auto channel = item.toObject();
    if (channel.value("state") == "active") {
      if (channel.value("type") == "network")
        inputLane = tr("IP network");
      else if (channel.value("type") == "bluetooth")
        inputLane = tr("Bluetooth");
    }
  }
  QString input = tr("Not active"), screen = tr("Not enabled");
  if (effective == "extended") {
    input = tr("Paused");
    // The current media implementation uses authenticated IP transport only.
    // Still require the matching renderer's presentation acknowledgement.
    screen = mediaActive ? tr("IP network · displaying") : tr("Waiting for display confirmation");
  } else if (effective == "input") {
    input = inputLane.isEmpty() ? tr("Channel not confirmed") : tr("%1 · in use").arg(inputLane);
  } else if (connected && usage.isEmpty())
    input = tr("Usage not confirmed");
  if (!connected)
    input = tr("Not connected");
  if (usage.value("usageState") == "Switching" && usage.value("targetMode") == "extended" && effective != "extended")
    screen = tr("Switching · not active yet");
  m_input->setToolTip(input);
  m_display->setToolTip(screen);
  m_input->setText(input);
  m_display->setText(screen);
  for (int lane = 0; lane < 2; ++lane) {
    QJsonObject channel;
    for (const auto &item : transport.value("channels").toArray())
      if (item.toObject().value("type") == (lane == 0 ? "network" : "bluetooth"))
        channel = item.toObject();
    const auto state = channel.value("state").toString();
    QString text = tr("Not confirmed"), style;
    const bool established = state == "active" || state == "ready" || (lane == 0 && mediaActive);
    if (established) {
      text = tr("Connected");
      style = "connected";
    } else if (state == "connecting") {
      text = tr("Connecting…");
      style = "pending";
    } else if (state == "authenticating") {
      text = tr("Authenticating…");
      style = "pending";
    } else if (state == "retrying") {
      text = tr("Reconnecting…");
      style = "pending";
    } else if (state == "unavailable" || !connected)
      text = tr("Not connected");
    m_states[lane]->setText(text);
    m_states[lane]->setProperty("status", style);
    m_states[lane]->setStyleSheet(
        style == "connected" ? "color:#208b69;"
        : style == "pending" ? "color:#a2752a;"
                             : "color:#8390a1;"
    );
    // Metrics are optional. Never substitute configured bandwidth or negotiated
    // video fps for measurements. Stale/missing samples are rendered as dashes.
    const auto measurements = lane == 0 && mediaActive ? display.value("statistics").toObject() : channel;
    bool timestampValid = false;
    const auto timestamp = measurements.value("sampledAtMs").toString().toLongLong(&timestampValid);
    const auto age = QDateTime::currentMSecsSinceEpoch() - timestamp;
    const bool metrics =
        established && measurements.value("metricsFresh").toBool() && timestampValid && age >= 0 && age <= 5000;
    m_latency[lane]->setText(tr("Latency %1").arg(metric(metrics ? measurements.value("rttMs") : QJsonValue{}, false)));
    m_rates[lane]->setText(tr("↑ %1   ↓ %2")
                               .arg(
                                   metric(metrics ? measurements.value("txBytesPerSecond") : QJsonValue{}, true),
                                   metric(metrics ? measurements.value("rxBytesPerSecond") : QJsonValue{}, true)
                               ));
    m_rates[lane]->setToolTip(tr("Application send / receive rate; this is not maximum available bandwidth."));
    auto address = channel.value("remoteAddress").toString();
    if (address.isEmpty() && lane == 0 && !m_savedAddress.isEmpty())
      address = tr("Saved: %1").arg(m_savedAddress);
    if (address.isEmpty())
      address = tr("Address not reported");
    m_addresses[lane]->setText(
        m_addresses[lane]->fontMetrics().elidedText(address, Qt::ElideMiddle, qMax(80, m_addresses[lane]->width()))
    );
    m_addresses[lane]->setToolTip(address);
    m_addresses[lane]->setAccessibleName(address);
    m_states[lane]->setToolTip(text);
  }
}
