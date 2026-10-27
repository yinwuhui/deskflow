// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include <QJsonObject>
#include <QLockFile>
#include <QMap>
#include <QString>
#include <memory>

namespace deskflow::display::infra {
enum class ResourcePhase { Intent, Observed, Active, Releasing, CleanupRequired, ReleaseVerified, Closed };
struct ResourceEvidence {
  QString kind = "unproven";
  QString reference;
  QString status = "unknown";
  bool operator==(const ResourceEvidence&) const = default;
};
struct ResourceRecord {
  QString id, owner, creationIntent, session, planHash;
  QString userSession, bootInstance, provider, providerVersion, kind;
  quint64 sequence = 0;
  ResourcePhase phase = ResourcePhase::Intent;
  QString platformReference;
  QStringList dependencies;
  ResourceEvidence evidence;
  bool operator==(const ResourceRecord&) const = default;
};
class DurableResourceLedger {
public:
  enum class Status { Ready, Busy, Unavailable, Corrupt, UnsupportedVersion };
  explicit DurableResourceLedger(QString directory, quint64 logLimit = 4 * 1024 * 1024);
  Status status() const { return m_status; }
  QString error() const { return m_error; }
  const QMap<QString,ResourceRecord>& records() const { return m_records; }
  QString activeOwner() const;
  bool createIntent(ResourceRecord);
  bool transition(const QString& resourceId, ResourcePhase, QString platformReference,
                  ResourceEvidence);
  bool compact();
  static QJsonObject encodeRecord(const ResourceRecord&);
private:
  bool load();
  bool checkpoint();
  bool append(ResourceRecord);
  bool apply(const ResourceRecord&, bool replay);
  bool valid(const ResourceRecord&) const;
  bool reject(QString error, Status status);
  void pruneClosed();
  QString m_directory, m_generation, m_error;
  Status m_status = Status::Unavailable;
  std::unique_ptr<QLockFile> m_lock;
  QMap<QString,ResourceRecord> m_records;
  quint64 m_sequence = 0;
  quint64 m_logLimit;
};
} // namespace deskflow::display::infra
