// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "SourceChangeTransaction.h"
namespace deskflow::gui {
QUuid SourceChangeTransaction::begin(const QString &original, const QString &candidate)
{
  if (active() || original.isEmpty() || candidate.isEmpty() || original == candidate)
    return {};
  m_original = original;
  m_candidate = candidate;
  m_attempt = QUuid::createUuid();
  m_phase = Phase::Candidate;
  m_ready = m_connected = false;
  m_failure.clear();
  Q_EMIT changed();
  return m_attempt;
}
void SourceChangeTransaction::runtimeReady(const QUuid &attempt, const QString &source)
{
  if (!active() || attempt != m_attempt || source != expectedSource())
    return;
  m_ready = true;
  completeIfReady();
}
void SourceChangeTransaction::connectionObserved(const QUuid &attempt, const QString &source, bool connected)
{
  if (!active() || attempt != m_attempt || source != expectedSource())
    return;
  m_connected = connected;
  completeIfReady();
}
void SourceChangeTransaction::completeIfReady()
{
  if (!m_ready || !m_connected)
    return;
  const bool accepted = m_phase == Phase::Candidate;
  m_phase = Phase::Complete;
  Q_EMIT changed();
  Q_EMIT finished(accepted, !accepted, m_failure);
}
void SourceChangeTransaction::failed(const QUuid &attempt, const QString &source, const QString &reason)
{
  if (!active() || attempt != m_attempt || source != expectedSource())
    return;
  if (m_phase == Phase::Candidate) {
    m_failure = reason;
    m_phase = Phase::Restoring;
    m_ready = m_connected = false;
    Q_EMIT changed();
    Q_EMIT restoreRequested();
  } else {
    m_phase = Phase::Failed;
    Q_EMIT changed();
    Q_EMIT finished(false, false, m_failure + "\n" + reason);
  }
}
void SourceChangeTransaction::abort(const QString &reason)
{
  if (!active())
    return;
  m_phase = Phase::Aborted;
  Q_EMIT changed();
  Q_EMIT finished(false, false, reason);
}
} // namespace deskflow::gui
