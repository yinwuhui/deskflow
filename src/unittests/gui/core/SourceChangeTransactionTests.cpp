// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "gui/core/SourceChangeTransaction.h"
#include <QtTest>
using deskflow::gui::SourceChangeTransaction;
class SourceChangeTransactionTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void acceptsOnlyReadyAndConnected()
  {
    SourceChangeTransaction state;
    QSignalSpy finished(&state, &SourceChangeTransaction::finished);
    const auto id = state.begin("old", "new");
    state.runtimeReady(id, "new");
    QVERIFY(state.active());
    state.connectionObserved(id, "old", true);
    state.connectionObserved(QUuid::createUuid(), "new", true);
    QVERIFY(finished.isEmpty());
    state.connectionObserved(id, "new", true);
    QCOMPARE(finished.size(), 1);
    QVERIFY(finished[0][0].toBool());
    QVERIFY(!finished[0][1].toBool());
  }
  void failureRequiresConfirmedRecovery()
  {
    SourceChangeTransaction state;
    QSignalSpy restore(&state, &SourceChangeTransaction::restoreRequested);
    QSignalSpy finished(&state, &SourceChangeTransaction::finished);
    const auto id = state.begin("old", "new");
    state.failed(id, "new", "timeout");
    QCOMPARE(restore.size(), 1);
    QVERIFY(finished.isEmpty());
    state.runtimeReady(id, "new"); // late candidate success cannot commit
    state.connectionObserved(id, "new", true);
    QVERIFY(finished.isEmpty());
    state.connectionObserved(id, "old", true);
    QVERIFY(finished.isEmpty());
    state.runtimeReady(id, "old");
    QCOMPARE(finished.size(), 1);
    QVERIFY(!finished[0][0].toBool());
    QVERIFY(finished[0][1].toBool());
  }
  void failedRecoveryNeverClaimsOriginalRestored()
  {
    SourceChangeTransaction state;
    QSignalSpy finished(&state, &SourceChangeTransaction::finished);
    const auto id = state.begin("old", "new");
    state.failed(id, "new", "candidate failed");
    state.failed(id, "old", "original unavailable");
    QCOMPARE(state.phase(), SourceChangeTransaction::Phase::Failed);
    QCOMPARE(finished.size(), 1);
    QVERIFY(!finished[0][0].toBool());
    QVERIFY(!finished[0][1].toBool());
    QVERIFY(finished[0][2].toString().contains("original unavailable"));
  }
  void stopAbortsWithoutRestartAndRejectsStaleAttempt()
  {
    SourceChangeTransaction state;
    QSignalSpy restore(&state, &SourceChangeTransaction::restoreRequested);
    QSignalSpy finished(&state, &SourceChangeTransaction::finished);
    const auto old = state.begin("old", "new");
    state.abort("stop all");
    state.runtimeReady(old, "new");
    state.connectionObserved(old, "new", true);
    QVERIFY(restore.isEmpty());
    QCOMPARE(finished.size(), 1);
    const auto next = state.begin("old", "next");
    state.failed(old, "next", "stale");
    QCOMPARE(state.phase(), SourceChangeTransaction::Phase::Candidate);
    QVERIFY(next != old);
  }
};
QTEST_MAIN(SourceChangeTransactionTests)
#include "SourceChangeTransactionTests.moc"
