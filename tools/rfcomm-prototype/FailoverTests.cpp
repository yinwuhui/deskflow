// SPDX-License-Identifier: MIT
#include "AutoTransport.h"
#include "FailoverSession.h"
#include <QFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <memory>
#include <openssl/pem.h>

class SynchronousWriteDevice : public QIODevice
{
public:
  SynchronousWriteDevice() { open(QIODevice::ReadWrite); }
  bool writing = false, reentered = false;
  QByteArray output;
  bool isSequential() const override { return true; }
protected:
  qint64 readData(char *, qint64) override { return 0; }
  qint64 writeData(const char *data, qint64 size) override
  {
    if (writing) {
      reentered = true;
      return -1;
    }
    writing = true;
    output.append(data, size);
    Q_EMIT bytesWritten(size);
    writing = false;
    return size;
  }
};

class FailoverTests : public QObject
{
  Q_OBJECT
  QTemporaryDir files;
  QString alice, bob;
  QByteArray alicePin, bobPin;
  std::unique_ptr<FailoverSession> client, server;
  QTcpServer coreServer, entrance;
  std::array<QPointer<FailoverLink>, 2> clientLinks, serverLinks;

  void identity(const QString &name, QString &path, QByteArray &pin)
  {
    auto *context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    QVERIFY(context);
    QVERIFY(EVP_PKEY_keygen_init(context) == 1);
    QVERIFY(EVP_PKEY_CTX_set_rsa_keygen_bits(context, 2048) == 1);
    EVP_PKEY *key = nullptr;
    QVERIFY(EVP_PKEY_keygen(context, &key) == 1);
    EVP_PKEY_CTX_free(context);
    auto *cert = X509_new();
    QVERIFY(cert);
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), -60);
    X509_gmtime_adj(X509_getm_notAfter(cert), 86400);
    X509_set_pubkey(cert, key);
    auto *subject = X509_get_subject_name(cert);
    const auto cn = name.toUtf8();
    X509_NAME_add_entry_by_txt(
        subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>(cn.constData()), -1, -1, 0
    );
    X509_set_issuer_name(cert, subject);
    QVERIFY(X509_sign(cert, key, EVP_sha256()) > 0);
    auto *bio = BIO_new(BIO_s_mem());
    QVERIFY(PEM_write_bio_X509(bio, cert) == 1);
    QVERIFY(PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr) == 1);
    char *data;
    const auto size = BIO_get_mem_data(bio, &data);
    path = files.filePath(name + ".pem");
    QFile pem(path);
    QVERIFY(pem.open(QIODevice::WriteOnly));
    QCOMPARE(pem.write(data, size), size);
    unsigned char hash[32];
    unsigned int length;
    QVERIFY(X509_digest(cert, EVP_sha256(), hash, &length) == 1);
    pin = QByteArray(reinterpret_cast<char *>(hash), length);
    BIO_free(bio);
    X509_free(cert);
    EVP_PKEY_free(key);
  }

  void addLane(int lane, bool badPin = false, QByteArray otherInstance = {})
  {
    QTcpServer transport;
    QVERIFY(transport.listen(QHostAddress::LocalHost, 0));
    auto *out = new QTcpSocket;
    out->connectToHost(QHostAddress::LocalHost, transport.serverPort());
    QTRY_VERIFY(transport.hasPendingConnections());
    auto *in = transport.nextPendingConnection();
    auto *a = new TlsChannel(out, false, alice, badPin ? QByteArray(32, 'x') : bobPin, this);
    auto *b = new TlsChannel(in, true, bob, alicePin, this);
    auto *left =
        new FailoverLink(a, false, otherInstance.isEmpty() ? client->instanceId() : otherInstance, this, 50, 600);
    auto *right = new FailoverLink(b, true, server->instanceId(), this, 50, 600);
    clientLinks[lane] = left;
    serverLinks[lane] = right;
    server->attach(FailoverSession::Lane(lane), right);
    client->attach(FailoverSession::Lane(lane), left);
    if (!badPin && otherInstance.isEmpty()) {
      QTRY_VERIFY(clientLinks[lane] && clientLinks[lane]->ready());
      QTRY_VERIFY(serverLinks[lane] && serverLinks[lane]->ready());
    }
  }

  void connectCore(QTcpSocket &local, QTcpSocket *&remote)
  {
    local.connectToHost(QHostAddress::LocalHost, entrance.serverPort());
    QTRY_VERIFY(entrance.hasPendingConnections());
    client->acceptLocal(entrance.nextPendingConnection());
    QTRY_VERIFY(coreServer.hasPendingConnections());
    remote = coreServer.nextPendingConnection();
    // Wait for the activation acknowledgement by sending a payload through the real stream.
    local.write("begin");
    QTRY_COMPARE(remote->bytesAvailable(), 5);
    QCOMPARE(remote->readAll(), QByteArray("begin"));
  }

