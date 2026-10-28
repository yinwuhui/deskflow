// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
// Manual only: briefly creates a real additional output, then verifies its absence.
#include "display/platform/macos/MacResourceProvider.h"
#include "display/resource/DurableResourceLedger.h"
#include "display/supervision/WorkerProcess.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
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
static bool until(const std::function<bool()> &f, int timeout)
{
  QElapsedTimer timer;
  timer.start();
  while (!f() && timer.elapsed() < timeout) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(2);
  }
  return f();
}
static ResourceRecord record(QString owner, QString session, QString plan, QString kind)
{
  ResourceRecord r;
  r.id = id();
  r.owner = owner;
  r.creationIntent = id();
  r.session = session;
  r.planHash = plan;
  r.userSession = macUserSessionReference();
  r.bootInstance = macBootInstanceReference();
  r.provider = "macos.cgdisplay.helper.v1";
  r.providerVersion = "1";
  r.kind = kind;
  r.preCreateContextDigest = macDisplayInventory().digest;
  r.verificationPolicy = "native-helper-and-system-absence.v1";
  r.evidence = {"unproven", "native-smoke-create-intent", "unknown"};
  return r;
}
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  if (argc == 2 && QString(argv[1]) == "--display-platform-probe") {
    if (QCoreApplication::applicationFilePath().endsWith(".invalid")) {
      QFile out;
      if (!out.open(stdout, QIODevice::WriteOnly))
        return 2;
      QDataStream stream(&out);
      stream << quint32(0x424d5001) << true << true << false << quint32(0xfffffffe);
      return out.flush() ? 0 : 3;
    }
    QThread::sleep(5);
    return 0;
  }
  try {
    require(argc == 3, "usage: MacPlatformHelperSmoke <helper-executable> <graceful|terminate>");
    const auto mode = QString(argv[2]);
    require(
        mode == "normal" || mode == "recover" || mode == "intent" || mode == "ambiguous" || mode == "probe-timeout" ||
            mode == "probe-invalid",
        "unknown mode"
    );
    const auto before = macDisplayInventory();
    const auto desktop = macDesktopState();
    require(
        before.complete && desktop.onConsole && desktop.lockKnown && !desktop.locked,
        "interactive unlocked native desktop required"
    );
    const auto owner = id(), session = id(),
               plan = QString::fromLatin1(
                   QCryptographicHash::hash((owner + session).toUtf8(), QCryptographicHash::Sha256).toHex()
               );
    QTemporaryDir temporary;
    require(temporary.isValid(), "private ledger directory");
    temporary.setAutoRemove(false); // Preserve durable evidence on every failure.
    std::cerr << "auditLedger=" << temporary.path().toStdString() << "/ledger\n";
    auto ledger = std::make_unique<DurableResourceLedger>(temporary.path() + "/ledger");
    auto workerRecord = record(owner, session, plan, "worker"),
         outputRecord = record(owner, session, plan, "virtualOutput");
    outputRecord.dependencies = {workerRecord.id};
    require(!workerRecord.userSession.isEmpty() && !workerRecord.bootInstance.isEmpty(), "durable process intent");
    WorkerBinding binding;
    binding.token = {session.toStdString(), 1, 1, 1, owner.toStdString(), id().toStdString()};
    binding.localUserSession = workerRecord.userSession;
    binding.buildCompatibility = "platform-helper1";
    binding.backend = "platform.macos.virtual-output";
    binding.profile = "macos.virtual-output.cgdisplay.sdr1080.v1";
    binding.planHash = plan;
    binding.localIdentity = QString::fromLatin1(
        QCryptographicHash::hash(binding.localUserSession.toUtf8(), QCryptographicHash::Sha256).toHex()
    );
    binding.purpose = WorkerPurpose::Platform;
    QFile executable(QFileInfo(QString::fromLocal8Bit(argv[1])).canonicalFilePath());
    require(executable.open(QIODevice::ReadOnly), "helper executable");
    QCryptographicHash digest(QCryptographicHash::Sha256);
    require(digest.addData(&executable), "helper hash");
    WorkerRegistration registration{
        QFileInfo(executable).canonicalFilePath(), QString::fromLatin1(digest.result().toHex()), binding.backend,
        binding.profile, WorkerPurpose::Platform
    };
    if (mode == "probe-timeout" || mode == "probe-invalid") {
      const auto fixturePath = temporary.path() + "/fixture.invalid";
      if (mode == "probe-invalid")
        require(QFile::copy(QCoreApplication::applicationFilePath(), fixturePath), "copy malformed fixture");
      QFile own(mode == "probe-invalid" ? fixturePath : QCoreApplication::applicationFilePath());
      require(own.open(QIODevice::ReadOnly), "fixture executable");
      QCryptographicHash ownHash(QCryptographicHash::Sha256);
      require(ownHash.addData(&own), "fixture digest");
      registration.executable = QFileInfo(own).canonicalFilePath();
      registration.sha256 = QString::fromLatin1(ownHash.result().toHex());
      require(validWorkerExecutable(registration), "registered fixture executable");
      MacResourceProvider stalled(*ledger, registration, binding);
      QElapsedTimer elapsed;
      elapsed.start();
      const auto observation = stalled.snapshot(
          workerRecord, Clock::now() + std::chrono::milliseconds(mode == "probe-invalid" ? 1500 : 300)
      );
      require(!observation.complete && elapsed.elapsed() < 1800, "stalled probe was not bounded");
      std::cout << "{\"mode\":\"" << mode.toStdString() << "\",\"bounded\":true}\n";
      temporary.setAutoRemove(true);
      return 0;
    }
    auto provider = std::make_shared<MacResourceProvider>(*ledger, registration, binding);
    auto makeExecutor = [&] {
      auto executor = std::make_unique<ResourceExecutor>(
          *ledger, workerRecord.userSession, workerRecord.bootInstance, [](const QString &) { return true; }
      ); // No media producer in this resource-only test.
      auto registered = provider->registration();
      registered.implementation = provider;
      require(executor->registerProvider(std::move(registered)), "register native provider");
      return executor;
    };
    auto executor = makeExecutor();
    ResourceAuthorization auth{
        owner,
        session,
        plan,
        workerRecord.userSession,
        workerRecord.bootInstance,
        Clock::now() + std::chrono::seconds(10),
        std::make_shared<std::atomic<bool>>(false)
    };
    if (mode == "ambiguous") {
      workerRecord.preCreateContextDigest = before.digest;
      require(ledger->createIntent(workerRecord), "ambiguous durable intent");
      auto current = nativeProcessIdentity(QCoreApplication::applicationPid());
      require(bool(current), "read current identity");
      require(
          ledger->transition(
              workerRecord.id, ResourcePhase::Observed, current->encode(), {"unproven", "ambiguous-test", "unknown"}
          ),
          "record ambiguous identity"
      );
      auto result = executor->cleanupOwner(owner, Clock::now() + std::chrono::seconds(1));
      require(
          result.outcome == ResourceOutcome::CleanupRequired && !ledger->activeOwner().isEmpty(),
          "live unregistered PID was adopted or cleared"
      );
      require(nativeProcessPresence(*current) == NativeProcessPresence::Exact, "unrelated live process changed");
      std::cout << "{\"test\":\"native-mac-resource-provider\",\"mode\":\"ambiguous\",\"blockedSafely\":true}\n";
      temporary.setAutoRemove(true);
      return 0;
    }
    auto worker = executor->create(workerRecord, auth, auth.expires);
    if (worker.outcome != ResourceOutcome::Active)
      std::cerr << worker.primaryError.toStdString() << "/" << worker.cleanupError.toStdString() << '\n';
    require(worker.outcome == ResourceOutcome::Active, "worker not active");
    if (mode == "intent") {
      outputRecord.preCreateContextDigest = macDisplayInventory().digest;
      require(ledger->createIntent(outputRecord), "intent before crash-window creation");
      auto claim = provider->create(outputRecord, auth.expires);
      require(claim.certainty == CreationCertainty::Created, "intent-window output not created");
      // Deliberately omit Observed, modelling the crash between create and journal.
    } else {
      auto output = executor->create(outputRecord, auth, auth.expires);
      if (output.outcome != ResourceOutcome::Active)
        std::cerr << output.primaryError.toStdString() << "/" << output.cleanupError.toStdString() << '\n';
      require(output.outcome == ResourceOutcome::Active, "output not active");
    }
    if (mode != "normal") {
      executor.reset();
      provider.reset(); // Destroy only the live owned helper, retain durable records.
      ledger.reset();
      ledger = std::make_unique<DurableResourceLedger>(temporary.path() + "/ledger");
      require(ledger->status() == DurableResourceLedger::Status::Ready, "reopen durable ledger");
      provider = std::make_shared<MacResourceProvider>(*ledger, registration, binding);
      executor = makeExecutor();
    }
    ResourceResult closed;
    require(
        until(
            [&] {
              closed = executor->cleanupOwner(owner, Clock::now() + std::chrono::seconds(5));
              return closed.outcome == ResourceOutcome::Closed;
            },
            6000
        ),
        "native resource owner not closed"
    );
    const auto after = macDisplayInventory();
    require(after.complete && !macResourceHintPresent(after, owner, outputRecord.id), "native output still present");
    require(ledger->activeOwner().isEmpty(), "owner reservation retained");
    QJsonObject result{
        {"test", "native-mac-resource-provider"},
        {"mode", mode},
        {"displaysBefore", int(before.displays.size())},
        {"displaysAfter", int(after.displays.size())},
        {"ownerClosed", true},
        {"outputAbsenceVerified", true},
        {"scope", "real ResourceExecutor/provider lifecycle; no capture/media certification"}
    };
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
    temporary.setAutoRemove(true);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
