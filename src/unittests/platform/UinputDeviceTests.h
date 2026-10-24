/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#pragma once
#include <QTest>

class UinputDeviceTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void releasesHeldKeysAndButtons();
  void mapsAbsoluteCoordinatesAndClamps();
  void reentryRefreshesCoordinates();
  void preservesFractionalWheelMotion();
  void relativeMotionUsesSeparateDevice();
};
