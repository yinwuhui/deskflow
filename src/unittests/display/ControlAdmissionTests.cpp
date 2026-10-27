// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/control/ControlAdmission.h"
#include <QCoreApplication>
#include <iostream>
#include <stdexcept>
using namespace deskflow::display::infra;
static void require(bool b, const char *why)
{
  if (!b)
    throw std::runtime_error(why);
}
static const QString op(32, '1'), digest(64, 'a');
static void rates()
{
  TimePoint now{};
  ControlRateLimiter rate(now);
  for (int i = 0; i < 40; ++i)
    require(rate.admit(ControlTraffic::Command, now), "command burst too small");
  require(!rate.admit(ControlTraffic::Command, now), "unbounded command burst");
  for (int i = 0; i < 128; ++i)
    require(rate.admit(ControlTraffic::Signal, now), "signalling burst too small");
  require(!rate.admit(ControlTraffic::Signal, now), "unbounded signals");
  for (int i = 0; i < 8; ++i)
    require(rate.admit(ControlTraffic::StopOrRevoke, now), "stop reserved quota consumed by ordinary traffic");
  require(!rate.admit(ControlTraffic::StopOrRevoke, now), "unbounded stop flood");
  now += std::chrono::milliseconds(50);
  require(
      rate.admit(ControlTraffic::Command, now) && !rate.admit(ControlTraffic::Command, now),
      "20 commands per second refill"
  );
  require(!rate.admit(ControlTraffic::Command, TimePoint{}), "clock rollback accepted");
}
static void dedup()
{
  auto budget = std::make_shared<CommandCacheBudget>();
  CommandResultCache c(budget);
  TimePoint now{};
  require(
      c.begin(op, digest, now, now + std::chrono::seconds(10)).disposition == CommandDisposition::Execute,
      "new command denied"
  );
  require(
      c.begin(op, digest, now, now + std::chrono::seconds(15)).disposition == CommandDisposition::Pending,
      "pending command duplicated"
  );
  require(
      c.begin(op, QString(64, 'b'), now, now + std::chrono::seconds(10)).disposition ==
          CommandDisposition::ParameterConflict,
      "operation ID rebound"
  );
  const ControlCommandResult result{CommandCompletion::Stopped, "Stopped", "owner-1", true};
  require(c.finish(op, digest, result, now) && c.finish(op, digest, result, now), "completion not idempotent");
  require(!c.finish(op, digest, {CommandCompletion::Accepted, {}, "other", false}, now), "completed result replaced");
  const auto replay = c.begin(op, digest, now, now + std::chrono::seconds(10));
  require(replay.disposition == CommandDisposition::Replay && replay.result == result, "cached result lost");
}
static void absolute()
{
  auto budget = std::make_shared<CommandCacheBudget>();
  CommandResultCache c(budget);
  TimePoint now{};
  c.begin(op, digest, now, now + std::chrono::seconds(1));
  now += std::chrono::milliseconds(900);
  require(
      c.begin(op, digest, now, now + std::chrono::seconds(15)).disposition == CommandDisposition::Pending,
      "pending deadline changed early"
  );
  now += std::chrono::milliseconds(100);
  const auto expired = c.begin(op, digest, now, now + std::chrono::seconds(15));
  require(
      expired.disposition == CommandDisposition::Expired && expired.result->code == "OperationDeadline",
      "duplicate extended deadline"
  );
  require(
      !c.finish(op, digest, {CommandCompletion::Accepted, "Active", {}, false}, now),
      "late success revived expired operation"
  );
  require(
      c.finish(op, digest, {CommandCompletion::Stopped, "Stopped", {}, true}, now), "late cleanup evidence rejected"
  );
  const auto cleanup = c.begin(op, digest, now, now + std::chrono::seconds(1));
  require(
      cleanup.disposition == CommandDisposition::Expired && cleanup.result->cleanupVerified &&
          cleanup.result->code == "OperationDeadline",
      "cleanup overwrote primary timeout"
  );
}
static void capacity()
{
  auto budget = std::make_shared<CommandCacheBudget>(2);
  TimePoint now{};
  {
    CommandResultCache first(budget, 1), second(budget, 2);
    require(
        first.begin(op, digest, now, now + std::chrono::seconds(1)).disposition == CommandDisposition::Execute,
        "first budget"
    );
    require(
        first.begin(QString(32, '2'), digest, now, now + std::chrono::seconds(1)).disposition ==
            CommandDisposition::OverCapacity,
        "peer cap ignored"
    );
    require(
        second.begin(op, digest, now, now + std::chrono::seconds(1)).disposition == CommandDisposition::Execute,
        "second peer budget"
    );
    require(
        second.begin(QString(32, '2'), digest, now, now + std::chrono::seconds(1)).disposition ==
            CommandDisposition::OverCapacity,
        "global cap ignored"
    );
    require(budget->used() == 2, "global accounting");
  }
  require(budget->used() == 0, "cache destructor leaked budget");
}
static void expiry()
{
  auto budget = std::make_shared<CommandCacheBudget>(1);
  CommandResultCache c(budget, 1);
  TimePoint now{};
  c.begin(op, digest, now, now + std::chrono::seconds(1));
  now += std::chrono::minutes(9);
  require(
      c.begin(op, digest, now, now + std::chrono::seconds(1)).disposition == CommandDisposition::Expired,
      "expired pending lost tombstone"
  );
  now += std::chrono::minutes(1);
  require(
      c.begin(QString(32, '2'), digest, now, now + std::chrono::seconds(1)).disposition ==
              CommandDisposition::Execute &&
          budget->used() == 1,
      "fixed ten minute retention or budget release failed"
  );
  require(!c.finish(QString(32, '2'), digest, {}, TimePoint{}), "cache clock rollback accepted");
}
static void semanticDigest()
{
  Envelope e;
  e.type = "PlanCommit";
  e.sessionId = QString(32, '2');
  e.attempt = 1;
  e.planHash = QString(64, 'c');
  e.body = {{"operationId", op}, {"reference", "plan-1"}};
  const QString binding(64, 'b');
  const auto original = commandParameterDigest(e, binding);
  require(original.size() == 64, "missing parameter digest");
  e.messageId = QString(32, '3');
  e.sequence = 99;
  require(commandParameterDigest(e, binding) == original, "transport replay metadata changed semantic digest");
  require(commandParameterDigest(e, QString(64, 'd')) != original, "connection binding not hashed");
  e.body.insert("reference", "plan-2");
  require(commandParameterDigest(e, binding) != original, "parameter change not hashed");
}
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  try {
    require(argc == 2, "case required");
    const std::string c = argv[1];
    if (c == "ControlRateReservations")
      rates();
    else if (c == "CommandDeduplication")
      dedup();
    else if (c == "CommandAbsoluteDeadline")
      absolute();
    else if (c == "CommandGlobalBudget")
      capacity();
    else if (c == "CommandCacheExpiry")
      expiry();
    else if (c == "CommandSemanticDigest")
      semanticDigest();
    else
      throw std::runtime_error("unknown case");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
