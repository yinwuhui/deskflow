// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "base/Log.h"
#include "deskflow/ipc/IpcServer.h"
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
namespace {
class TestServer : public deskflow::core::ipc::IpcServer
{
public:
  explicit TestServer(const QString &path) : IpcServer(nullptr, path, "test")
  {
  }
  QStringList messages;

private:
  void processCommand(QLocalSocket *, const QString &command, const QStringList &parts) override
  {
    messages.append(command + ":" + parts.value(1));
  }
};
QString socketName(const QTemporaryDir &directory)
{
#ifdef Q_OS_WIN
  return "deskflow-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
#else
  return directory.filePath("ipc");
#endif
}
} // namespace
class IpcServerTests : public QObject
{
  Q_OBJECT
private:
  Log m_log;
private Q_SLOTS:
  void sameUserCanConnectAndPartialMessagesWaitForTheirDelimiter()
  {
    QTemporaryDir directory;
    const auto path = socketName(directory);
    TestServer server(path);
    server.listen();
    QLocalSocket client;
    client.connectToServer(path);
    QVERIFY(client.waitForConnected());
    client.write("ping=partial");
    client.flush();
    QTest::qWait(20);
    QVERIFY(server.messages.isEmpty());
    client.write("\nping=complete\n");
    client.flush();
    QTRY_COMPARE(server.messages.size(), 2);
    QCOMPARE(server.messages, QStringList({"ping:partial", "ping:complete"}));
  }
  void oversizedUnterminatedMessageDisconnectsWithoutDispatching()
  {
    QTemporaryDir directory;
    const auto path = socketName(directory);
    TestServer server(path);
    server.listen();
    QLocalSocket client;
    client.connectToServer(path);
    QVERIFY(client.waitForConnected());
    client.write(QByteArray(1024 * 1024 + 1, 'a'));
    client.flush();
    QTRY_COMPARE(client.state(), QLocalSocket::UnconnectedState);
    QVERIFY(server.messages.isEmpty());
  }
};
QTEST_GUILESS_MAIN(IpcServerTests)
#include "IpcServerTests.moc"
