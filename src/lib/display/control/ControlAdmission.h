// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "InfrastructureControl.h"
#include <QMap>
#include <memory>
#include <mutex>

namespace deskflow::display::infra {
enum class ControlTraffic
{
  Command,
  Signal,
  StopOrRevoke
};
// Retain per authenticated peer across base-connection reconnections. This is a
// single-executor object; timestamps come exclusively from local steady_clock.
class ControlRateLimiter
{
public:
  explicit ControlRateLimiter(TimePoint now);
  bool admit(ControlTraffic, TimePoint now);

private:
  struct Bucket
  {
    double tokens, capacity, rate;
  };
  Bucket m_commands{40, 40, 20}, m_signals{128, 128, 100}, m_stop{8, 8, 5};
  TimePoint m_last;
};
class CommandCacheBudget
{
public:
  explicit CommandCacheBudget(uint32_t maximum = 1024);
  uint32_t used() const;

private:
  friend class CommandResultCache;
  bool acquire();
  void release();
  mutable std::mutex m_mutex;
  uint32_t m_maximum, m_used = 0;
};
enum class CommandCompletion
{
  Accepted,
  Rejected,
  Cancelled,
  Stopped,
  Failed
};
struct ControlCommandResult
{
  CommandCompletion completion = CommandCompletion::Rejected;
  QString code, resultRef;
  bool cleanupVerified = false;
  bool operator==(const ControlCommandResult &) const = default;
};
enum class CommandDisposition
{
  Execute,
  Pending,
  Replay,
  ParameterConflict,
  Expired,
  OverCapacity,
  Invalid
};
struct CommandLookup
{
  CommandDisposition disposition = CommandDisposition::Invalid;
  std::optional<ControlCommandResult> result;
};
// Result data is a fixed typed summary, never an SDP, credentials or arbitrary
// JSON. Resource/session terminal state must still be checked independently.
class CommandResultCache
{
public:
  explicit CommandResultCache(std::shared_ptr<CommandCacheBudget>, uint32_t capacity = 256);
  ~CommandResultCache();
  CommandResultCache(const CommandResultCache &) = delete;
  CommandResultCache &operator=(const CommandResultCache &) = delete;
  CommandLookup
  begin(const QString &operationId, const QString &parameterDigest, TimePoint now, TimePoint operationDeadline);
  bool finish(const QString &operationId, const QString &parameterDigest, ControlCommandResult, TimePoint now);
  uint32_t size() const
  {
    return uint32_t(m_entries.size());
  }

private:
  struct Entry
  {
    QString digest;
    TimePoint deadline, expires;
    std::optional<ControlCommandResult> result;
    bool expired = false;
  };
  bool acceptTime(TimePoint now);
  void prune(TimePoint now);
  void expire(Entry &, TimePoint now);
  std::shared_ptr<CommandCacheBudget> m_budget;
  uint32_t m_capacity;
  QMap<QString, Entry> m_entries;
  std::optional<TimePoint> m_last;
};
// Excludes transport sequence/message ID, includes actual shared connection
// binding plus all semantic command fields. Body must already pass its registry.
QString commandParameterDigest(const Envelope &, const QString &controlBinding);
} // namespace deskflow::display::infra
