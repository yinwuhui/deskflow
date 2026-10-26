// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "gui/core/PeerUsageModel.h"
#include "gui/widgets/DeviceConnectionInfo.h"
#include <QDateTime>
#include <QJsonArray>
#include <QLabel>
#include <QtTest>
class DeviceConnectionInfoTests : public QObject
{
  Q_OBJECT
  const QString peer = QString(64, 'a'), device = "computer:work";
  QJsonObject usage(int revision, const QString &mode)
  {
    return {
        {"peerId", peer},
        {"revision", QString::number(revision)},
        {"effectiveMode", mode},
        {"usageState", mode == "extended" ? "DisplayActive" : "InputActive"}
    };
  }
  QJsonObject transports(int revision, bool bluetoothActive = false)
  {
    return {
        {"version", 2},
        {"revision", revision},
        {"peers",
         QJsonArray{QJsonObject{
             {"peerId", peer},
             {"channels", QJsonArray{
                              QJsonObject{{"type", "network"}, {"state", bluetoothActive ? "ready" : "active"}},
                              QJsonObject{{"type", "bluetooth"}, {"state", bluetoothActive ? "active" : "ready"}}
                          }}
         }}}
    };
  }
  void bind(PeerUsageModel &model)
  {
    model.setConnectedDevices({device});
    model.bindAuthenticatedDevice(device, peer);
    model.receiveSnapshot(usage(1, "input"));
  }
  QString text(DeviceConnectionInfo &widget, const char *name)
  {
    return widget.findChild<QLabel *>(name)->text();
  }
private Q_SLOTS:
  void connectedDoesNotMeanUsedForBothPurposes()
  {
    PeerUsageModel model;
    bind(model);
    DeviceConnectionInfo widget(nullptr, &model);
    widget.setDevice(device);
    model.receiveTransportSnapshot(transports(1));
    QVERIFY(text(widget, "inputTransportUsage").contains("IP network"));
    QCOMPARE(text(widget, "displayTransportUsage"), QString("Not enabled"));
    QCOMPARE(text(widget, "networkChannelState"), QString("Connected"));
    QCOMPARE(text(widget, "bluetoothChannelState"), QString("Connected"));
    model.receiveTransportSnapshot(transports(2, true));
    QVERIFY(text(widget, "inputTransportUsage").contains("Bluetooth"));
    model.receiveTransportSnapshot(transports(1));
    QVERIFY(text(widget, "inputTransportUsage").contains("Bluetooth"));
    QVERIFY(text(widget, "networkChannelLatency").contains(QString::fromUtf8("—")));
  }
  void extensionRequiresConfirmedRendererAndPausesOriginalInput()
  {
    PeerUsageModel model;
    bind(model);
    DeviceConnectionInfo widget(nullptr, &model);
    widget.setDevice(device);
    model.receiveTransportSnapshot(transports(1, true));
    model.receiveSnapshot(usage(2, "extended"));
    QCOMPARE(text(widget, "inputTransportUsage"), QString("Paused"));
    QCOMPARE(text(widget, "displayTransportUsage"), QString("Waiting for display confirmation"));
    model.receiveDisplaySnapshot(
        {{"peerId", QString(64, 'b')}, {"revision", "1"}, {"state", "Streaming"}, {"firstFramePresented", true}}
    );
    QCOMPARE(text(widget, "displayTransportUsage"), QString("Waiting for display confirmation"));
    model.receiveDisplaySnapshot(
        {{"peerId", peer}, {"revision", "2"}, {"state", "Streaming"}, {"firstFramePresented", true}}
    );
    QCOMPARE(text(widget, "displayTransportUsage"), QString("IP network · displaying"));
    // Media is an independent IP connection, not the keyboard transport lane.
    auto root = transports(2, true);
    auto peers = root["peers"].toArray();
    auto p = peers[0].toObject();
    auto lanes = p["channels"].toArray();
    lanes[0] = QJsonObject{{"type", "network"}, {"state", "unavailable"}};
    p["channels"] = lanes;
    peers[0] = p;
    root["peers"] = peers;
    model.receiveTransportSnapshot(root);
    QCOMPARE(text(widget, "networkChannelState"), QString("Connected"));
    QCOMPARE(text(widget, "inputTransportUsage"), QString("Paused"));
    model.setConnectedDevices({});
    QCOMPARE(text(widget, "inputTransportUsage"), QString("Not connected"));
    QCOMPARE(text(widget, "displayTransportUsage"), QString("Not enabled"));
    QCOMPARE(text(widget, "bluetoothChannelState"), QString("Not connected"));
  }
  void missingExpiredAndInvalidMetricsAreNotZeroLatency()
  {
    PeerUsageModel model;
    bind(model);
    DeviceConnectionInfo widget(nullptr, &model);
    widget.setDevice(device);
    auto root = transports(1);
    auto peers = root["peers"].toArray();
    auto value = peers[0].toObject();
    auto channels = value["channels"].toArray();
    auto network = channels[0].toObject();
    network.insert("metricsFresh", true);
    network.insert("sampledAtMs", QString::number(QDateTime::currentMSecsSinceEpoch()));
    network.insert("rttMs", 8);
    network.insert("txBytesPerSecond", 2048);
    network.insert("rxBytesPerSecond", 0);
    channels[0] = network;
    value["channels"] = channels;
    peers[0] = value;
    root["peers"] = peers;
    model.receiveTransportSnapshot(root);
    QVERIFY(text(widget, "networkChannelLatency").contains("8"));
    QVERIFY(text(widget, "networkChannelRates").contains("KB/s"));
    QVERIFY(text(widget, "networkChannelRates").contains("0 B/s"));
    network.insert("sampledAtMs", QString::number(QDateTime::currentMSecsSinceEpoch() - 10000));
    channels[0] = network;
    value["channels"] = channels;
    peers[0] = value;
    root["peers"] = peers;
    root["revision"] = 2;
    model.receiveTransportSnapshot(root);
    QVERIFY(text(widget, "networkChannelLatency").contains(QString::fromUtf8("—")));
    model.clearTransportSnapshot();
    QCOMPARE(text(widget, "inputTransportUsage"), QString("Channel not confirmed"));
  }
};
QTEST_MAIN(DeviceConnectionInfoTests)
#include "DeviceConnectionInfoTests.moc"
