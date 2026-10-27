// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "ControlAdmission.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
namespace deskflow::display::infra {
ControlRateLimiter::ControlRateLimiter(TimePoint now) : m_last(now)
{
}
bool ControlRateLimiter::admit(ControlTraffic traffic, TimePoint now)
{
  if (now < m_last)
    return false;
  const auto elapsed = std::chrono::duration<double>(now - m_last).count();
  if (!std::isfinite(elapsed))
    return false;
  for (auto *b : {&m_commands, &m_signals, &m_stop})
    b->tokens = std::min(b->capacity, b->tokens + elapsed * b->rate);
  m_last = now;
  Bucket *selected = nullptr;
  switch (traffic) {
  case ControlTraffic::Command:
    selected = &m_commands;
    break;
  case ControlTraffic::Signal:
    selected = &m_signals;
    break;
  case ControlTraffic::StopOrRevoke:
    selected = &m_stop;
    break;
  default:
    return false;
  }
  if (selected->tokens < 1)
    return false;
  selected->tokens -= 1;
  return true;
}
CommandCacheBudget::CommandCacheBudget(uint32_t maximum) : m_maximum(maximum <= 1024 ? maximum : 0)
{
}
uint32_t CommandCacheBudget::used() const
{
  std::lock_guard lock(m_mutex);
  return m_used;
}
bool CommandCacheBudget::acquire()
{
  std::lock_guard lock(m_mutex);
  if (m_used >= m_maximum)
    return false;
  ++m_used;
  return true;
}
void CommandCacheBudget::release()
{
  std::lock_guard lock(m_mutex);
  if (m_used)
    --m_used;
}
CommandResultCache::CommandResultCache(std::shared_ptr<CommandCacheBudget> budget, uint32_t capacity)
    : m_budget(std::move(budget)),
      m_capacity(capacity <= 256 ? capacity : 0)
{
}
CommandResultCache::~CommandResultCache()
{
  if (m_budget)
    for (qsizetype i = 0; i < m_entries.size(); ++i)
      m_budget->release();
}
bool CommandResultCache::acceptTime(TimePoint now)
{
  if (m_last && now < *m_last)
    return false;
  m_last = now;
  return true;
}
void CommandResultCache::prune(TimePoint now)
{
  for (auto i = m_entries.begin(); i != m_entries.end();) {
    if (now >= i->expires) {
      i = m_entries.erase(i);
      m_budget->release();
    } else
      ++i;
  }
}
void CommandResultCache::expire(Entry &e, TimePoint now)
{
  if (!e.result && now >= e.deadline) {
    e.expired = true;
    e.result = ControlCommandResult{CommandCompletion::Cancelled, "OperationDeadline", {}, false};
  }
}
CommandLookup
CommandResultCache::begin(const QString &operation, const QString &digest, TimePoint now, TimePoint deadline)
{
  if (!m_budget || !m_capacity || !acceptTime(now) || !validId(operation.toStdString(), 16) ||
      !validId(digest.toStdString(), 32))
    return {};
  prune(now);
  auto i = m_entries.find(operation);
  if (i != m_entries.end()) {
    if (i->digest != digest)
      return {CommandDisposition::ParameterConflict, {}};
    expire(i.value(), now);
    return {
        i->expired ? CommandDisposition::Expired
                   : (i->result ? CommandDisposition::Replay : CommandDisposition::Pending),
        i->result
    };
  }
  if (deadline <= now || deadline > now + std::chrono::seconds(15))
    return {};
  if (m_entries.size() >= m_capacity || !m_budget->acquire())
    return {CommandDisposition::OverCapacity, {}};
  m_entries.insert(operation, Entry{digest, deadline, now + std::chrono::minutes(10), {}, false});
  return {CommandDisposition::Execute, {}};
}
bool CommandResultCache::finish(
    const QString &operation, const QString &digest, ControlCommandResult result, TimePoint now
)
{
  if (!acceptTime(now))
    return false;
  auto i = m_entries.find(operation);
  if (i == m_entries.end() || i->digest != digest || now >= i->expires || int(result.completion) < 0 ||
      int(result.completion) > int(CommandCompletion::Failed) ||
      (!result.code.isEmpty() && !validName(result.code.toStdString(), 64)) ||
      (!result.resultRef.isEmpty() && !validName(result.resultRef.toStdString(), 128)))
    return false;
  expire(i.value(), now);
  if (i->expired) {
    // A late cleanup can improve evidence but must not turn the expired action
    // into success. New media actions still require the lifecycle's current token.
    if (result.completion == CommandCompletion::Stopped && result.cleanupVerified) {
      i->result->cleanupVerified = true;
      return true;
    }
    return false;
  }
  if (i->result)
    return *i->result == result;
  i->result = std::move(result);
  return true;
}
QString commandParameterDigest(const Envelope &e, const QString &binding)
{
  if (!validId(binding.toStdString(), 32) || !validName(e.type.toStdString()) || e.body.size() > 32)
    return {};
  const QJsonObject object{{"binding", binding},     {"type", e.type},
                           {"session", e.sessionId}, {"attempt", QString::number(e.attempt)},
                           {"planHash", e.planHash}, {"body", e.body}};
  const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  ControlError error;
  if (!parseControlObject(bytes, error))
    return {};
  return QString::fromLatin1(
      QCryptographicHash::hash(QByteArray("display-command-parameters/1\0", 29) + bytes, QCryptographicHash::Sha256)
          .toHex()
  );
}
} // namespace deskflow::display::infra
