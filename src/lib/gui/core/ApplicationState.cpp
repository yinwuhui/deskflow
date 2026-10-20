// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "ApplicationState.h"

namespace deskflow::gui {
void ApplicationState::initialize(const QByteArray &snapshot)
{
  m_saved = m_active = snapshot;
  Q_EMIT changed();
}
void ApplicationState::restore(
    const QByteArray &saved, const QByteArray &active, quint64 savedRevision, quint64 activeRevision
)
{
  m_saved = saved;
  m_active = active;
  m_savedRevision = std::max(quint64(1), savedRevision);
  m_activeRevision = activeRevision;
  Q_EMIT changed();
}
void ApplicationState::persisted(const QByteArray &snapshot)
{
  if (snapshot == m_saved)
    return;
  m_saved = snapshot;
  ++m_savedRevision;
  Q_EMIT changed();
}
quint64 ApplicationState::beginApply()
{
  if (applying())
    return 0;
  m_applying = m_savedRevision;
  m_applyingSnapshot = m_saved;
  m_error.clear();
  Q_EMIT changed();
  return m_applying;
}
void ApplicationState::finishApply(quint64 revision, bool success, const QString &reason)
{
  if (!m_applying || revision != m_applying)
    return;
  if (success) {
    m_active = m_applyingSnapshot;
    m_activeRevision = revision;
  }
  m_error = success ? QString() : reason;
  m_applying = 0;
  m_applyingSnapshot.clear();
  Q_EMIT changed();
}
QUuid ApplicationState::beginSession(const QString &device)
{
  auto &session = m_sessions[device];
  session.attempt = QUuid::createUuid();
  session.connected = false;
  Q_EMIT changed();
  return session.attempt;
}
void ApplicationState::sessionConnected(const QString &device, const QUuid &attempt, bool connected)
{
  auto it = m_sessions.find(device);
  if (it == m_sessions.end() || it->attempt != attempt)
    return;
  it->connected = connected;
  Q_EMIT changed();
}
bool ApplicationState::verifyManually(const QString &device)
{
  auto it = m_sessions.find(device);
  if (it == m_sessions.end() || !it->connected)
    return false;
  it->manuallyVerifiedAt = QDateTime::currentDateTimeUtc();
  it->verificationAttempt = it->attempt;
  Q_EMIT changed();
  return true;
}
void ApplicationState::disconnectAll()
{
  for (auto &session : m_sessions)
    session.connected = false;
  Q_EMIT changed();
}
} // namespace deskflow::gui
