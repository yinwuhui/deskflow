/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "TlsUtility.h"

#include "common/Settings.h"
#include "net/SecureUtils.h"

#include <QFile>
#include <QSaveFile>
#include <QSslCertificate>
#include <QSslKey>
#include <QString>
#include <QTemporaryFile>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace deskflow::gui::TlsUtility {

bool isEnabled()
{
  return Settings::value(Settings::Security::TlsEnabled).toBool();
}

bool isCertValid(const QString &certPath)
{
  const auto certs = QSslCertificate::fromPath(certPath);
  if (certs.isEmpty()) {
    //: %1 will be replaced by the certificate path
    qDebug() << QObject::tr("failed to read key from certificate file: %1").arg(certPath);
    return false;
  }

  const auto cert = certs.first();
  if (cert.isNull()) {
    //: %1 will be replaced by the certificate path
    qDebug() << QObject::tr("failed to parse certificate file: %1").arg(certPath);
    return false;
  }

  BIO *bio = BIO_new_file(QFile::encodeName(certPath).constData(), "r");
  if (!bio)
    return false;
  EVP_PKEY *privateKey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  bio = BIO_new_file(QFile::encodeName(certPath).constData(), "r");
  X509 *identity = bio ? PEM_read_bio_X509(bio, nullptr, nullptr, nullptr) : nullptr;
  if (bio)
    BIO_free(bio);
  const bool paired = privateKey && identity && X509_check_private_key(identity, privateKey) == 1;
  if (privateKey)
    EVP_PKEY_free(privateKey);
  if (identity)
    X509_free(identity);
  if (!paired)
    return false;
  const auto key = cert.publicKey();
  if (key.isNull()) {
    //: %1 will be replaced by the certificate path
    qDebug() << QObject::tr("failed to read key from certificate file: %1").arg(certPath);
    return false;
  }

  if (key.length() < 2048) {
    qDebug() << QObject::tr("key detected is the incorrect size");
    return false;
  }

  if (key.algorithm() != QSsl::Rsa) {
    //: %1 will be replaced by the certificate path
    qDebug() << QObject::tr("failed to read RSA key from certificate file: %1").arg(certPath);
    return false;
  }

  return true;
}

int getCertKeyLength(const QString &certPath)
{
  QFile file(certPath);
  if (!file.open(QFile::ReadOnly)) {
    //: %1 will be replaced by the certificate path
    qDebug() << QObject::tr("failed to read key from certificate file: %1").arg(certPath);
    return -1;
  }

  const auto key = QSslKey(&file, QSsl::Rsa);
  if (key.isNull()) {
    //: %1 will be replaced by the certificate path
    qDebug() << QObject::tr("failed to parse certificate file: %1").arg(certPath);
    return -1;
  }
  return key.length();
}

QByteArray certFingerprint(const QString &certPath)
{
  QByteArray fingerprint;

  const auto certs = QSslCertificate::fromPath(certPath);
  if (certs.isEmpty()) {
    //: %1 will be replaced by the certificate path
    qDebug() << QObject::tr("failed to read key from certificate file: %1").arg(certPath);
    return fingerprint;
  }

  const auto cert = certs.first();
  if (cert.isNull()) {
    //: %1 will be replaced by the certificate path
    qWarning() << QObject::tr("failed to parse certificate file: %1").arg(certPath);
    return fingerprint;
  }

  return cert.digest(QCryptographicHash::Sha256);
}

bool generateCertificate(int requestedKeyLength)
{
  qDebug(
      "generating tls certificate, "
      "all clients must trust the new fingerprint"
  );

  const auto keyLength =
      requestedKeyLength ? requestedKeyLength : std::max(2048, Settings::value(Settings::Security::KeySize).toInt());
  if (keyLength != 2048 && keyLength != 4096)
    return false;
  const auto certPath = Settings::value(Settings::Security::Certificate).toString();

  return generateCertificateAtPath(certPath, keyLength);
}

bool generateCertificateAtPath(const QString &certPath, int keyLength)
{
  if (certPath.isEmpty() || (keyLength != 2048 && keyLength != 4096))
    return false;
  QFileInfo info(certPath);
  if (QDir dir(info.absolutePath()); !dir.exists() && !dir.mkpath(".")) {
    qCritical("failed to create directory for tls certificate");
    return false;
  }

  try {
    QTemporaryFile staged(info.absolutePath() + "/.identity-XXXXXX.pem");
    if (!staged.open())
      return false;
    const auto stagedPath = staged.fileName();
    staged.close();
    deskflow::generatePemSelfSignedCert(stagedPath, keyLength);
    if (!isCertValid(stagedPath))
      return false;
    QFile source(stagedPath);
    if (!source.open(QIODevice::ReadOnly))
      return false;
    const auto data = source.readAll();
    QSaveFile replacement(certPath);
    if (!replacement.open(QIODevice::WriteOnly))
      return false;
    replacement.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (replacement.write(data) != data.size() || !replacement.commit())
      return false;
  } catch (const std::exception &e) {
    qCritical() << "failed to generate self-signed pem cert:" << e.what();
    return false;
  }
  qDebug("tls certificate generated");
  return true;
}

} // namespace deskflow::gui::TlsUtility
