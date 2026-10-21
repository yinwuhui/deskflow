// SPDX-License-Identifier: MIT
#include "gui/core/TransportNotifications.h"
#include <QSignalSpy>
#include <QtTest>
class TransportNotificationsTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void fastRecoveryOnlyShowsSuccess()
  {
    TransportNotifications policy(nullptr, 100);
    QSignalSpy notifications(&policy, &TransportNotifications::notification);
    policy.update("network");
    QCOMPARE(notifications.count(), 0);
    policy.update("switching");
    policy.update("bluetooth");
    QTest::qWait(150);
    QCOMPARE(notifications.count(), 1);
    policy.update("bluetooth");
    QCOMPARE(notifications.count(), 1);
  }
  void slowRecoveryAndRetriesDoNotSpam()
  {
    TransportNotifications policy(nullptr, 20);
    QSignalSpy notifications(&policy, &TransportNotifications::notification);
    policy.update("network");
    policy.update("switching");
    QTRY_COMPARE(notifications.count(), 1);
    policy.update("switching");
    policy.update("offline");
    QCOMPARE(notifications.count(), 2);
    for (int i = 0; i < 20; ++i) {
      policy.update("switching");
      policy.update("offline");
    }
    QCOMPARE(notifications.count(), 2);
    policy.update("network");
    QCOMPARE(notifications.count(), 3);
  }
  void stopCancelsPendingNotification()
  {
    TransportNotifications policy(nullptr, 20);
    QSignalSpy notifications(&policy, &TransportNotifications::notification);
    policy.update("network");
    policy.update("switching");
    policy.reset();
    QTest::qWait(50);
    QCOMPARE(notifications.count(), 0);
    policy.update("bluetooth");
    QCOMPARE(notifications.count(), 0);
  }
};
QTEST_GUILESS_MAIN(TransportNotificationsTests)
#include "TransportNotificationsTests.moc"
