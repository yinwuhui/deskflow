// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "ConfigurationBackup.h"
#include "net/PeerIdentityStore.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
namespace deskflow::gui {
QByteArray ConfigurationBackup::encode() const
{
  QByteArray values;
  QDataStream stream(&values, QIODevice::WriteOnly);
  stream.setVersion(QDataStream::Qt_6_7);
  stream << settings;
  QJsonObject archive{
      {"format", "deskflow-profile-backup"},
      {"version", 2},
      {"settings", QString::fromLatin1(values.toBase64())},
      {"serverConfig", QString::fromLatin1(serverConfig.toBase64())},
      {"trustedServers", QString::fromLatin1(trustedServers.toBase64())},
      {"trustedClients", QString::fromLatin1(trustedClients.toBase64())},
      {"revokedFingerprints", QString::fromLatin1(revokedFingerprints.toBase64())},
      {"peerIdentities", QString::fromLatin1(peerIdentities.toBase64())},
      {"sha256", QString::fromLatin1(
                     QCryptographicHash::hash(
                         values + serverConfig + trustedServers + trustedClients + revokedFingerprints + peerIdentities,
                         QCryptographicHash::Sha256
                     )
                         .toHex()
                 )}
  };
  return QJsonDocument(archive).toJson(QJsonDocument::Indented);
}
bool ConfigurationBackup::decode(const QByteArray &bytes, ConfigurationBackup &result, QString &error)
{
  if (bytes.size() > 16 * 1024 * 1024) {
    error = "Backup exceeds 16 MiB";
    return false;
  }
  QJsonParseError parseError;
  const auto document = QJsonDocument::fromJson(bytes, &parseError);
  const auto object = document.object();
  if (parseError.error != QJsonParseError::NoError || object.value("format") != "deskflow-profile-backup" ||
      (object.value("version").toInt() != 1 && object.value("version").toInt() != 2)) {
    error = "Unsupported or invalid backup format";
    return false;
  }
  const auto values =
      QByteArray::fromBase64(object.value("settings").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
  ConfigurationBackup candidate;
  candidate.serverConfig = QByteArray::fromBase64(
      object.value("serverConfig").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors
  );
  candidate.trustedServers = QByteArray::fromBase64(
      object.value("trustedServers").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors
  );
  candidate.trustedClients = QByteArray::fromBase64(
      object.value("trustedClients").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors
  );
  candidate.revokedFingerprints = QByteArray::fromBase64(
      object.value("revokedFingerprints").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors
  );
  if (object.value("version").toInt() == 2)
    candidate.peerIdentities = QByteArray::fromBase64(
        object.value("peerIdentities").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors
    );
  const auto digest = QCryptographicHash::hash(
                          values + candidate.serverConfig + candidate.trustedServers + candidate.trustedClients +
                              candidate.revokedFingerprints + candidate.peerIdentities,
                          QCryptographicHash::Sha256
  )
                          .toHex();
  if (values.isEmpty() || object.value("sha256").toString().toLatin1() != digest) {
    error = "Backup integrity check failed";
    return false;
  }
  if (!candidate.peerIdentities.isEmpty()) {
    QList<PeerIdentityStore::Record> peers;
    if (!PeerIdentityStore::decode(candidate.peerIdentities, peers)) {
      error = "Invalid device identity bindings";
      return false;
    }
  }
  QDataStream stream(values);
  stream.setVersion(QDataStream::Qt_6_7);
  stream >> candidate.settings;
  if (stream.status() != QDataStream::Ok || !stream.atEnd()) {
    error = "Invalid settings payload";
    return false;
  }
  result = candidate;
  error.clear();
  return true;
}
} // namespace deskflow::gui
