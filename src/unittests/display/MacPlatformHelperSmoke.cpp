// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
// Manual only: briefly creates a real additional output, then verifies its absence.
#include "display/platform/macos/MacVirtualOutput.h"
#include "display/resource/DurableResourceLedger.h"
#include "display/supervision/WorkerProcess.h"
#include <QCoreApplication>
#include <QCryptographicHash>
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
  try {
    require(argc == 3, "usage: MacPlatformHelperSmoke <helper-executable> <graceful|terminate>");
    const bool force = QString(argv[2]) == "terminate";
    require(force || QString(argv[2]) == "graceful", "unknown mode");
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
    DurableResourceLedger ledger(temporary.path() + "/ledger");
    auto workerRecord = record(owner, session, plan, "worker"),
         outputRecord = record(owner, session, plan, "virtualOutput");
    outputRecord.dependencies = {workerRecord.id};
    require(
        !workerRecord.userSession.isEmpty() && !workerRecord.bootInstance.isEmpty() &&
            ledger.createIntent(workerRecord),
        "durable process intent"
    );
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
    WorkerProcess process;
    bool observed = false, active = false, exited = false, stopped = false;
    QString failure, identity, reference, processReference;
    uint32_t displayId = 0;
    process.onEstablished = [&] {
      processReference =
          QString("native-child.%1.%2").arg(process.processId()).arg(QString::fromStdString(binding.token.worker));
      const ResourceEvidence ownership{
          "nativeObjectOwnership", QString::fromStdString(binding.token.worker), "verified"
      };
      require(
          ledger.transition(workerRecord.id, ResourcePhase::Observed, processReference, ownership) &&
              ledger.transition(workerRecord.id, ResourcePhase::Active, processReference, ownership),
          "durable authenticated process observation"
      );
      outputRecord.preCreateContextDigest = macDisplayInventory().digest;
      require(ledger.createIntent(outputRecord), "durable display intent");
      WorkerPrepareOutput preparation{
          outputRecord.id, outputRecord.creationIntent, outputRecord.preCreateContextDigest,
          uint64_t(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  (Clock::now() + std::chrono::seconds(10)).time_since_epoch()
              )
                  .count()
          )
      };
      require(process.send(preparation), "prepare native output");
    };
    process.onMessage = [&](const WorkerMessage &message) {
      if (std::holds_alternative<WorkerReady>(message))
        require(process.send(WorkerStart{}), "create native output");
      else if (const auto *claim = std::get_if<WorkerOutputObserved>(&message)) {
        require(claim->resourceId == outputRecord.id, "claim scope");
        observed = true;
        identity = claim->nativeIdentity;
        displayId = uint32_t(claim->source.primary);
        reference = QString("cgdisplay.%1.%2").arg(displayId).arg(identity);
        require(
            ledger.transition(
                outputRecord.id, ResourcePhase::Observed, reference,
                {"nativeObjectOwnership", QString::fromStdString(binding.token.worker), "verified"}
            ),
            "durable native ownership before layout"
        );
        require(process.send(WorkerConfigureOutput{outputRecord.id}), "configure only after durable observed record");
      } else if (const auto *ready = std::get_if<WorkerOutputReady>(&message)) {
        require(ready->resourceId == outputRecord.id, "ready scope");
        const auto during = macDisplayInventory();
        bool exact = false;
        for (const auto &row : during.displays)
          if (row.id == displayId && row.identity == identity && row.active && !row.mirrored &&
              row.pixelWidth == 1920 && row.pixelHeight == 1080)
            exact = true;
        require(during.complete && exact, "independent topology verification");
        require(
            ledger.transition(
                outputRecord.id, ResourcePhase::Active, reference, {"nativeObjectOwnership", identity, "verified"}
            ),
            "durable topology confirmation"
        );
        active = true;
      } else if (const auto *f = std::get_if<WorkerFailure>(&message)) {
        failure = f->code;
      } else if (std::holds_alternative<WorkerQuiesced>(message))
        stopped = true;
    };
    process.onExited = [&](int, QProcess::ExitStatus) { exited = true; };
    require(process.launch(registration, binding), "launch native helper");
    const bool ready = until([&] { return active || exited || !failure.isEmpty(); }, 12000);
    if (!ready || !active) {
      if (process.running())
        process.stop(StopReason::Deadline);
      until([&] { return exited; }, 6500);
      std::cerr << "native failure=" << failure.toStdString() << " supervisor=" << process.error().toStdString()
                << '\n';
      throw std::runtime_error("native output not ready");
    }
    require(
        ledger.transition(
            outputRecord.id, ResourcePhase::Releasing, reference, {"unproven", "native-smoke-stop", "unknown"}
        ),
        "durable display stop intent"
    );
    if (force)
      process.terminate();
    else
      process.stop(StopReason::Cancelled);
    require(until([&] { return exited; }, 6500), "helper failed bounded exit");
    require(
        until(
            [&] {
              const auto inventory = macDisplayInventory();
              if (!inventory.complete)
                return false;
              for (const auto &row : inventory.displays)
                if (row.identity == identity)
                  return false;
              return true;
            },
            4000
        ),
        "created output still enumerated after helper exit"
    );
    const ResourceEvidence absence{"systemAbsenceCheck", identity, "verified"};
    require(
        ledger.transition(outputRecord.id, ResourcePhase::ReleaseVerified, reference, absence) &&
            ledger.transition(outputRecord.id, ResourcePhase::Closed, reference, absence),
        "durable output closure"
    );
    const ResourceEvidence processAbsent{"systemAbsenceCheck", processReference, "verified"};
    require(
        ledger.transition(
            workerRecord.id, ResourcePhase::Releasing, processReference,
            {"unproven", "native-smoke-worker-stop", "unknown"}
        ) && ledger.transition(workerRecord.id, ResourcePhase::ReleaseVerified, processReference, processAbsent) &&
            ledger.transition(workerRecord.id, ResourcePhase::Closed, processReference, processAbsent),
        "process closes after dependent output"
    );
    if (!force && !stopped)
      std::cerr << "graceful terminal cause=" << process.error().toStdString() << '\n';
    require(ledger.activeOwner().isEmpty() && (force || stopped), "unverified owner released");
    const auto after = macDisplayInventory();
    const QJsonObject result{
        {"test", "native-mac-platform-helper"},
        {"mode", force ? "terminate" : "graceful"},
        {"observed", observed},
        {"active", active},
        {"nativeDisplayId", double(displayId)},
        {"nativeIdentity", identity},
        {"displaysBefore", int(before.displays.size())},
        {"displaysAfter", int(after.displays.size())},
        {"helperExited", exited},
        {"quiescedMessage", stopped},
        {"cleanupVerified", true},
        {"ownerClosed", ledger.activeOwner().isEmpty()},
        {"supervisorCause", process.error()},
        {"scope", "real output/resource lifecycle; no capture, media or presentation certification"}
    };
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
