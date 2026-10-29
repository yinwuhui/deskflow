// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/core/SessionMailbox.h"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace deskflow::display::infra;
using namespace std::chrono_literals;
static void require(bool value, const char *why)
{
  if (!value)
    throw std::runtime_error(why);
}
struct Fixture
{
  TimePoint now{100s};
  InfrastructureSession session;
  const std::string worker = std::string(32, '3');
  SessionToken token() const
  {
    return session.snapshot().token;
  }
  Fixture()
  {
    require(
        session.reserve(
            {std::string(64, 'a'), std::string(64, 'b'), std::string(64, 'e'), std::string(32, '1'),
             std::string(32, '2'), Direction::Send, 1},
            now
        ),
        "reserve"
    );
  }
  void commit()
  {
    CapabilitySnapshot a, b;
    a.peerIdentity = std::string(64, 'a');
    b.peerIdentity = std::string(64, 'b');
    a.revision = b.revision = 1;
    a.environment = std::string(64, 'c');
    b.environment = std::string(64, 'd');
    a.directNetworkAllowed = b.directNetworkAllowed = a.sessionAvailable = b.sessionAvailable = a.cleanupVerified =
        b.cleanupVerified = true;
    PipelineProfile p;
    p.id = "test.mailbox.sender";
    p.implementation = "test";
    p.implementationVersion = "1";
    p.environment = a.environment;
    p.readiness = Readiness::Available;
    p.security = {SecurityProfile::WebRtcAuthenticatedDtlsSrtp};
    p.formats = {VideoFormat{}};
    p.rateFeedback = p.refreshFeedback = p.presentationEvidence = p.verifiedCleanup = true;
    a.pipelines = {p};
    p.id = "test.mailbox.receiver";
    p.environment = b.environment;
    p.direction = Direction::Receive;
    b.pipelines = {p};
    SelectionPolicy selection;
    selection.allowUncertifiedDiagnostics = true;
    Requirements requirements;
    SessionPlan selected{
        std::string(64, 'e'),
        token().session,
        1,
        a.peerIdentity,
        b.peerIdentity,
        1,
        1,
        plan(a, b, requirements, selection).candidates.front(),
        std::string(64, 'f'),
        {}
    };
    require(session.beginNegotiation(token(), now), "negotiate");
    require(
        session.acceptPlan(token(), selected, std::string(64, 'a'), a, b, requirements, selection, now) ==
            Rejection::None,
        "plan"
    );
    require(session.commit(token(), {true, true, true, true, true, now + 60s}, now), "commit");
    auto effects = session.takeEffects();
    require(effects.size() == 1 && effects[0].kind == EffectKind::PersistIntent, "dispatched intent");
  }
  void preparing()
  {
    commit();
    require(session.intentPersisted(token(), true, now), "intent");
    auto e = session.takeEffects();
    require(e.size() == 1 && e[0].kind == EffectKind::PrepareBackend, "dispatched spawn");
  }
  void awaiting()
  {
    preparing();
    require(session.attachWorker(token(), worker, now), "attach");
    require(session.prepared(token(), now), "prepared");
    session.takeEffects();
  }
  void running()
  {
    awaiting();
    require(session.presented(token(), PresentationEvidence::NativePaintConfirmed, true, now), "presented");
  }
};
static void priorityFlood()
{
  Fixture f;
  f.awaiting();
  SessionMailbox queue(f.session);
  const auto t = f.token();
  for (uint64_t i = 1; i <= 10000; ++i)
    require(
        queue.post({t, MailboxStatistics{i, i, i, i, 1024}}) != MailboxPost::Full,
        "statistics consumed ordinary capacity"
    );
  for (uint64_t i = 1; i <= 64; ++i)
    require(
        queue.post({t, MailboxPresented{i, PresentationEvidence::NativePaintConfirmed, true}}) == MailboxPost::Queued,
        "ordinary capacity"
    );
  require(queue.post({t, MailboxStop{}}) == MailboxPost::Queued, "stop reserved");
  require(queue.post({t, MailboxRevoked{}}) == MailboxPost::Queued, "revoke reserved");
  require(queue.post({t, MailboxDisconnected{}}) == MailboxPost::Queued, "disconnect reserved");
  require(queue.post({t, MailboxWorkerExited{f.worker}}) == MailboxPost::Queued, "exit reserved");
  for (int i = 0; i < 10000; ++i)
    require(queue.post({t, MailboxRevoked{}}) == MailboxPost::Coalesced, "revoke duplicates grew queue");
  const auto used = queue.usage();
  require(used.ordinary == 64 && used.critical == 4 && used.statistics && !used.overflow, "queue bounds");
  require(f.session.snapshot().state == Lifecycle::AwaitingFirstFrame, "producer mutated lifecycle");
  const auto r = queue.drain(f.now, 8);
  require(r.ownerAccepted && r.applied == 4 && r.ignored == 9 && !r.statistics, "critical order/batch/statistics");
  require(queue.usage().ordinary == 56, "unbounded batch");
  require(
      f.session.snapshot().state == Lifecycle::VerifyingCleanup &&
          f.session.snapshot().primaryError == "AuthorizationRevoked",
      "late frame overrode revoke/exit"
  );
  for (const auto &e : f.session.takeEffects())
    require(e.kind != EffectKind::StartMedia, "stale frame restarted media");
}
static void lateIntent()
{
  Fixture f;
  f.commit();
  SessionMailbox queue(f.session);
  const auto t = f.token();
  require(queue.post({t, MailboxIntentCompleted{true}}) == MailboxPost::Queued, "intent post");
  queue.post({t, MailboxStop{}});
  auto r = queue.drain(f.now);
  require(r.applied == 2 && f.session.snapshot().state == Lifecycle::VerifyingCleanup, "late durable intent lost");
  const auto e = f.session.takeEffects();
  require(e.size() == 1 && e[0].kind == EffectKind::DestroyResources, "cancel started backend");
  queue.post({t, MailboxResourcesDestroyed{}});
  queue.post({t, MailboxCleanupVerified{true}});
  r = queue.drain(f.now + 1ms);
  require(
      r.applied == 2 && f.session.snapshot().state == Lifecycle::Idle && f.session.snapshot().cleanupVerified,
      "original owner cleanup rejected after generation change"
  );
}
static void exitBeforeAttach()
{
  Fixture f;
  f.preparing();
  SessionMailbox queue(f.session);
  const auto t = f.token();
  queue.post({t, MailboxWorkerAttached{f.worker}});
  queue.post({t, MailboxWorkerExited{f.worker}});
  auto r = queue.drain(f.now, 0);
  require(
      r.applied == 1 && f.session.snapshot().token.worker == f.worker &&
          f.session.snapshot().state == Lifecycle::VerifyingCleanup,
      "exit overtaking attach lost original instance"
  );
  require(f.session.snapshot().primaryError == "WorkerCrashed", "exit cause");
  r = queue.drain(f.now + 1ms);
  require(
      r.ignored == 1 && f.session.snapshot().state == Lifecycle::VerifyingCleanup,
      "late attach resurrected exited worker"
  );
  for (const auto &e : f.session.takeEffects())
    require(e.kind != EffectKind::StartMedia, "exited worker started");
}
static void overflowRetry()
{
  Fixture f;
  f.commit();
  SessionMailbox queue(f.session, 1);
  const auto t = f.token();
  queue.post({t, MailboxRecovered{}});
  require(queue.post({t, MailboxIntentCompleted{true}}) == MailboxPost::Full, "ordinary overflow unreported");
  auto r = queue.drain(f.now);
  require(r.overflowStop && f.session.snapshot().state == Lifecycle::Stopping, "overflow skipped in-flight journal");
  require(f.session.snapshot().primaryError == "OwnerMailboxOverflow", "overflow cause hidden");
  require(queue.post({t, MailboxIntentCompleted{true}}) == MailboxPost::Queued, "completion retry rejected");
  r = queue.drain(f.now + 1ms);
  require(
      r.applied == 1 && f.session.snapshot().state == Lifecycle::VerifyingCleanup,
      "retry failed to preserve cleanup ownership"
  );
  const auto e = f.session.takeEffects();
  require(e.size() == 1 && e[0].kind == EffectKind::DestroyResources, "overflow continued creation");
}
static void evidenceAndStatistics()
{
  Fixture f;
  f.awaiting();
  SessionMailbox queue(f.session);
  const auto t = f.token();
  queue.post({t, MailboxStatistics{100, 1000, 900, 0, 42}});
  queue.post({t, MailboxStatistics{99, 999, 899, 0, 42}});
  auto r = queue.drain(f.now);
  require(
      r.statistics && std::get<MailboxStatistics>(r.statistics->payload).sequence == 100,
      "stale pending statistics replaced newer"
  );
  require(f.session.snapshot().state == Lifecycle::AwaitingFirstFrame, "statistics activated session");
  queue.post({t, MailboxStatistics{99, 999, 899, 0, 42}});
  queue.post({t, MailboxPresented{1, PresentationEvidence::Decoded, true}});
  queue.post({t, MailboxPresented{2, PresentationEvidence::NativePaintConfirmed, false}});
  r = queue.drain(f.now + 1ms);
  require(!r.statistics && r.applied == 0 && r.ignored == 3, "replay/decode/hidden evidence accepted");
  queue.post({t, MailboxPresented{3, PresentationEvidence::NativePaintConfirmed, true}});
  queue.post({t, MailboxPresented{2, PresentationEvidence::NativePaintConfirmed, true}});
  r = queue.drain(f.now + 2ms);
  require(r.applied == 1 && r.ignored == 1 && f.session.snapshot().state == Lifecycle::Streaming, "presentation order");
  auto stale = t;
  --stale.generation;
  require(queue.post({stale, MailboxPrepared{true}}) == MailboxPost::Invalid, "invalid generation accepted");
}
static void threadAndScope()
{
  Fixture f;
  f.running();
  SessionMailbox queue(f.session);
  const auto t = f.token();
  std::array<std::thread, 4> producers;
  for (size_t n = 0; n < producers.size(); ++n)
    producers[n] = std::thread([&, n] {
      for (uint64_t i = 1; i <= 1000; ++i)
        queue.post({t, MailboxStatistics{uint64_t(n) * 1000 + i, i, i, 0, 16}});
    });
  for (auto &p : producers)
    p.join();
  MailboxDrain foreign;
  std::thread wrongOwner([&] { foreign = queue.drain(f.now); });
  wrongOwner.join();
  require(!foreign.ownerAccepted && queue.usage().statistics, "foreign thread drained mailbox");
  auto wrong = t;
  wrong.owner = std::string(32, '9');
  require(queue.post({wrong, MailboxRevoked{}}) == MailboxPost::WrongScope, "wrong owner admitted");
  auto invalid = t;
  invalid.worker = std::string(5000, 'a');
  require(queue.post({invalid, MailboxStatistics{1}}) == MailboxPost::Invalid, "unbounded identity admitted");
  require(queue.post({t, MailboxStop{MailboxStopReason(99)}}) == MailboxPost::Invalid, "unknown stop enum");
  auto r = queue.drain(f.now);
  require(
      r.statistics && std::get<MailboxStatistics>(r.statistics->payload).sequence == 4000,
      "concurrent coalescing lost newest"
  );
  require(!queue.drain(f.now - 1ms).ownerAccepted, "time moved backward");
  require(f.session.snapshot().state == Lifecycle::Streaming, "foreign work mutated core");
  queue.post({t, MailboxWorkerExited{std::string(32, '9')}});
  r = queue.drain(f.now + 1ms);
  require(
      r.ignored == 1 && f.session.snapshot().state == Lifecycle::Streaming, "wrong worker exit stopped current worker"
  );
}
static void retiredScope()
{
  Fixture f;
  f.running();
  SessionMailbox queue(f.session);
  const auto old = f.token();
  queue.post({old, MailboxStop{}});
  queue.post({old, MailboxWorkerQuiesced{}});
  queue.post({old, MailboxResourcesDestroyed{}});
  queue.post({old, MailboxCleanupVerified{true}});
  queue.drain(f.now);
  f.session.takeEffects();
  require(f.session.snapshot().state == Lifecycle::Idle, "old owner cleanup");
  require(
      f.session.reserve(
          {std::string(64, 'a'), std::string(64, 'b'), std::string(64, 'e'), std::string(32, '7'), std::string(32, '8'),
           Direction::Send, 2},
          f.now
      ),
      "new reservation"
  );
  const auto revision = f.session.snapshot().revision;
  queue.post({old, MailboxRevoked{}});
  queue.drain(f.now + 60s);
  require(
      f.session.snapshot().state == Lifecycle::Reserved && f.session.snapshot().revision == revision,
      "retired mailbox tick/revoke affected new scope"
  );
  require(queue.post({f.token(), MailboxStop{}}) == MailboxPost::WrongScope, "retired mailbox rebound implicitly");
}
int main(int argc, char **argv)
{
  const std::vector<std::pair<std::string, std::function<void()>>> cases{
      {"MailboxPriorityFlood", priorityFlood},
      {"MailboxLateIntent", lateIntent},
      {"MailboxExitBeforeAttach", exitBeforeAttach},
      {"MailboxOverflowRetry", overflowRetry},
      {"MailboxEvidenceStatistics", evidenceAndStatistics},
      {"MailboxThreadScope", threadAndScope},
      {"MailboxRetiredScope", retiredScope}
  };
  try {
    require(argc == 2, "one case required");
    for (const auto &[name, run] : cases)
      if (name == argv[1]) {
        run();
        return 0;
      }
    throw std::runtime_error("unknown case");
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
