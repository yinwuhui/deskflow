// SPDX-License-Identifier: MIT
#include "../../lib/common/PeerPolicy.h"
#include "ConnectionManager.h"
#include "../../lib/common/PeerConnectionRegistry.h"
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>
#include <openssl/pem.h>
class ConnectionManagerTests : public QObject
{
  Q_OBJECT
  QTemporaryDir files;
  QString alice, bob, carol;
  QByteArray alicePin, bobPin, carolPin;
  QTcpServer core;
  std::unique_ptr<ConnectionManager> server, client;
  quint16 localPort = 0;
  QPointer<QTcpSocket> initialBluetooth;
  QMap<QString, QJsonObject> store;
  static QJsonObject first(const ConnectionManager &m)
  {
    return m.snapshot().value("peers").toArray().first().toObject();
  }
  void identity(const QString &name, QString &path, QByteArray &pin)
  {
    auto *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    QVERIFY(ctx);
    QVERIFY(EVP_PKEY_keygen_init(ctx) > 0);
    QVERIFY(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) > 0);
    EVP_PKEY *key = nullptr;
    QVERIFY(EVP_PKEY_keygen(ctx, &key) > 0);
    EVP_PKEY_CTX_free(ctx);
    auto *cert = X509_new();
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
    PEM_write_bio_X509(bio, cert);
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    char *data = nullptr;
    auto n = BIO_get_mem_data(bio, &data);
    path = files.filePath(name + ".pem");
    QFile out(path);
    QVERIFY(out.open(QIODevice::WriteOnly));
    QCOMPARE(out.write(data, n), n);
    unsigned char hash[32];
    unsigned int length;
    QVERIFY(X509_digest(cert, EVP_sha256(), hash, &length) == 1);
    pin = QByteArray(reinterpret_cast<char *>(hash), length);
    BIO_free(bio);
    X509_free(cert);
    EVP_PKEY_free(key);
  }
  void lane(ConnectionManager &left, ConnectionManager &right, int type, QTcpSocket *&out)
  {
    QTcpServer wire;
    QVERIFY(wire.listen(QHostAddress::LocalHost, 0));
    out = new QTcpSocket;
    out->connectToHost(QHostAddress::LocalHost, wire.serverPort());
    QTRY_VERIFY(wire.hasPendingConnections());
    auto *in = wire.nextPendingConnection();
    right.acceptTransport(in, type);
    left.acceptTransport(out, type);
  }
  void setup(int initial = 0, bool automatic = true, int serverTransports = 3)
  {
    QVERIFY(core.listen(QHostAddress::LocalHost, 0));
    QTcpServer reserve;
    QVERIFY(reserve.listen(QHostAddress::LocalHost, 0));
    const auto port = reserve.serverPort();
    reserve.close();
    ConnectionManager::Options right;
    right.server = true;
    right.serverTransports = serverTransports;
    right.pem = bob;
    right.name = "bob";
    right.port = port;
    right.corePort = core.serverPort();
    right.automatic = automatic;
    right.trusted = [this](const QByteArray &p) { return p == alicePin || p == carolPin; };
    right.bluetoothPermission = [](QObject *, bool, std::function<void(bool)> done) { done(false); };
    right.savePeer = [this](const QString &id, const QJsonObject &p) { store[id] = p; };
    server = std::make_unique<ConnectionManager>(right);
    server->start();
    ConnectionManager::Options left;
    left.pem = alice;
    left.name = "alice";
    left.host = initial == 0 ? QString("127.0.0.1") : QString();
    left.port = port;
    left.initialLane = initial;
    left.automatic = automatic;
    left.bluetoothAddress = initial == 1 ? QString("11:22:33:44:55:66") : QString();
    left.trusted = [this](const QByteArray &p) { return p == bobPin; };
    left.bluetoothPermission = right.bluetoothPermission;
    client = std::make_unique<ConnectionManager>(left);
    connect(client.get(), &ConnectionManager::started, this, [this](quint16 p) { localPort = p; });
    client->start();
    if (initial == 1) {
      QTcpSocket *wire;
      lane(*client, *server, 1, wire);
      initialBluetooth = wire;
    }
    QTRY_COMPARE(client->snapshot().value("peers").toArray().size(), 1);
    QTRY_COMPARE(server->snapshot().value("peers").toArray().size(), 1);
  }
  void online(QTcpSocket &local, QTcpSocket *&remote)
  {
    local.connectToHost(QHostAddress::LocalHost, localPort);
    QTRY_VERIFY(core.hasPendingConnections());
    remote = core.nextPendingConnection();
    local.write("hello");
    QTRY_COMPARE(remote->bytesAvailable(), 5);
    remote->readAll();
    client->coreConnected({}, true);
    server->coreConnected(QString::fromLatin1(alicePin.toHex()), true);
    QTRY_VERIFY(first(*client).value("state") == "network" || first(*client).value("state") == "bluetooth");
  }
