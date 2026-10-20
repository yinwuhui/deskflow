// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "PeerIdentityStore.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <utility>
namespace {
bool validHash(const QByteArray &hash)
{
  return hash.size() == 64 && QByteArray::fromHex(hash).size() == 32 && QByteArray::fromHex(hash).toHex() == hash;
}
bool valid(const PeerIdentityStore::Record &record)
{
  return (record.role == "client" || record.role == "server") && !record.key.isEmpty() && record.key.size() <= 512 &&
         record.identifier.size() <= 512 && validHash(record.fingerprint);
}
} // namespace
PeerIdentityStore::PeerIdentityStore(QString path) : m_path(std::move(path))
{
}
bool PeerIdentityStore::read(QList<Record> &result) const
{
  result.clear();
  QFile file(m_path);
  if (!file.exists())
    return true;
  if (file.size() > 2 * 1024 * 1024 || !file.open(QIODevice::ReadOnly))
    return false;
  return decode(file.readAll(), result);
}
bool PeerIdentityStore::decode(const QByteArray &bytes, QList<Record> &result)
{
  result.clear();
  if (bytes.size() > 2 * 1024 * 1024)
    return false;
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(bytes, &error);
  const auto object = document.object();
  if (error.error != QJsonParseError::NoError || object.value("version").toInt() != 1 ||
      !object.value("peers").isArray())
    return false;
  const auto peers = object.value("peers").toArray();
  if (peers.size() > 10000)
    return false;
  QSet<QString> keys;
  for (const auto &peer : peers) {
    const auto item = peer.toObject();
    Record record{
        item.value("role").toString(), item.value("key").toString(), item.value("identifier").toString(),
        item.value("acceptedAt").toString(), item.value("sha256").toString().toLatin1()
    };
    const auto key = record.role + ":" + record.key;
    if (!valid(record) || keys.contains(key))
      return false;
    keys.insert(key);
    result.append(record);
  }
  return true;
}
bool PeerIdentityStore::records(QList<Record> &result) const
{
  return read(result);
}
PeerIdentityStore::Status
PeerIdentityStore::check(const QString &role, const QString &key, const QByteArray &fingerprint, Record *previous) const
{
  if (previous)
    *previous = {};
  if (!valid({role, key, {}, {}, fingerprint}))
    return Status::Unavailable;
  QList<Record> all;
  if (!read(all))
    return Status::Unavailable;
  for (const auto &record : all)
    if (record.role == role && record.key == key) {
      if (previous)
        *previous = record;
      return record.fingerprint == fingerprint ? Status::Match : Status::Changed;
    }
  return Status::Unbound;
}
bool PeerIdentityStore::accept(const Record &record, const QByteArray &expectedPrevious)
{
  if (!valid(record))
    return false;
  if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
    return false;
  QLockFile lock(m_path + ".lock");
  if (!lock.tryLock(250))
    return false;
  QList<Record> all;
  if (!read(all))
    return false;
  auto updated = record;
  updated.acceptedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  bool found = false;
  for (auto &entry : all)
    if (entry.role == record.role && entry.key == record.key) {
      if (entry.fingerprint == record.fingerprint)
        return true;
      if (entry.fingerprint != expectedPrevious)
        return false;
      entry = updated;
      found = true;
      break;
    }
  if (!found) {
    if (!expectedPrevious.isEmpty() || all.size() >= 10000)
      return false;
    all.append(updated);
  }
  QJsonArray peers;
  for (const auto &entry : all)
    peers.append(
        QJsonObject{
            {"role", entry.role},
            {"key", entry.key},
            {"identifier", entry.identifier},
            {"acceptedAt", entry.acceptedAt},
            {"sha256", QString::fromLatin1(entry.fingerprint)}
        }
    );
  const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"peers", peers}}).toJson(QJsonDocument::Compact);
  QSaveFile file(m_path);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
  return file.write(bytes) == bytes.size() && file.commit();
}
