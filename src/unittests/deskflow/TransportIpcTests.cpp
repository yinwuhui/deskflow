// SPDX-License-Identifier: MIT
#include "base/Log.h"
#include "common/VersionInfo.h"
#include "deskflow/ipc/IpcServer.h"
#include <QLocalSocket>
#include <QSignalSpy>
#include <QUuid>
#include <QtTest>
class TestTransportServer : public deskflow::core::ipc::IpcServer
{
public:
  explicit TestTransportServer(const QString &name) : IpcServer(nullptr, name, "core")
  {
  }

private:
  void processCommand(QLocalSocket *, const QString &, const QStringList &) override
  {
  }
};
class TransportIpcTests : public QObject
{
  Q_OBJECT
  Log log;
private Q_SLOTS:
  void partialFramesAndAuthentication()
  {
    const auto name = "/tmp/bm-ipc-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    TestTransportServer server(name);
    server.listen();
    QSignalSpy commands(&server, &TestTransportServer::transportCommand);
    QLocalSocket socket;
    socket.connectToServer(name);
    QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
    socket.write("transport={\"kind\":\"stopPeer\"}\n");
    socket.flush();
    QTest::qWait(20);
    QCOMPARE(commands.count(), 0);
    const auto hello = QString("hello=%1+%2\n").arg(kVersion, kVersionGitSha).toUtf8();
    socket.write(hello.left(5));
    socket.flush();
    QTest::qWait(20);
    QCOMPARE(commands.count(), 0);
    socket.write(hello.mid(5));
    socket.flush();
    QTRY_COMPARE(commands.count(), 1); // authenticated snapshot request
    socket.write("transport={\"kind\":\"policy\",\"enabled\":");
    socket.flush();
    QTest::qWait(20);
    QCOMPARE(commands.count(), 1);
    socket.write("false}\ntransport={\"kind\":\"snapshot\"}\n");
    socket.flush();
    QTRY_COMPARE(commands.count(), 3);
    QVERIFY(commands[1][0].toString().contains("false"));
  }
  void rejectsOversizedPayload()
  {
    const auto name = "/tmp/bm-ipc-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    TestTransportServer server(name);
    server.listen();
    QLocalSocket socket;
    socket.connectToServer(name);
    QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
    socket.write(QByteArray(65537, 'x'));
    socket.flush();
    QTRY_COMPARE(socket.state(), QLocalSocket::UnconnectedState);
  }
};
QTEST_GUILESS_MAIN(TransportIpcTests)
#include "TransportIpcTests.moc"
