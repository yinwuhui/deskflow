// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

// Public certificate bindings only. Never stores private keys or discovery assertions.
class PeerIdentityStore
{
public:
  struct Record
  {
    QString role, key, identifier, acceptedAt;
    QByteArray fingerprint;
  };
  enum class Status
  {
    Match,
    Unbound,
    Changed,
    Unavailable
  };
  explicit PeerIdentityStore(QString path);
  Status
  check(const QString &role, const QString &key, const QByteArray &fingerprint, Record *previous = nullptr) const;
  bool records(QList<Record> &result) const;
  static bool decode(const QByteArray &bytes, QList<Record> &result);
  // Compare-and-swap prevents an older confirmation from overwriting a newer identity.
  bool accept(const Record &record, const QByteArray &expectedPrevious);

private:
  bool read(QList<Record> &result) const;
  QString m_path;
};
