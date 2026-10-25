// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "Coordinator.h"
#include <QLockFile>
#include <QString>
#include <memory>
#include <optional>
namespace deskflow::display {
struct OwnedResource {
  Key owner;
  QString peerIdentity;
  Role role = Role::None;
  QString displayId;
};
// One active ownership record per user/application. Record before spawning.
// A record never authorizes deletion by name or sending signals to a stored PID.
class ResourceJournal {
public:
  enum class Status { Empty, Pending, Unavailable, Corrupt };
  explicit ResourceJournal(const QString& directory);
  Status status() const { return m_status; }
  const std::optional<OwnedResource>& record() const { return m_record; }
  bool claim(const OwnedResource& resource);
  bool updateDisplay(const Key& owner, const QString& displayId);
  bool clearVerified(const Key& owner, bool verified);
private:
  bool write(const std::optional<OwnedResource>& resource);
  QString m_path;
  std::unique_ptr<QLockFile> m_lock;
  Status m_status = Status::Unavailable;
  std::optional<OwnedResource> m_record;
};
} // namespace deskflow::display
