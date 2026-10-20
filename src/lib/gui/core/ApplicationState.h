// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once

#include "common/Enums.h"
#include <QDateTime>
#include <QMap>
#include <QObject>
#include <QUuid>

namespace deskflow::gui {

// Shared by the workspace, diagnostics and system menu. Only observed core
// events may update transport state; saving never implies a connection.
class ApplicationState : public QObject
{
  Q_OBJECT
public:
  struct Session
  {
    QUuid attempt;
    bool connected = false;
    QDateTime manuallyVerifiedAt;
    QUuid verificationAttempt;
  };
  explicit ApplicationState(QObject *parent = nullptr) : QObject(parent)
  {
  }
  quint64 savedRevision() const
  {
    return m_savedRevision;
  }
  quint64 activeRevision() const
  {
    return m_activeRevision;
  }
  bool pending() const
  {
    return m_saved != m_active;
  }
  bool applying() const
  {
    return m_applying != 0;
  }
  QString error() const
  {
    return m_error;
  }
  const QMap<QString, Session> &sessions() const
  {
    return m_sessions;
  }
  void initialize(const QByteArray &snapshot);
  void restore(const QByteArray &saved, const QByteArray &active, quint64 savedRevision, quint64 activeRevision);
  QByteArray savedSnapshot() const
  {
    return m_saved;
  }
  QByteArray activeSnapshot() const
  {
    return m_active;
  }
  void persisted(const QByteArray &snapshot);
  quint64 beginApply();
  void finishApply(quint64 revision, bool success, const QString &reason = {});
  QUuid beginSession(const QString &device);
  void sessionConnected(const QString &device, const QUuid &attempt, bool connected);
  bool verifyManually(const QString &device);
  void disconnectAll();

Q_SIGNALS:
  void changed();

private:
  QByteArray m_saved, m_active, m_applyingSnapshot;
  quint64 m_savedRevision = 1, m_activeRevision = 1, m_applying = 0;
  QString m_error;
  QMap<QString, Session> m_sessions;
};
} // namespace deskflow::gui