private Q_SLOTS:
  void initTestCase()
  {
    identity("alice", alice, alicePin);
    identity("bob", bob, bobPin);
    identity("carol", carol, carolPin);
  }
  void cleanup()
  {
    client.reset();
    server.reset();
    core.close();
    store.clear();
    localPort = 0;
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
  void bluetoothOnlySharingAlsoRestrictsNativeSource()
  {
    setup(1,false,2);QTcpSocket local;QTcpSocket* remote=nullptr;online(local,remote);
    const auto id=QString::fromLatin1(alicePin.toHex());const QString connection(32,'f');
    PeerConnectionRegistry::update({{"baseConnectionId",connection},{"peerId",id},{"originalInputRole","source"}});
    auto policy=[&]{for(const auto& item:PeerConnectionRegistry::snapshot().value("connections").toArray())if(item.toObject().value("baseConnectionId")==connection)return item.toObject().value("displayNetworkPolicy").toString();return QString();};
    QTRY_COMPARE(policy(),QString("bluetoothOnly"));PeerConnectionRegistry::remove(connection);
  }
  void bluetoothDisplayPolicyTracksExplicitPermission()
  {
    setup(1,false);QTcpSocket local;QTcpSocket* remote=nullptr;online(local,remote);
    const auto id=QString::fromLatin1(bobPin.toHex());const QString connection(32,'e');
    PeerConnectionRegistry::update({{"baseConnectionId",connection},{"peerId",id},{"originalInputRole","receiver"}});
    auto policy=[&]{for(const auto& item:PeerConnectionRegistry::snapshot().value("connections").toArray())if(item.toObject().value("baseConnectionId")==connection)return item.toObject().value("displayNetworkPolicy").toString();return QString();};
    QTRY_COMPARE(policy(),QString("bluetoothOnly"));
    client->command({{"kind","policy"},{"peerId",id},{"enabled",true}});
    QTRY_COMPARE(policy(),QString("networkAllowed"));
    // Still using Bluetooth is not equivalent to forbidding the network.
    QCOMPARE(first(*client).value("state").toString(),QString("bluetooth"));
    client->command({{"kind","policy"},{"peerId",id},{"enabled",false}});
    QTRY_COMPARE(policy(),QString("bluetoothOnly"));PeerConnectionRegistry::remove(connection);
  }
  void networkFirstDoesNotNeedBluetoothAddress()
  {
    setup();
    QTcpSocket local;
    QTcpSocket *remote;
    online(local, remote);
    QTRY_COMPARE(first(*client).value("state").toString(), QString("network"));
    QTRY_VERIFY(!client->snapshot().value("bluetoothReason").toString().isEmpty());
    local.write("still works");
    QTRY_COMPARE(remote->bytesAvailable(), 11);
    QCOMPARE(first(*client).value("channels").toArray()[0].toObject().value("state").toString(), QString("active"));
    QVERIFY(first(*client).value("channels").toArray()[0].toObject().value("remoteAddress").toString().startsWith("127.0.0.1:"));
  }
  void bluetoothFirstLearnsNetworkWithoutPreconfiguredHost()
  {
    setup(1);
    QTcpSocket local;
    QTcpSocket *remote;
    online(local, remote);
    QTRY_COMPARE(first(*client).value("state").toString(), QString("bluetooth"));
    // A TLS-authenticated endpoint offer causes the real TCP connector to create the standby.
    QTRY_COMPARE(first(*client).value("channels").toArray()[0].toObject().value("state").toString(), QString("ready"));
    QCOMPARE(first(*client).value("channels").toArray()[1].toObject().value("state").toString(), QString("active"));
  }
  void disabledPolicyKeepsActiveAndClosesStandby()
  {
    setup();
    QTcpSocket local;
    QTcpSocket *remote;
    online(local, remote);
    QTcpSocket *backup;
    lane(*client, *server, 1, backup);
    QTRY_COMPARE(first(*client).value("backup").toString(), QString("ready"));
    const auto id = QString::fromLatin1(bobPin.toHex());
    client->command({{"kind", "policy"}, {"peerId", id}, {"enabled", false}});
    QTRY_VERIFY(!first(*server).value("effective").toBool());
    QTRY_COMPARE(
        first(*client).value("channels").toArray()[1].toObject().value("state").toString(), QString("unavailable")
    );
    QCOMPARE(local.state(), QAbstractSocket::ConnectedState);
    local.write("alive");
    QTRY_COMPARE(remote->bytesAvailable(), 5);
  }
  void disabledBackupStillReconnectsTheActiveTransport()
  {
    setup();
    QTcpSocket local;
    QTcpSocket *remote;
    online(local, remote);
    client->command({{"kind", "policy"}, {"peerId", QString::fromLatin1(bobPin.toHex())}, {"enabled", false}});
    bool found = false;
    for (auto *socket : client->findChildren<QTcpSocket *>())
      if (qobject_cast<TlsChannel *>(socket->parent())) {
        socket->abort();
        found = true;
        break;
      }
    QVERIFY(found);
    QTRY_COMPARE(local.state(), QAbstractSocket::UnconnectedState);
    QTRY_COMPARE(first(*client).value("channels").toArray()[0].toObject().value("state").toString(), QString("ready"));
    QTcpSocket restored;
    QTcpSocket *next;
    online(restored, next);
    QCOMPARE(first(*client).value("state").toString(), QString("network"));
    QVERIFY(!first(*client).value("effective").toBool());
  }
  void stopPeerPreventsRetryAndRemoteResurrection()
  {
    setup();
    QTcpSocket local;
    QTcpSocket *remote;
    online(local, remote);
    client->command({{"kind", "stopPeer"}, {"peerId", QString::fromLatin1(bobPin.toHex())}});
    QTRY_COMPARE(first(*client).value("state").toString(), QString("stopped"));
    QTest::qWait(1200);
    QCOMPARE(first(*client).value("channels").toArray()[0].toObject().value("retryInMs").toInt(), -1);
    QCOMPARE(local.state(), QAbstractSocket::UnconnectedState);
  }
  void bluetoothFailureRecoversViaLearnedNetwork()
  {
    setup(1);
    QTcpSocket local;
    QTcpSocket *remote;
    online(local, remote);
    QTRY_COMPARE(first(*client).value("backup").toString(), QString("ready"));
    const auto channels = first(*client).value("channels").toArray();
    QCOMPARE(channels[1].toObject().value("state").toString(), QString("active"));
    // Drop only the Bluetooth byte stream, preserving the authenticated network standby.
    // The supplied connector owns its socket under the TLS wrapper.
    QVERIFY(initialBluetooth);
    initialBluetooth->abort();
    QTRY_COMPARE(local.state(), QAbstractSocket::UnconnectedState);
    QTcpSocket restored;
    QTcpSocket *next;
    online(restored, next);
    QTRY_COMPARE(first(*client).value("state").toString(), QString("network"));
    QCOMPARE(next->bytesAvailable(), qint64(0));
  }
  void multiplePeersRemainIndependentWhenOneDisconnects()
  {
    setup();
    QTcpSocket local;
    QTcpSocket *remote;
    online(local, remote);
    ConnectionManager::Options options;
    options.pem = carol;
    options.name = "carol";
    options.host = "127.0.0.1";
    // Inject a connected network stream so this peer needs no configured server port.
    options.port = 1;
    options.automatic = false;
    options.trusted = [this](const QByteArray &pin) { return pin == bobPin; };
    options.bluetoothPermission = [](QObject *, bool, std::function<void(bool)> done) { done(false); };
    ConnectionManager other(options);
    quint16 otherPort = 0;
    connect(&other, &ConnectionManager::started, this, [&](quint16 p) { otherPort = p; });
    other.start();
    QTcpSocket *wire;
    lane(other, *server, 0, wire);
    QTRY_COMPARE(server->snapshot().value("peers").toArray().size(), 2);
    QTcpSocket otherCore;
    otherCore.connectToHost(QHostAddress::LocalHost, otherPort);
    QTRY_VERIFY(core.hasPendingConnections());
    auto *otherRemote = core.nextPendingConnection();
    otherCore.write("carol");
    QTRY_COMPARE(otherRemote->bytesAvailable(), 5);
    otherRemote->readAll();
    server->command({{"kind", "stopPeer"}, {"peerId", QString::fromLatin1(alicePin.toHex())}});
    QTRY_COMPARE(local.state(), QAbstractSocket::UnconnectedState);
    QCOMPARE(otherCore.state(), QAbstractSocket::ConnectedState);
    otherCore.write("still carol");
    QTRY_COMPARE(otherRemote->bytesAvailable(), 11);
  }
  void cancelIgnoresLateActionAndAllowsExplicitRetry()
  {
    setup();
    QTcpSocket local;
    QTcpSocket *remote;
    online(local, remote);
    QTRY_VERIFY(!first(*client).value("actionId").toString().isEmpty());
    const auto peerId = QString::fromLatin1(bobPin.toHex());
    const auto old = first(*client).value("actionId");
    client->command({{"kind", "action"}, {"peerId", peerId}, {"actionId", old}, {"cancel", true}});
    QVERIFY(first(*client).value("actionId").toString().isEmpty());
    client->command({{"kind", "action"}, {"peerId", peerId}, {"actionId", old}});
    QVERIFY(first(*client).value("actionId").toString().isEmpty());
    QCOMPARE(local.state(), QAbstractSocket::ConnectedState);
    client->command({{"kind", "retryBackup"}, {"peerId", peerId}});
    QTRY_VERIFY(!first(*client).value("actionId").toString().isEmpty());
    QVERIFY(first(*client).value("actionId") != old);
  }
  void stopDuringSleepDoesNotResume()
  {
    setup();
    QTcpSocket local;
    QTcpSocket *remote;
    online(local, remote);
    client->suspend(true);
    QTRY_COMPARE(local.state(), QAbstractSocket::UnconnectedState);
    client->command({{"kind", "stopPeer"}, {"peerId", QString::fromLatin1(bobPin.toHex())}});
    client->suspend(false);
    QTest::qWait(1100);
    QCOMPARE(first(*client).value("state").toString(), QString("stopped"));
    QCOMPARE(first(*client).value("channels").toArray()[0].toObject().value("retryInMs").toInt(), -1);
  }
  void runtimeDefaultDoesNotOverwriteExplicitPeerChoice()
  {
    setup();
    QTcpSocket local;
    QTcpSocket *remote;
    online(local, remote);
    QCOMPARE(first(*client).value("policySource").toString(), QString("default"));
    client->command({{"kind", "defaultPolicy"}, {"enabled", false}});
    QVERIFY(!first(*client).value("requested").toBool());
    client->command({{"kind", "policy"}, {"peerId", QString::fromLatin1(bobPin.toHex())}, {"enabled", false}});
    client->command({{"kind", "defaultPolicy"}, {"enabled", true}});
    QVERIFY(!first(*client).value("requested").toBool());
    QCOMPARE(first(*client).value("policySource").toString(), QString("peer"));
    QCOMPARE(local.state(), QAbstractSocket::ConnectedState);
  }
  void firstBluetoothPermissionDenialCanBeRetriedExplicitly()
  {
    ConnectionManager::Options options;
    options.pem = alice;
    options.initialLane = 1;
    options.bluetoothAddress = "11:22:33:44:55:66";
    options.trusted = [this](const QByteArray &p) { return p == bobPin; };
    int requests = 0;
    options.bluetoothPermission = [&](QObject *, bool, std::function<void(bool)> done) {
      ++requests;
      done(false);
    };
    ConnectionManager manager(options);
    manager.start();
    QCOMPARE(requests, 1);
    QCOMPARE(first(manager).value("peerId").toString(), QString("bootstrap"));
    QCOMPARE(first(manager).value("action").toString(), QString("bluetoothSettings"));
    QTest::qWait(1100);
    QCOMPARE(requests, 1);
    manager.command({{"kind", "retryBluetooth"}});
    QCOMPARE(requests, 2);
  }
  void sharingTransportSelectionControlsActualListeners_data()
  {
    QTest::addColumn<int>("mask");
    QTest::newRow("network-only") << 1;
    QTest::newRow("bluetooth-only") << 2;
    QTest::newRow("both") << 3;
  }
  void sharingTransportSelectionControlsActualListeners()
  {
    QFETCH(int, mask);
    QTcpServer reservation;
    QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const auto port = reservation.serverPort();
    reservation.close();
    ConnectionManager::Options options;
    options.server = true;
    options.pem = bob;
    options.port = port;
    options.serverTransports = mask;
    options.advertiseBluetooth = mask & 2;
    options.trusted = [](const QByteArray &) { return true; };
    int permissionRequests = 0;
    options.bluetoothPermission = [&](QObject *, bool, std::function<void(bool)> done) {
      ++permissionRequests;
      done(false);
    };
    ConnectionManager manager(options);
    QSignalSpy errors(&manager, &ConnectionManager::fatalError);
    manager.start();
    QCOMPARE(errors.count(), 0);
    QCOMPARE(manager.snapshot().value("networkListening").toBool(), bool(mask & 1));
    QCOMPARE(permissionRequests, mask & 2 ? 1 : 0);
    QVERIFY(!manager.snapshot().value("bluetoothListening").toBool());
    if (!(mask & 1)) {
      QTcpServer probe;
      QVERIFY(probe.listen(QHostAddress::Any, port));
    }
    if (!(mask & 2)) {
      manager.command({{"kind", "retryBluetooth"}});
      QCOMPARE(permissionRequests, 0);
      QTcpServer wire;
      QVERIFY(wire.listen(QHostAddress::LocalHost, 0));
      QTcpSocket outgoing;
      outgoing.connectToHost(QHostAddress::LocalHost, wire.serverPort());
      QTRY_VERIFY(wire.hasPendingConnections());
      auto incoming = wire.nextPendingConnection();
      manager.acceptTransport(incoming, 1);
      QTRY_COMPARE(outgoing.state(), QAbstractSocket::UnconnectedState);
      QVERIFY(manager.snapshot().value("peers").toArray().isEmpty());
    }
  }

  void peerClipboardLimitsAreIsolated()
  {
    PeerPolicy::setLimit("a", 64);
    QCOMPARE(PeerPolicy::limit("a", 1024 * 1024), size_t(65536));
    QCOMPARE(PeerPolicy::limit("b", 1024 * 1024), size_t(1024 * 1024));
    QCOMPARE(PeerPolicy::limit("a", 2048), size_t(2048));
    PeerPolicy::setLimit("a", 0);
    QCOMPARE(PeerPolicy::limit("a", 1024 * 1024), size_t(1024 * 1024));
  }
};
QTEST_GUILESS_MAIN(ConnectionManagerTests)
#include "ConnectionManagerTests.moc"
