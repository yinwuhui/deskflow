// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "common/Settings.h"
#include "gui/TlsUtility.h"
#include "net/FingerprintDatabase.h"
#include "net/PeerIdentityStore.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace deskflow::gui;
class IdentityTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void identityChangesRequireExplicitCompareAndSwap()
  {
    QTemporaryDir directory;
    PeerIdentityStore store(directory.filePath("identities.json"));
    const QByteArray first(64, 'a'), second(64, 'b'), third(64, 'c');
    using Status = PeerIdentityStore::Status;
    QCOMPARE(store.check("client", "stable-id", first), Status::Unbound);
    QVERIFY(store.accept({"client", "stable-id", "peer", {}, first}, {}));
    QCOMPARE(store.check("client", "stable-id", first), Status::Match);
    PeerIdentityStore::Record previous;
    QCOMPARE(store.check("client", "stable-id", second, &previous), Status::Changed);
    QCOMPARE(previous.fingerprint, first);
    QVERIFY(!store.accept({"client", "stable-id", "peer", {}, second}, {}));
    QVERIFY(store.accept({"client", "stable-id", "peer", {}, second}, first));
    QVERIFY(!store.accept({"client", "stable-id", "peer", {}, third}, first));
    QCOMPARE(store.check("client", "stable-id", second), Status::Match);
    QCOMPARE(store.check("server", "stable-id", second), Status::Unbound);
    QFile corrupt(directory.filePath("identities.json"));
    QVERIFY(corrupt.open(QIODevice::WriteOnly));
    corrupt.write("corrupt");
    corrupt.close();
    QCOMPARE(store.check("client", "stable-id", first), Status::Unavailable);
    QVERIFY(!store.accept({"client", "stable-id", "peer", {}, first}, {}));
  }
  void regenerationKeepsPeerTrustAndRejectsMismatchedKey()
  {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    qputenv("XDG_CONFIG_HOME", QDir::tempPath().toUtf8() + "/deskflow-unit-tests");
    qputenv("XDG_STATE_HOME", QDir::tempPath().toUtf8() + "/deskflow-unit-test-state");
    Settings::setSettingsFile(directory.filePath("profile.conf"));
    Settings::setValue(Settings::Security::KeySize, 2048);
    const auto first = directory.filePath("first.pem");
    Settings::setValue(Settings::Security::Certificate, first);
    Fingerprint peer{QCryptographicHash::Sha256, QByteArray(32, 'a')};
    FingerprintDatabase trusted;
    trusted.addTrusted(peer);
    const auto trustPath = directory.filePath("trusted");
    QVERIFY(trusted.write(trustPath));
    QFile trustFile(trustPath);
    QVERIFY(trustFile.open(QIODevice::ReadOnly));
    const auto trustBytes = trustFile.readAll();
    trustFile.close();
    QVERIFY(TlsUtility::generateCertificate());
    QVERIFY(TlsUtility::isCertValid(first));
    const auto fingerprint = TlsUtility::certFingerprint(first);
    QCOMPARE(fingerprint.size(), 32);
    const auto second = directory.filePath("second.pem");
    Settings::setValue(Settings::Security::Certificate, second);
    QVERIFY(TlsUtility::generateCertificate());
    QVERIFY(TlsUtility::certFingerprint(second) != fingerprint);
    QVERIFY(trustFile.open(QIODevice::ReadOnly));
    QCOMPARE(trustFile.readAll(), trustBytes);
    QFile a(first), b(second);
    QVERIFY(a.open(QIODevice::ReadOnly));
    QVERIFY(b.open(QIODevice::ReadOnly));
    const auto bytesA = a.readAll(), bytesB = b.readAll();
    const auto mixedPath = directory.filePath("mixed.pem");
    QFile mixed(mixedPath);
    QVERIFY(mixed.open(QIODevice::WriteOnly));
    mixed.write(
        bytesB.left(bytesB.indexOf("-----BEGIN CERTIFICATE-----")) +
        bytesA.mid(bytesA.indexOf("-----BEGIN CERTIFICATE-----"))
    );
    mixed.close();
    QVERIFY(!TlsUtility::isCertValid(mixedPath));
    FingerprintDatabase restored;
    restored.read(trustPath);
    QVERIFY(restored.isTrusted(peer));
  }
  void revokingOneFingerprintPreservesTheOther()
  {
    QTemporaryDir directory;
    Fingerprint first{QCryptographicHash::Sha256, QByteArray(32, 'a')};
    Fingerprint second{QCryptographicHash::Sha256, QByteArray(32, 'b')};
    FingerprintDatabase database;
    database.addTrusted(first);
    database.addTrusted(second);
    database.removeTrusted(first);
    QVERIFY(database.write(directory.filePath("trusted")));
    FingerprintDatabase restored;
    restored.read(directory.filePath("trusted"));
    QVERIFY(!restored.isTrusted(first));
    QVERIFY(restored.isTrusted(second));
    QCOMPARE(restored.fingerprints().size(), 1);
  }
};
QTEST_MAIN(IdentityTests)
#include "IdentityTests.moc"
