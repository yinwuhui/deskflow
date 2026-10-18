/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/MdnsBrowser.h"
#include "net/MdnsPublisher.h"

#include <QTest>

class MdnsLifecycleTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void browserStoppedBeforeInitialization()
  {
    deskflow::mdns::MdnsBrowser browser;
    browser.stop();
    browser.start();
    QVERIFY(browser.wait(3000));
  }

  void publisherStoppedBeforeInitialization()
  {
    deskflow::mdns::MdnsPublisher publisher(QStringLiteral("deskflow-lifecycle-test"), 24800, {});
    publisher.stop();
    publisher.start();
    QVERIFY(publisher.wait(3000));
  }
};

QTEST_GUILESS_MAIN(MdnsLifecycleTests)
#include "MdnsLifecycleTests.moc"
