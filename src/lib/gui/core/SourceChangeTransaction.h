// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QObject>
#include <QUuid>
namespace deskflow::gui {
class SourceChangeTransaction : public QObject
{
  Q_OBJECT
public:
  enum class Phase
  {
    Idle,
    Candidate,
    Restoring,
    Complete,
    Failed,
    Aborted
  };
  explicit SourceChangeTransaction(QObject *parent = nullptr) : QObject(parent)
  {
  }
  QUuid begin(const QString &original, const QString &candidate);
  bool active() const
  {
    return m_phase == Phase::Candidate || m_phase == Phase::Restoring;
  }
  Phase phase() const
  {
    return m_phase;
  }
  QUuid attempt() const
  {
    return m_attempt;
  }
  QString expectedSource() const
  {
    return m_phase == Phase::Candidate ? m_candidate : m_original;
  }
  void runtimeReady(const QUuid &attempt, const QString &source);
  void connectionObserved(const QUuid &attempt, const QString &source, bool connected);
  void failed(const QUuid &attempt, const QString &source, const QString &reason);
  void abort(const QString &reason);
Q_SIGNALS:
  void restoreRequested();
  void finished(bool candidateAccepted, bool originalRestored, const QString &reason);
  void changed();

private:
  void completeIfReady();
  Phase m_phase = Phase::Idle;
  QUuid m_attempt;
  QString m_original, m_candidate, m_failure;
  bool m_ready = false, m_connected = false;
};
} // namespace deskflow::gui
