// SPDX-License-Identifier: MIT
#include "BluetoothDiscovery.h"
#include "BluetoothPairing.h"
#include "RfcommBridge.h"
#include "RfcommSession.h"
#include <QSignalSpy>
#include <QTcpServer>
#include <QtEndian>
#include <QtTest>
#include <memory>

class RfcommSessionTests : public QObject
{
  Q_OBJECT
  QTcpServer listener;
  QTcpSocket *peer = nullptr;
  RfcommSession *receiver = nullptr;

  void openReceiver()
  {
    QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
    peer = new QTcpSocket(this);
    peer->connectToHost(QHostAddress::LocalHost, listener.serverPort());
    QTRY_VERIFY(listener.hasPendingConnections());
    receiver = new RfcommSession(listener.nextPendingConnection(), false, false, 1);
    receiver->setParent(this);
    receiver->start();
  }

private Q_SLOTS:
  void cleanup()
  {
    delete receiver;
    receiver = nullptr;
    delete peer;
    peer = nullptr;
    listener.close();
  }

  void invalidBridgeFailsBeforeTouchingAdapter()
  {
    RfcommBridge bridge(true, false, QBluetoothAddress(), 24800, 0);
    QSignalSpy errors(&bridge, &RfcommBridge::fatalError);
    QSignalSpy ready(&bridge, &RfcommBridge::started);
    bridge.start();
    QCOMPARE(errors.count(), 1);
    QCOMPARE(ready.count(), 0);
    bridge.stop();
  }

  void invalidServiceDiscoveryCompletesOnce()
  {
    BluetoothDiscovery discovery;
    QSignalSpy errors(&discovery, &BluetoothDiscovery::error);
    QSignalSpy done(&discovery, &BluetoothDiscovery::finished);
    discovery.discoverServices(QBluetoothAddress());
    QCOMPARE(errors.count(), 1);
    QCOMPARE(done.count(), 1);
    discovery.stop();
    QCOMPARE(done.count(), 1);
  }

#ifdef Q_OS_MACOS
  void invalidPairingDoesNotStartNativeRequest()
  {
    BluetoothPairing pairing;
    QSignalSpy done(&pairing, &BluetoothPairing::finished);
    pairing.pair(QBluetoothAddress());
    QCOMPARE(done.count(), 1);
    QCOMPARE(done.first().first().toBool(), false);
    pairing.cancel();
    pairing.confirm(true);
    QCOMPARE(done.count(), 1);
  }
#endif

  void fragmentedHelloAndCoalescedPing()
  {
    openReceiver();
    const auto hello = RfcommSession::frame(RfcommSession::Hello, "DFBT1 probe");
    for (int i = 0; i < hello.size() - 1; ++i) {
      peer->write(hello.mid(i, 1));
      QTest::qWait(1);
      QCOMPARE(peer->bytesAvailable(), 0);
    }
    peer->write(hello.right(1) + RfcommSession::frame(RfcommSession::Ping, "42"));
    const auto expected =
        RfcommSession::frame(RfcommSession::Ready, {}) + RfcommSession::frame(RfcommSession::Pong, "42");
    QTRY_COMPARE(peer->bytesAvailable(), expected.size());
    QCOMPARE(peer->readAll(), expected);
  }

  void malformedFrames_data()
  {
    QTest::addColumn<QByteArray>("packet");
    QTest::newRow("data-before-hello") << RfcommSession::frame(RfcommSession::Data, "bad");
    QTest::newRow("wrong-version") << RfcommSession::frame(RfcommSession::Hello, "DFBT2 probe");
    auto oversized = RfcommSession::frame(RfcommSession::Hello, {});
    qToBigEndian<quint32>(RfcommSession::MaxPayload + 1, oversized.data() + 1);
    QTest::newRow("oversized-header") << oversized;
    QTest::newRow("repeated-hello") << RfcommSession::frame(RfcommSession::Hello, "DFBT1 probe") +
                                           RfcommSession::frame(RfcommSession::Hello, "DFBT1 probe");
    QTest::newRow("probe-data") << RfcommSession::frame(RfcommSession::Hello, "DFBT1 probe") +
                                       RfcommSession::frame(RfcommSession::Data, "not-allowed");
    QTest::newRow("unknown-type") << RfcommSession::frame(RfcommSession::Hello, "DFBT1 probe") +
                                         RfcommSession::frame(RfcommSession::Type(255), {});
  }