private Q_SLOTS:
  void initTestCase()
  {
    QVERIFY(files.isValid());
    identity("alice", alice, alicePin);
    identity("bob", bob, bobPin);
  }
  void init()
  {
    QVERIFY(coreServer.listen(QHostAddress::LocalHost, 0));
    QVERIFY(entrance.listen(QHostAddress::LocalHost, 0));
    client = std::make_unique<FailoverSession>(false);
    server = std::make_unique<FailoverSession>(true);
    server->setCorePort(coreServer.serverPort());
  }
  void synchronousWriteNotificationDoesNotReenterTlsDrain()
  {
    auto *wire = new SynchronousWriteDevice;
    TlsChannel channel(wire, false, alice, bobPin, this);
    QSignalSpy disconnected(&channel, &TlsChannel::disconnected);
    channel.start();
    QVERIFY(!wire->output.isEmpty());
    const auto hello = wire->output;
    QCoreApplication::processEvents();
    QVERIFY(!wire->reentered);
    QCOMPARE(wire->output, hello);
    QCOMPARE(disconnected.count(), 0);
  }
  void cleanup()
  {
    client->stop();
    server->stop();
    client.reset();
    server.reset();
    coreServer.close();
    entrance.close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
  void physicalNetworkConnectorRetriesAfterPeerRestart()
  {
    QTcpServer port;
    QVERIFY(port.listen(QHostAddress::LocalHost, 0));
    const auto networkPort = port.serverPort();
    port.close();
    AutoTransport::Options left, right;
    left.host = "127.0.0.1";
    left.networkPort = networkPort;
    left.bluetoothPeer = QBluetoothAddress("02:00:00:00:00:01");
    left.pem = alice;
    left.peerSha256 = bobPin;
    right = left;
    right.server = true;
    right.pem = bob;
    right.peerSha256 = alicePin;
    right.corePort = coreServer.serverPort();
    auto remote = std::make_unique<AutoTransport>(right);
    QSignalSpy remoteStarted(remote.get(), &AutoTransport::started);
    remote->start();
    QCOMPARE(remoteStarted.count(), 1);
    AutoTransport local(left);
    QSignalSpy localStarted(&local, &AutoTransport::started);
    local.start();
    QCOMPARE(localStarted.count(), 1);
    const auto localPort = localStarted.first().first().value<quint16>();
    QTcpSocket core;
    for (int attempt = 0; attempt < 30 && !coreServer.hasPendingConnections(); ++attempt) {
      core.abort();
      core.connectToHost(QHostAddress::LocalHost, localPort);
      QTest::qWait(100);
    }
    QVERIFY(coreServer.hasPendingConnections());
    auto *first = coreServer.nextPendingConnection();
    core.write("network-one");
    QTRY_COMPARE(first->bytesAvailable(), 11);
    QCOMPARE(first->readAll(), QByteArray("network-one"));
    local.coreConnected(true);
    remote->coreConnected(true);
    QTest::qWait(550);
    remote->stop();
    remote.reset();
    QTRY_COMPARE(core.state(), QAbstractSocket::UnconnectedState);
    QTRY_COMPARE(first->state(), QAbstractSocket::UnconnectedState);
    remote = std::make_unique<AutoTransport>(right);
    remote->start();
    for (int attempt = 0; attempt < 40 && !coreServer.hasPendingConnections(); ++attempt) {
      core.abort();
      core.connectToHost(QHostAddress::LocalHost, localPort);
      QTest::qWait(100);
    }
    QVERIFY(coreServer.hasPendingConnections());
    auto *second = coreServer.nextPendingConnection();
    core.write("network-two");
    QTRY_COMPARE(second->bytesAvailable(), 11);
    QCOMPARE(second->readAll(), QByteArray("network-two"));
    local.stop();
    remote->stop();
  }

  void standbyDoesNotCreateControlSession()
  {
    addLane(0);
    addLane(1);
    QVERIFY(!coreServer.hasPendingConnections());
    QTest::qWait(200);
    QVERIFY(clientLinks[0] && clientLinks[1]);
    QCOMPARE(server->selectedLane(), -1);
    QCOMPARE(client->selectedLane(), -1);
  }
  void fingerprintMismatchIsRejected()
  {
    addLane(0, true);
    QTRY_VERIFY(!clientLinks[0]);
    QTRY_VERIFY(!serverLinks[0]);
    QVERIFY(!coreServer.hasPendingConnections());
  }
  void peerInstancesCannotBeMixed()
  {
    addLane(0);
    addLane(1, false, QUuid::createUuid().toRfc4122());
    QTRY_VERIFY(!serverLinks[1]);
    QVERIFY(serverLinks[0] && serverLinks[0]->ready());
  }
  void silentOrMalformedPeerIsRemoved_data()
  {
    QTest::addColumn<bool>("malformed");
    QTest::newRow("heartbeat-blackhole") << false;
    QTest::newRow("oversized-frame") << true;
  }
  void silentOrMalformedPeerIsRemoved()
  {
    QFETCH(bool, malformed);
    QTcpServer listener;
    QVERIFY(listener.listen(QHostAddress::LocalHost, 0));
    auto *out = new QTcpSocket;
    out->connectToHost(QHostAddress::LocalHost, listener.serverPort());
    QTRY_VERIFY(listener.hasPendingConnections());
    auto *rawPeer = new TlsChannel(listener.nextPendingConnection(), true, bob, alicePin, server.get());
    auto *tls = new TlsChannel(out, false, alice, bobPin, this);
    auto *link = new FailoverLink(tls, false, client->instanceId(), this, 30, 200);
    QSignalSpy closed(link, &FailoverLink::disconnected);
    connect(rawPeer, &TlsChannel::authenticated, rawPeer, [rawPeer, this, malformed] {
      const auto hello = QByteArray("DFF2") + char(1) + server->instanceId();
      QByteArray header(13, '\0');
      header[0] = char(FailoverLink::Hello);
      qToBigEndian<quint32>(hello.size(), header.data() + 1);
      rawPeer->write(header + hello);
      if (malformed) {
        header[0] = char(FailoverLink::Data);
        qToBigEndian<quint32>(FailoverLink::MaxData + 1, header.data() + 1);
        rawPeer->write(header);
      }
    });
    connect(rawPeer, &TlsChannel::readyRead, rawPeer, [rawPeer] { rawPeer->readAll(); });
    client->attach(FailoverSession::Network, link);
    rawPeer->start();
    QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 2000);
    QVERIFY(!coreServer.hasPendingConnections());
  }

  void laneStatisticsAreMeasuredAndRetiredOnDisconnect()
  {
    addLane(0);
    addLane(1);
    QTcpSocket local;
    QTcpSocket *remote = nullptr;
    connectCore(local, remote);
    QTRY_VERIFY(clientLinks[0]->statistics().value("metricsFresh").toBool());
    QTRY_VERIFY(clientLinks[1]->statistics().value("rttMs").isDouble());
    QVERIFY(clientLinks[0]->statistics().value("rttMs").toDouble() >= 0);
    QVERIFY(!clientLinks[0]->statistics().value("sampledAtMs").toString().isEmpty());
    QByteArray payload(256 * 1024, 'm');
    local.write(payload);
    QTRY_COMPARE(remote->bytesAvailable(), qint64(payload.size()));
    QCOMPARE(remote->readAll(), payload);
    QTRY_VERIFY(clientLinks[0]->statistics().value("txBytesPerSecond").toDouble() > 1000);
    QTRY_VERIFY(serverLinks[0]->statistics().value("rxBytesPerSecond").toDouble() > 1000);
    const auto standbyRate=clientLinks[1]->statistics().value("txBytesPerSecond").toDouble();
    QVERIFY(standbyRate < 1000); // Heartbeat/control traffic is not fabricated data throughput.
    clientLinks[1]->stop("metrics-disconnect");
    QVERIFY(clientLinks[1]->statistics().isEmpty());
  }
  void networkPreferredAndBidirectionalBinary()
  {
    addLane(1);
    addLane(0);
    QTcpSocket local;
    QTcpSocket *remote = nullptr;
    connectCore(local, remote);
    QCOMPARE(client->selectedLane(), int(FailoverSession::Network));
    QByteArray payload(2000000, '\0');
    for (int i = 0; i < payload.size(); ++i)
      payload[i] = char(i % 256);
    remote->setReadBufferSize(1);
    local.write(payload);
    QTest::qWait(100);
    QCOMPARE(local.state(), QAbstractSocket::ConnectedState);
    remote->setReadBufferSize(0);
    QTRY_COMPARE(remote->bytesAvailable(), payload.size());
    QCOMPARE(remote->readAll(), payload);
    local.setReadBufferSize(1);
    remote->write(payload);
    QTest::qWait(100);
    QCOMPARE(remote->state(), QAbstractSocket::ConnectedState);
    local.setReadBufferSize(0);
    QTRY_COMPARE(local.bytesAvailable(), payload.size());
    QCOMPARE(local.readAll(), payload);
  }
  void activeLossClosesCoreAndReconnectsOnBackup()
  {
    addLane(0);
    addLane(1);
    QTcpSocket old;
    QTcpSocket *oldRemote = nullptr;
    connectCore(old, oldRemote);
    client->coreConnected(true);
    server->coreConnected(true);
    QTest::qWait(550);
    QSignalSpy states(client.get(), &FailoverSession::transportState);
    clientLinks[0]->stop("test network failure");
    QTRY_COMPARE(old.state(), QAbstractSocket::UnconnectedState);
    QTRY_COMPARE(oldRemote->state(), QAbstractSocket::UnconnectedState);
    QVERIFY(!states.isEmpty());
    QCOMPARE(states.first().first().toString(), QString("switching"));
    QTcpSocket fresh;
    QTcpSocket *newRemote = nullptr;
    connectCore(fresh, newRemote);
    QCOMPARE(client->selectedLane(), int(FailoverSession::Bluetooth));
    newRemote->write("fresh-only");
    QTRY_COMPARE(fresh.bytesAvailable(), 10);
    QCOMPARE(fresh.readAll(), QByteArray("fresh-only"));
    client->coreConnected(true);
    QTRY_COMPARE(states.last().first().toString(), QString("bluetooth"));
    // Network returns in standby. Do not interrupt Bluetooth or steal focus.
    addLane(0);
    QCOMPARE(client->selectedLane(), int(FailoverSession::Bluetooth));
    // A late CLOSE or activation from epoch 1 cannot terminate epoch 2.
    clientLinks[1]->send(FailoverLink::Close, 1);
    clientLinks[0]->send(FailoverLink::Activate, 1);
    fresh.write("still-live");
    QTRY_COMPARE(newRemote->bytesAvailable(), 10);
    QCOMPARE(newRemote->readAll(), QByteArray("still-live"));
  }
  void standbyLossDoesNotInterruptInput()
  {
    addLane(0);
    addLane(1);
    QTcpSocket local;
    QTcpSocket *remote = nullptr;
    connectCore(local, remote);
    client->coreConnected(true);
    QTest::qWait(550);
    QSignalSpy states(client.get(), &FailoverSession::transportState);
    clientLinks[1]->stop("standby failure");
    QTest::qWait(100);
    QCOMPARE(states.count(), 0);
    local.write("active");
    QTRY_COMPARE(remote->bytesAvailable(), 6);
    QCOMPARE(local.state(), QAbstractSocket::ConnectedState);
  }
  void offlineReportedOnceAndStopDoesNotReconnect()
  {
    addLane(0);
    QTcpSocket local;
    QTcpSocket *remote = nullptr;
    connectCore(local, remote);
    client->coreConnected(true);
    QTest::qWait(550);
    QSignalSpy states(client.get(), &FailoverSession::transportState);
    clientLinks[0]->stop("all links lost");
    QTRY_COMPARE_WITH_TIMEOUT(states.count(), 2, 3500);
    QCOMPARE(states.last().first().toString(), QString("offline"));
    QTest::qWait(150);
    QCOMPARE(states.count(), 2);
    client->stop();
    client->coreConnected(true);
    QTest::qWait(550);
    QCOMPARE(states.count(), 2);
  }
};
QTEST_GUILESS_MAIN(FailoverTests)
#include "FailoverTests.moc"
