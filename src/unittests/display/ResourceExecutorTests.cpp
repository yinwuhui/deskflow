// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/resource/ResourceExecutor.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUuid>
#include <iostream>
#include <stdexcept>
using namespace deskflow::display::infra;
static void require(bool v, const char *why)
{
  if (!v)
    throw std::runtime_error(why);
}
static QString id()
{
  return QUuid::createUuid().toString(QUuid::Id128);
}
static ResourceRecord record(QString owner = {})
{
  ResourceRecord r;
  r.id = id();
  r.owner = owner.isEmpty() ? id() : owner;
  r.creationIntent = id();
  r.session = QString(32, '1');
  r.planHash = QString(64, 'a');
  r.userSession = "console.user";
  r.bootInstance = "boot1";
  r.provider = "fixture.output";
  r.providerVersion = "1";
  r.kind = "virtualOutput";
  r.verificationPolicy = "owner-and-absence.v1";
  r.evidence = {"unproven", "create-intent", "unknown"};
  return r;
}
static ResourceAuthorization grant(const ResourceRecord &r)
{
  return {
      r.owner,
      r.session,
      r.planHash,
      r.userSession,
      r.bootInstance,
      Clock::now() + std::chrono::seconds(10),
      std::make_shared<std::atomic<bool>>(false)
  };
}
static TimePoint deadline()
{
  return Clock::now() + std::chrono::seconds(10);
}
struct Provider : PlatformResourceProvider
{
  DurableResourceLedger *ledger = nullptr;
  QMap<QString, QString> objects;
  QStringList released;
  int creates = 0;
  bool ambiguous = false, removeOnRelease = true, absenceVerified = true, quiescent = true, topology = true;
  std::function<void()> duringCreate;
  ResourceContextSnapshot snapshot(const ResourceRecord &, TimePoint) override
  {
    return {true, QString(64, 'b')};
  }
  NativeCreation create(const ResourceRecord &r, TimePoint) override
  {
    require(
        ledger && ledger->records().value(r.id).phase == ResourcePhase::Intent, "native create preceded durable intent"
    );
    ++creates;
    const auto ref = "output-" + r.id;
    objects.insert(r.id, ref);
    if (duringCreate)
      duringCreate();
    return {CreationCertainty::Created, ref, {"nativeObjectOwnership", r.owner, "verified"}};
  }
  bool quiesce(const ResourceRecord &, TimePoint) override
  {
    return quiescent;
  }
  NativeInspection inspect(const ResourceRecord &r, TimePoint) override
  {
    if (ambiguous)
      return {Presence::Ambiguous, {}, {"unproven", "ambiguous", "unknown"}, false};
    if (!objects.contains(r.id))
      return {
          Presence::Absent,
          {},
          absenceVerified ? ResourceEvidence{"systemAbsenceCheck", r.owner, "verified"} : ResourceEvidence{},
          false
      };
    return {Presence::Owned, objects.value(r.id), {"nativeObjectOwnership", r.owner, "verified"}, topology};
  }
  bool release(const ResourceRecord &r, const ResourceEvidence &, TimePoint) override
  {
    released.push_back(r.id);
    if (removeOnRelease)
      objects.remove(r.id);
    return true;
  }
};
struct Fixture
{
  QTemporaryDir temp;
  DurableResourceLedger ledger{temp.path() + "/ledger"};
  bool quiesced = true;
  ResourceExecutor executor{ledger, "console.user", "boot1", [this](const QString &) { return quiesced; }};
  std::shared_ptr<Provider> provider = std::make_shared<Provider>();
  Fixture()
  {
    provider->ledger = &ledger;
    require(
        executor.registerProvider(
            {"fixture.output", "1", "owner-and-absence.v1", {"virtualOutput", "capture"}, {}, provider}
        ),
        "register"
    );
  }
};
static void durable()
{
  Fixture f;
  auto r = record();
  auto auth = grant(r);
  const auto result = f.executor.create(r, auth, deadline());
  require(result.outcome == ResourceOutcome::Active, "create not active");
  require(
      f.provider->creates == 1 && f.ledger.records().value(r.id).preCreateContextDigest == QString(64, 'b'),
      "creation context not persisted"
  );
  require(
      f.executor.create(r, auth, deadline()).outcome == ResourceOutcome::Active && f.provider->creates == 1,
      "duplicate created new resource"
  );
  auto conflicting = r;
  conflicting.creationIntent = id();
  require(
      f.executor.create(conflicting, auth, deadline()).outcome == ResourceOutcome::Rejected,
      "resource ID parameter collision accepted"
  );
  require(
      f.executor.cleanupOwner(r.owner, deadline()).outcome == ResourceOutcome::Closed && f.provider->objects.isEmpty(),
      "cleanup"
  );
  require(f.executor.create(r, auth, deadline()).outcome == ResourceOutcome::Rejected, "closed intent recreated");
}
static void cancellation()
{
  Fixture f;
  auto r = record();
  auto auth = grant(r);
  f.provider->duringCreate = [&] { auth.revoked->store(true); };
  const auto result = f.executor.create(r, auth, deadline());
  require(
      result.outcome == ResourceOutcome::Cancelled && f.provider->objects.isEmpty(),
      "cancelled create was lost or exposed"
  );
  require(f.ledger.records().value(r.id).phase == ResourcePhase::Closed, "cancelled output not closed");
}
static void ambiguity()
{
  Fixture f;
  auto r = record();
  auto auth = grant(r);
  f.provider->ambiguous = true;
  const auto result = f.executor.create(r, auth, deadline());
  require(
      result.outcome == ResourceOutcome::CleanupRequired && f.provider->released.isEmpty(), "ambiguous object deleted"
  );
  f.provider->ambiguous = false;
  require(f.executor.cleanupOwner(r.owner, deadline()).outcome == ResourceOutcome::Closed, "owned retry did not close");
}
static void absence()
{
  Fixture f;
  auto r = record();
  auto auth = grant(r);
  require(f.executor.create(r, auth, deadline()).outcome == ResourceOutcome::Active, "active");
  f.provider->removeOnRelease = false;
  require(
      f.executor.cleanupOwner(r.owner, deadline()).outcome == ResourceOutcome::CleanupRequired,
      "successful release call treated as absence"
  );
  f.provider->removeOnRelease = true;
  f.provider->absenceVerified = false;
  require(
      f.executor.cleanupOwner(r.owner, deadline()).outcome == ResourceOutcome::CleanupRequired,
      "absence without native evidence accepted"
  );
  f.provider->absenceVerified = true;
  require(f.executor.cleanupOwner(r.owner, deadline()).outcome == ResourceOutcome::Closed, "verified retry");
}
static void dependency()
{
  Fixture f;
  auto parent = record();
  auto auth = grant(parent);
  require(f.executor.create(parent, auth, deadline()).outcome == ResourceOutcome::Active, "parent");
  auto child = record(parent.owner);
  child.dependencies = {parent.id};
  child.kind = "capture";
  require(f.executor.create(child, auth, deadline()).outcome == ResourceOutcome::Active, "child");
  require(f.executor.cleanupOwner(parent.owner, deadline()).outcome == ResourceOutcome::Closed, "dependency closure");
  require(f.provider->released == QStringList{child.id, parent.id}, "parent destroyed before dependent producer");
}
static void intentRecovery()
{
  Fixture f;
  auto r = record();
  r.preCreateContextDigest = QString(64, 'b');
  require(f.ledger.createIntent(r), "persist crash intent");
  f.provider->objects.insert(r.id, "surviving-owner-bound-object");
  require(f.executor.cleanupOwner(r.owner, deadline()).outcome == ResourceOutcome::Closed, "intent-only orphan lost");
  require(f.provider->released.size() == 1, "orphan not released");
  auto stale = record();
  stale.bootInstance = "old-boot";
  stale.preCreateContextDigest = QString(64, 'b');
  require(f.ledger.createIntent(stale), "old boot intent");
  f.provider->objects.insert(stale.id, "reused-id");
  require(
      f.executor.cleanupOwner(stale.owner, deadline()).outcome == ResourceOutcome::CleanupRequired,
      "old boot identity authorized deletion"
  );
  f.provider->objects.remove(stale.id);
  require(
      f.executor.cleanupOwner(stale.owner, deadline()).outcome == ResourceOutcome::Closed,
      "proven old-boot absence not accepted"
  );
}
static void barriers()
{
  Fixture f;
  auto r = record();
  auto auth = grant(r);
  auth.revoked->store(true);
  require(
      f.executor.create(r, auth, deadline()).outcome == ResourceOutcome::Rejected && f.provider->creates == 0,
      "revoked create"
  );
  auth.revoked->store(false);
  f.provider->duringCreate = [&] {
    require(
        f.executor.cleanupOwner(r.owner, deadline()).primaryError == "ResourceExecutorBusy",
        "provider reentered executor"
    );
  };
  require(f.executor.create(r, auth, deadline()).outcome == ResourceOutcome::Active, "create");
  f.quiesced = false;
  require(
      f.executor.cleanupOwner(r.owner, deadline()).outcome == ResourceOutcome::CleanupRequired &&
          f.provider->released.isEmpty(),
      "producer release barrier bypassed"
  );
  f.quiesced = true;
  f.provider->quiescent = false;
  require(
      f.executor.cleanupOwner(r.owner, deadline()).outcome == ResourceOutcome::CleanupRequired &&
          f.provider->released.isEmpty(),
      "in-flight create ignored"
  );
  f.provider->quiescent = true;
  require(f.executor.cleanupOwner(r.owner, deadline()).outcome == ResourceOutcome::Closed, "quiescence retry");
}
static QString folder(const QString &root)
{
  QFile f(root + "/CURRENT");
  require(f.open(QIODevice::ReadOnly), "current");
  return root + "/" + QJsonDocument::fromJson(f.readAll()).object().value("generation").toString();
}
static void observationFailure()
{
  Fixture f;
  auto r = record();
  auto auth = grant(r);
  f.provider->duringCreate = [&] {
    const auto log = folder(f.temp.path() + "/ledger") + "/events.log";
    require(QFile::rename(log, log + ".retained"), "move journal");
    require(QFile::link(log + ".retained", log), "inject forbidden journal symlink");
  };
  const auto result = f.executor.create(r, auth, deadline());
  require(
      result.outcome == ResourceOutcome::CleanupRequired && result.primaryError == "ResourceObservationNotDurable",
      "journal failure hidden"
  );
  require(
      f.provider->objects.isEmpty() && f.provider->released.size() == 1, "unlogged native resource not compensated"
  );
  require(!f.ledger.activeOwner().isEmpty(), "non-durable closure released owner gate");
}
static void mutateSnapshot(const QString &root, const std::function<void(QJsonObject &)> &mutate)
{
  const auto path = folder(root) + "/snapshot.json";
  QFile sf(path);
  require(sf.open(QIODevice::ReadOnly), "snapshot");
  auto snapshot = QJsonDocument::fromJson(sf.readAll()).object();
  sf.close();
  auto resources = snapshot.value("resources").toArray();
  auto item = resources[0].toObject();
  mutate(item);
  resources[0] = item;
  snapshot.insert("resources", resources);
  const auto bytes = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
  require(sf.open(QIODevice::WriteOnly | QIODevice::Truncate) && sf.write(bytes) == bytes.size(), "snapshot write");
  sf.close();
  QFile cf(root + "/CURRENT");
  require(cf.open(QIODevice::ReadOnly), "current read");
  auto current = QJsonDocument::fromJson(cf.readAll()).object();
  cf.close();
  current.insert(
      "snapshotDigest", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())
  );
  const auto cb = QJsonDocument(current).toJson(QJsonDocument::Compact);
  require(cf.open(QIODevice::WriteOnly | QIODevice::Truncate) && cf.write(cb) == cb.size(), "current write");
  cf.close();
}
static void legacy()
{
  QTemporaryDir temp;
  const auto root = temp.path() + "/ledger";
  auto r = record();
  r.preCreateContextDigest = QString(64, 'b');
  {
    DurableResourceLedger l(root);
    require(l.createIntent(r) && l.compact(), "write legacy base");
  }
  mutateSnapshot(root, [](QJsonObject &old) {
    old.remove("preCreateContextDigest");
    old.remove("verificationPolicy");
    old.insert("schemaVersion", 1);
  });
  DurableResourceLedger restored(root);
  require(restored.status() == DurableResourceLedger::Status::Ready, "legacy record rejected");
  require(
      restored.records().value(r.id).preCreateContextDigest.isEmpty() &&
          restored.records().value(r.id).verificationPolicy == "legacy-unproven",
      "legacy ownership evidence invented"
  );
}
static void unsupported()
{
  QTemporaryDir temp;
  const auto root = temp.path() + "/ledger";
  auto r = record();
  r.preCreateContextDigest = QString(64, 'b');
  {
    DurableResourceLedger l(root);
    require(l.createIntent(r) && l.compact(), "future base");
  }
  mutateSnapshot(root, [](QJsonObject &o) { o.insert("schemaVersion", 3); });
  DurableResourceLedger restored(root);
  require(
      restored.status() == DurableResourceLedger::Status::UnsupportedVersion,
      "future record version erased or misclassified"
  );
}
static void providerException()
{
  Fixture f;
  auto r = record();
  auto auth = grant(r);
  f.provider->duringCreate = [] { throw std::runtime_error("fixture native call"); };
  const auto result = f.executor.create(r, auth, deadline());
  require(
      result.outcome == ResourceOutcome::Rejected && result.primaryError == "ResourceCreateUncertain",
      "provider exception escaped or became success"
  );
  require(
      f.provider->objects.isEmpty() && f.ledger.records().value(r.id).phase == ResourcePhase::Closed,
      "exception-created native object lost"
  );
}
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  try {
    require(argc == 2, "case required");
    const std::string c = argv[1];
    if (c == "ResourceExecutionDurability")
      durable();
    else if (c == "ResourceCancelledCreation")
      cancellation();
    else if (c == "ResourceAmbiguousOwner")
      ambiguity();
    else if (c == "ResourceIndependentAbsence")
      absence();
    else if (c == "ResourceDependencyCleanup")
      dependency();
    else if (c == "ResourceIntentRecovery")
      intentRecovery();
    else if (c == "ResourceExecutionBarriers")
      barriers();
    else if (c == "ResourceObservationFailure")
      observationFailure();
    else if (c == "ResourceLegacyRecord")
      legacy();
    else if (c == "ResourceUnsupportedRecord")
      unsupported();
    else if (c == "ResourceProviderException")
      providerException();
    else
      throw std::runtime_error("unknown case");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