  void malformedFrames()
  {
    QFETCH(QByteArray, packet);
    openReceiver();
    QSignalSpy stopped(receiver, &RfcommSession::finished);
    peer->write(packet);
    QTRY_COMPARE(stopped.count(), 1);
    QTRY_COMPARE(peer->state(), QAbstractSocket::UnconnectedState);
  }

  void probeRttAndDisconnect()
  {
    openReceiver();
    // Transfer ownership from the fixture to a real initiator session.
    auto *wire = peer;
    peer = nullptr;
    RfcommSession sender(wire, true, true, 1);
    QSignalSpy samples(&sender, &RfcommSession::sample);
    QSignalSpy stopped(receiver, &RfcommSession::finished);
    sender.start();
    QTRY_VERIFY_WITH_TIMEOUT(samples.count() >= 2, 4000);
    QVERIFY(samples.first().first().toDouble() >= 0.0);
    sender.stop("test_drop");
    QTRY_COMPARE(stopped.count(), 1);
  }

  void silentPeerTimesOut()
  {
    openReceiver();
    QSignalSpy stopped(receiver, &RfcommSession::finished);
    QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 12000);
    QTRY_COMPARE(peer->state(), QAbstractSocket::UnconnectedState);
  }

  void bridgeBinaryDataAndFreshConnectionAfterDrop()
  {
    QTcpServer core, entrance, transport;
    QVERIFY(core.listen(QHostAddress::LocalHost, 0));
    QVERIFY(entrance.listen(QHostAddress::LocalHost, 0));
    QVERIFY(transport.listen(QHostAddress::LocalHost, 0));
    // Two distinct sessions verify that a disconnected stream is never resumed/replayed.
    for (int attempt = 0; attempt < 2; ++attempt) {
      QTcpSocket client;
      client.connectToHost(QHostAddress::LocalHost, entrance.serverPort());
      QTRY_VERIFY(entrance.hasPendingConnections());
      auto *local = entrance.nextPendingConnection();
      auto *wire = new QTcpSocket;
      wire->connectToHost(QHostAddress::LocalHost, transport.serverPort());
      QTRY_VERIFY(transport.hasPendingConnections());
      RfcommSession server(transport.nextPendingConnection(), false, false, core.serverPort());
      RfcommSession sender(wire, true, false, 1, local);
      QSignalSpy ready(&sender, &RfcommSession::ready);
      QSignalSpy stopped(&server, &RfcommSession::finished);
      server.start();
      // Queue local input before RFCOMM readiness, as the Deskflow TLS handshake does.
      QByteArray payload(200000, '\0');
      for (int i = 0; i < payload.size(); ++i)
        payload[i] = char((i + attempt) % 256);
      client.write(payload);
      sender.start();
      QTRY_VERIFY(core.hasPendingConnections());
      std::unique_ptr<QTcpSocket> coreSocket(core.nextPendingConnection());
      QTRY_COMPARE(ready.count(), 1);
      QTRY_COMPARE(coreSocket->bytesAvailable(), payload.size());
      QCOMPARE(coreSocket->readAll(), payload);
      coreSocket->write(payload);
      QTRY_COMPARE(client.bytesAvailable(), payload.size());
      QCOMPARE(client.readAll(), payload);
      sender.stop("test_bluetooth_loss");
      QTRY_COMPARE(stopped.count(), 1);
      QTRY_COMPARE(coreSocket->state(), QAbstractSocket::UnconnectedState);
      QTRY_COMPARE(client.state(), QAbstractSocket::UnconnectedState);
    }
  }
};

QTEST_GUILESS_MAIN(RfcommSessionTests)
#include "tests.moc"
