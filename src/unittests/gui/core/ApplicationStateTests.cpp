// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "gui/core/ApplicationState.h"
#include <QtTest>
using deskflow::gui::ApplicationState;
class ApplicationStateTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void savedWhileApplyingRemainsPending()
  {
    ApplicationState state;
    state.initialize("A");
    state.persisted("B");
    const auto revision = state.beginApply();
    state.persisted("C");
    state.finishApply(revision, true);
    QCOMPARE(state.activeRevision(), revision);
    QVERIFY(state.pending());
    QVERIFY(state.savedRevision() > state.activeRevision());
  }
  void staleCompletionCannotCommit()
  {
    ApplicationState state;
    state.initialize("A");
    state.persisted("B");
    const auto revision = state.beginApply();
    state.finishApply(revision + 1, true);
    QVERIFY(state.applying());
    QCOMPARE(state.activeRevision(), quint64(1));
    state.finishApply(revision, false, "failed");
    QVERIFY(state.pending());
    QCOMPARE(state.activeRevision(), quint64(1));
    QCOMPARE(state.error(), QString("failed"));
  }
  void canceledSessionIgnoresOldSuccess()
  {
    ApplicationState state;
    const auto old = state.beginSession("peer");
    const auto current = state.beginSession("peer");
    state.sessionConnected("peer", old, true);
    QVERIFY(!state.sessions().value("peer").connected);
    QVERIFY(!state.verifyManually("peer"));
    state.sessionConnected("peer", current, true);
    QVERIFY(state.verifyManually("peer"));
    state.disconnectAll();
    QVERIFY(!state.sessions().value("peer").connected);
    QVERIFY(state.sessions().value("peer").manuallyVerifiedAt.isValid());
  }
  void restoredPendingStateDoesNotBecomeAppliedOnRestart()
  {
    ApplicationState state;
    state.restore("saved", "active", 7, 5);
    QVERIFY(state.pending());
    QCOMPARE(state.savedRevision(), quint64(7));
    QCOMPARE(state.activeRevision(), quint64(5));
    const auto revision = state.beginApply();
    state.finishApply(revision, true);
    QVERIFY(!state.pending());
    QCOMPARE(state.activeRevision(), quint64(7));
  }
  void equivalentSaveDoesNotCreateRevision()
  {
    ApplicationState state;
    state.initialize("A");
    state.persisted("A");
    QVERIFY(!state.pending());
    QCOMPARE(state.savedRevision(), quint64(1));
  }
};
QTEST_MAIN(ApplicationStateTests)
#include "ApplicationStateTests.moc"
