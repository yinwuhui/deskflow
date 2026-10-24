/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "gui/StyleUtils.h"
#include "gui/widgets/HomeIcons.h"

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QtTest>

using namespace deskflow::gui;

class IconTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void everyHomeIconHasVisibleTintedPixels()
  {
    const QColor tint("#087fff");
    for (int value = 0; value <= static_cast<int>(HomeIcon::Close); ++value) {
      const auto icon = homeIcon(static_cast<HomeIcon>(value), tint);
      QVERIFY2(!icon.isNull(), qPrintable(QString::number(value)));
      for (const qreal scale : {1.0, 2.0}) {
        const auto pixmap = icon.pixmap(QSize(24, 24), scale);
        QCOMPARE(pixmap.devicePixelRatio(), scale);
        const auto image = pixmap.toImage();
        bool visible = false;
        for (int y = 0; y < image.height(); ++y) {
          for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixelColor(x, y);
            if (pixel.alpha() > 240) {
              visible = true;
              QVERIFY(qAbs(pixel.red() - tint.red()) <= 1);
              QVERIFY(qAbs(pixel.green() - tint.green()) <= 1);
              QVERIFY(qAbs(pixel.blue() - tint.blue()) <= 1);
            }
          }
        }
        QVERIFY(visible);
      }
    }
  }

  void bundledThemesCoverDialogsAndActions()
  {
    updateIconTheme();
    for (const auto &theme : {QStringLiteral("deskflow-light"), QStringLiteral("deskflow-dark")}) {
      QIcon::setThemeName(theme);
      const QDir directory(":/icons/" + theme + "/actions/32");
      QVERIFY(directory.entryList({"*.png"}, QDir::Files).size() >= 40);
      for (const auto &file : directory.entryList({"*.png"}, QDir::Files)) {
        const auto name = QFileInfo(file).completeBaseName();
        QVERIFY2(QIcon::hasThemeIcon(name), qPrintable(theme + "/" + name));
        QVERIFY2(!QIcon::fromTheme(name).pixmap(24, 24).isNull(), qPrintable(name));
      }
      for (const auto icon :
           {QIcon::ThemeIcon::GoPrevious, QIcon::ThemeIcon::GoHome, QIcon::ThemeIcon::WindowClose,
            QIcon::ThemeIcon::ListAdd, QIcon::ThemeIcon::ListRemove})
        QVERIFY(!QIcon::fromTheme(icon).pixmap(24, 24).isNull());
    }
    updateIconTheme();
  }

  void bluetoothStatesAreDistinct()
  {
    const auto idle = homeIcon(HomeIcon::Bluetooth).pixmap(24, 24).toImage();
    const auto connected = homeIcon(HomeIcon::BluetoothConnected).pixmap(24, 24).toImage();
    const auto disabled = homeIcon(HomeIcon::BluetoothDisabled).pixmap(24, 24).toImage();
    QVERIFY(idle != connected);
    QVERIFY(idle != disabled);
    QVERIFY(connected != disabled);
  }
};

QTEST_MAIN(IconTests)
#include "IconTests.moc"
