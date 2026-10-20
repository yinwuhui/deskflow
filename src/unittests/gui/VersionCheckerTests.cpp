// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "common/VersionInfo.h"
#include "gui/VersionChecker.h"
#include <QNetworkReply>
#include <QtTest>
class VersionReply : public QNetworkReply
{
public:
  explicit VersionReply(QByteArray payload, bool failure = false) : m_payload(std::move(payload))
  {
    open(QIODevice::ReadOnly);
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, failure ? 503 : 200);
    if (failure)
      setError(QNetworkReply::ServiceUnavailableError, "unavailable");
  }
  void abort() override
  {
  }

protected:
  qint64 readData(char *data, qint64 maximum) override
  {
    const auto length = std::min(maximum, qint64(m_payload.size()) - m_position);
    if (length <= 0)
      return -1;
    memcpy(data, m_payload.constData() + m_position, length);
    m_position += length;
    return length;
  }

private:
  QByteArray m_payload;
  qint64 m_position = 0;
};
class VersionCheckerTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void malformedRepliesFinishWithoutCrashing_data()
  {
    QTest::addColumn<QByteArray>("body");
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("missing-parts") << QByteArray("2");
    QTest::newRow("html") << QByteArray("<html>server unavailable</html>");
    QTest::newRow("oversized") << QByteArray(300, '1');
  }
  void malformedRepliesFinishWithoutCrashing()
  {
    QFETCH(QByteArray, body);
    VersionChecker checker;
    QSignalSpy result(&checker, &VersionChecker::checkFinished);
    checker.replyFinished(new VersionReply(body));
    QCOMPARE(result.count(), 1);
    QVERIFY(!result.first().at(0).toBool());
  }
  void equalVersionAndNetworkFailureHaveTerminalStates()
  {
    VersionChecker checker;
    QSignalSpy result(&checker, &VersionChecker::checkFinished);
    checker.replyFinished(new VersionReply(QByteArray(kVersion) + "\n"));
    QCOMPARE(result.count(), 1);
    QVERIFY(result.first().at(0).toBool());
    QVERIFY(result.first().at(1).toString().isEmpty());
    checker.replyFinished(new VersionReply({}, true));
    QCOMPARE(result.count(), 2);
    QVERIFY(!result.last().at(0).toBool());
  }
};
QTEST_MAIN(VersionCheckerTests)
#include "VersionCheckerTests.moc"
