// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "display/ScreenInventoryService.h"
#include "gui/widgets/DeviceScreenInfo.h"
#include <QAction>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using deskflow::display::ScreenInventoryService;
class DeviceScreenInfoTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void informationBeforeStreamingAndOfflineCache()
  {
    QTemporaryDir dir;
    ScreenInventoryService service(nullptr, dir.filePath("cache.ini"), false);
    DeviceScreenInfo widget(nullptr, &service);
    widget.setDevice("computer:remote");
    QVERIFY(widget.extensionSettings()->isHidden());
    widget.setExtensionSelected(true);
    QVERIFY(widget.findChild<QLabel *>("screenInventoryState")->text().contains("Connect"));
    const QString peer(64, 'a'), connection(32, 'b');
    service.observeConnection(
        {{"peerId", peer},
         {"baseConnectionId", connection},
         {"baseConnected", true},
         {"deviceKeys", QJsonArray{"computer:remote"}}}
    );
    QJsonObject s{
        {"screenId", QString(64, 'c')},
        {"name", "<b>Display</b>"},
        {"logicalWidth", 1920},
        {"logicalHeight", 1080},
        {"x", 0},
        {"y", 0},
        {"pixelWidth", 3840},
        {"pixelHeight", 2160},
        {"pixelSizeSource", "qtGeometryTimesDpr"},
        {"scale", 2.0},
        {"refreshRate", 60.0},
        {"physicalWidthMm", QJsonValue(QJsonValue::Null)},
        {"physicalHeightMm", QJsonValue(QJsonValue::Null)},
        {"physicalSizeSource", "systemReportedUnverified"},
        {"primary", true},
        {"orientation", "landscape"}
    };
    QJsonObject inventory{
        {"schemaVersion", 1}, {"instanceId", QString(32, 'd')}, {"revision", "1"}, {"screens", QJsonArray{s}}
    };
    service.receiveControl(
        {{"peerId", peer},
         {"baseConnectionId", connection},
         {"body", QJsonObject{{"protocol", "device-screens/1"}, {"type", "inventory"}, {"inventory", inventory}}}}
    );
    auto text = widget.findChild<QLabel *>("screenInventoryDetails");
    QVERIFY(text->text().contains("3840"));
    QCOMPARE(text->textFormat(), Qt::PlainText);
    QVERIFY(widget.findChild<QLabel *>("screenSupportedMode")->text().contains("1920"));
    auto targets = widget.findChild<QToolButton *>("preferredReceiverScreen");
    QVERIFY(targets->isEnabled());
    QCOMPARE(targets->menu()->actions().count(), 2);
    auto detail = widget.findChild<QWidget *>("screenTechnicalDetails");
    QVERIFY(detail->isHidden());
    QVERIFY(!widget.extensionSettings()->isHidden());
    auto toggle = widget.findChild<QToolButton *>("screenDetailsToggle");
    toggle->click();
    QVERIFY(!detail->isHidden());
    QVERIFY(!text->text().contains("Logical"));
    widget.setExtensionSelected(false);
    QVERIFY(widget.extensionSettings()->isHidden());
    widget.setExtensionSelected(true);
    QVERIFY(!widget.extensionSettings()->isHidden());
    const auto originalActions = targets->menu()->actions();
    targets->menu()->actions().at(1)->trigger();
    QCOMPARE(
        service.forDevice("computer:remote").value("preference").toObject().value("screenId").toString(),
        QString(64, 'c')
    );
    widget.findChild<QPushButton *>("preferredDisplayFullscreen")->click();
    QCOMPARE(
        service.forDevice("computer:remote").value("preference").toObject().value("presentationMode").toString(),
        QString("fullscreen")
    );
    QVERIFY(!widget.findChild<QLabel *>("screenPreferenceResult")->isHidden());
    QCOMPARE(targets->menu()->actions(), originalActions);
    QVERIFY(service.setPreference(peer, QString(64, 'c'), "fullscreen"));
    QVERIFY(widget.findChild<QPushButton *>("preferredDisplayFullscreen")->isChecked());
    service.observeConnection({{"peerId", peer}, {"baseConnectionId", connection}, {"baseConnected", false}});
    QVERIFY(widget.findChild<QLabel *>("screenInventoryState")->text().contains("Last connection"));
    QVERIFY(!targets->isEnabled());
    QVERIFY(text->text().contains("3840"));
  }
};
QTEST_MAIN(DeviceScreenInfoTests)
#include "DeviceScreenInfoTests.moc"
