// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/resource/DurableResourceLedger.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
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
static QString randomId()
{
  return QUuid::createUuid().toString(QUuid::Id128);
}
static ResourceRecord intent(QString owner = {})
{
  ResourceRecord r;
  r.id = randomId();
  r.owner = owner.isEmpty() ? randomId() : owner;
  r.creationIntent = randomId();
  r.session = QString(32, '1');
  r.planHash = QString(64, 'a');
  r.userSession = "test-user-session";
  r.bootInstance = "test-boot";
  r.preCreateContextDigest = QString(64, 'd');
  r.verificationPolicy = "owner-and-absence.v1";
  r.provider = "test.virtual-output.v1";
  r.providerVersion = "1";
  r.kind = "virtualOutput";
  r.evidence = {"unproven", "create-intent", "unknown"};
  return r;
}
static QString currentFolder(const QString &root)
{
  QFile f(root + "/CURRENT");
  require(f.open(QIODevice::ReadOnly), "read current");
  return root + "/" + QJsonDocument::fromJson(f.readAll()).object().value("generation").toString();
}
static ResourceEvidence absent()
{
  return {"systemAbsenceCheck", "fixture-enumeration", "verified"};
}
static void release(DurableResourceLedger &ledger, const ResourceRecord &r)
{
  const auto ref = ledger.records().value(r.id).platformReference;
  require(ledger.transition(r.id, ResourcePhase::Releasing, ref, {"unproven", "stopping", "unknown"}), "begin release");
  require(ledger.transition(r.id, ResourcePhase::ReleaseVerified, ref, absent()), "verify release");
  require(ledger.transition(r.id, ResourcePhase::Closed, ref, absent()), "close owner");
}
static void durableIntentAndReplay()
{
  QTemporaryDir fixture;
  require(fixture.isValid(), "temp directory");
  const auto root = fixture.filePath("ledger");
  const auto r = intent();
  {
    DurableResourceLedger ledger(root);
    require(ledger.status() == DurableResourceLedger::Status::Ready, "ledger not ready");
    require(ledger.createIntent(r), "persist intent");
    require(ledger.activeOwner() == r.owner, "owner not held");
    require(!ledger.createIntent(intent()), "second owner acquired resources");
    require(!ledger.transition(r.id, ResourcePhase::Active, "display-1", absent()), "create skipped observation");
  }
  {
    DurableResourceLedger ledger(root);
    require(ledger.status() == DurableResourceLedger::Status::Ready, "replay failed");
    require(ledger.records().value(r.id).phase == ResourcePhase::Intent, "creation intent lost across restart");
    require(
        ledger.transition(
            r.id, ResourcePhase::Observed, "display-1", {"nativeObjectOwnership", "helper-1", "verified"}
        ),
        "observe resource"
    );
    require(
        ledger.transition(r.id, ResourcePhase::Active, "display-1", {"nativeObjectOwnership", "helper-1", "verified"}),
        "activate resource"
    );
    require(
        !ledger.transition(r.id, ResourcePhase::Closed, "display-1", absent()), "resource closed without releasing"
    );
    release(ledger, r);
    require(ledger.activeOwner().isEmpty(), "verified owner not released");
    require(!ledger.createIntent(intent(r.owner)), "closed resource owner reused");
  }
  DurableResourceLedger final(root);
  require(final.status() == DurableResourceLedger::Status::Ready, "final replay failed");
  require(final.records().value(r.id).phase == ResourcePhase::Closed, "owner closure not durable");
}
static void exclusiveLock()
{
  QTemporaryDir fixture;
  const auto root = fixture.filePath("ledger");
  DurableResourceLedger a(root), b(root);
  require(a.status() == DurableResourceLedger::Status::Ready, "first owner unavailable");
  require(b.status() == DurableResourceLedger::Status::Busy, "second instance acquired ledger lock");
}
static void truncatedTail()
{
  QTemporaryDir fixture;
  const auto root = fixture.filePath("ledger");
  const auto r = intent();
  QString log;
  qint64 goodSize = 0;
  {
    DurableResourceLedger ledger(root);
    require(ledger.createIntent(r), "intent");
    log = currentFolder(root) + "/events.log";
    goodSize = QFileInfo(log).size();
  }
  {
    QFile f(log);
    require(f.open(QIODevice::Append), "open log");
    require(f.write("BMDL\x00\x01", 6) == 6, "append incomplete header");
  }
  DurableResourceLedger ledger(root);
  require(ledger.status() == DurableResourceLedger::Status::Ready, "torn tail made valid prefix unavailable");
  require(ledger.activeOwner() == r.owner, "intent removed by tail repair");
  require(QFileInfo(log).size() == goodSize, "incomplete tail not truncated");
}
static void checksumCorruption()
{
  QTemporaryDir fixture;
  const auto root = fixture.filePath("ledger");
  QString log;
  {
    DurableResourceLedger ledger(root);
    require(ledger.createIntent(intent()), "intent");
    log = currentFolder(root) + "/events.log";
  }
  {
    QFile f(log);
    require(f.open(QIODevice::ReadWrite), "open log");
    require(f.seek(40), "seek");
    auto b = f.read(1);
    require(b.size() == 1, "read byte");
    b[0] = static_cast<char>(b[0] ^ 1);
    require(f.seek(40) && f.write(b) == 1, "corrupt byte");
  }
  DurableResourceLedger ledger(root);
  require(ledger.status() == DurableResourceLedger::Status::Corrupt, "checksum corruption skipped");
  require(!ledger.createIntent(intent()), "new resources allowed on corrupt ledger");
}
static void missingCurrent()
{
  QTemporaryDir fixture;
  const auto root = fixture.filePath("ledger");
  {
    DurableResourceLedger ledger(root);
    require(ledger.createIntent(intent()), "intent");
  }
  require(QFile::remove(root + "/CURRENT"), "remove current");
  DurableResourceLedger ledger(root);
  require(ledger.status() == DurableResourceLedger::Status::Corrupt, "missing CURRENT discarded ownership");
}
static void compaction()
{
  QTemporaryDir fixture;
  const auto root = fixture.filePath("ledger");
  ResourceRecord last;
  {
    DurableResourceLedger ledger(root, 4096);
    require(ledger.status() == DurableResourceLedger::Status::Ready, "small ledger unavailable");
    for (unsigned i = 0; i < 12; ++i) {
      last = intent();
      require(ledger.createIntent(last), "compaction create");
      release(ledger, last);
    }
    const auto active = intent();
    require(ledger.createIntent(active), "active intent");
    last = active;
    require(ledger.compact(), "checkpoint failed");
    require(
        QDir(root).entryList({"gen-*"}, QDir::Dirs | QDir::NoDotAndDotDot).size() <= 2,
        "unbounded checkpoint generations"
    );
  }
  DurableResourceLedger ledger(root, 4096);
  require(ledger.status() == DurableResourceLedger::Status::Ready, "compacted ledger replay failed");
  require(ledger.activeOwner() == last.owner, "checkpoint lost active owner");
}
static void dependencyOrder()
{
  QTemporaryDir fixture;
  DurableResourceLedger ledger(fixture.filePath("ledger"));
  auto parent = intent();
  require(ledger.createIntent(parent), "parent intent");
  auto child = intent(parent.owner);
  child.kind = "capture";
  child.dependencies = {parent.id};
  require(ledger.createIntent(child), "child intent");
  require(
      ledger.transition(parent.id, ResourcePhase::Releasing, {}, {"unproven", "stop", "unknown"}),
      "parent release intent"
  );
  require(
      !ledger.transition(parent.id, ResourcePhase::ReleaseVerified, {}, absent()), "parent released with live child"
  );
  release(ledger, child);
  require(ledger.transition(parent.id, ResourcePhase::ReleaseVerified, {}, absent()), "parent verify after child");
  require(ledger.transition(parent.id, ResourcePhase::Closed, {}, absent()), "parent close");
}
static void symlinkSafety()
{
#ifndef Q_OS_WIN
  QTemporaryDir fixture;
  const auto target = fixture.filePath("other");
  QFile f(target);
  require(f.open(QIODevice::WriteOnly), "fixture file");
  f.write("keep");
  f.close();
  const auto root = fixture.filePath("ledger");
  require(QDir().mkpath(root), "mkdir");
  require(QFile::link(target, root + "/CURRENT"), "symlink");
  DurableResourceLedger ledger(root);
  require(ledger.status() == DurableResourceLedger::Status::Corrupt, "symlink target followed");
  require(f.open(QIODevice::ReadOnly) && f.readAll() == "keep", "foreign data modified");
#endif
}
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  try {
    if (argc != 2)
      throw std::runtime_error("one test required");
    const std::string n = argv[1];
    if (n == "DurableIntentReplay")
      durableIntentAndReplay();
    else if (n == "ExclusiveLedger")
      exclusiveLock();
    else if (n == "TornLedgerTail")
      truncatedTail();
    else if (n == "LedgerCorruption")
      checksumCorruption();
    else if (n == "MissingCurrent")
      missingCurrent();
    else if (n == "LedgerCompaction")
      compaction();
    else if (n == "DependencyOrder")
      dependencyOrder();
    else if (n == "LedgerSymlink")
      symlinkSafety();
    else
      throw std::runtime_error("unknown test");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
