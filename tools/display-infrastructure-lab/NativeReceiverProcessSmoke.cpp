// SPDX-License-Identifier: MIT
// Diagnostic sender only. This does not supply product authorization or TLS.
#include "display/media/gstreamer/NativeWebRtcEngine.h"
#include "display/supervision/WorkerProcess.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <iostream>
using namespace deskflow::display::infra;
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  if (argc != 3)
    return 2;
  NativeWebRtcConfiguration tx;
  auto &b = tx.binding;
  b.token = {std::string(32, '1'), 1, 1, 1, std::string(32, '2'), std::string(32, '3')};
  b.localUserSession = "diagnostic.local";
  b.buildCompatibility = "native-receiver-process1";
  b.backend = "webrtc";
  b.profile = "diagnostic.webrtc.h264.control-map.v1";
  b.planHash = QString(64, 'd');
  b.controlBinding = QString(64, 'c');
  b.localIdentity = QString(64, 'a');
  b.peerIdentity = QString(64, 'b');
  b.direction = Direction::Send;
  auto &p = tx.preparation;
  p.allowedPeerAddresses = {"127.0.0.1"};
  p.allowLoopbackDiagnostics = true;
  p.startupDeadlineNanoseconds = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>((Clock::now() + std::chrono::seconds(10)).time_since_epoch())
          .count()
  );
  p.sourceResource = tx.streamSourceResource = QString(32, '5');
  p.source = {NativeSourceKind::CgDisplay, 77, 0};
  p.sourceIdentity = "11111111-2222-3333-4444-555555555555";
  p.sourceContextDigest = QString(64, 'a');
  p.sourceTopologyGeneration = tx.streamTopologyGeneration = 1;
  tx.localAddress = "127.0.0.1";
  tx.permit = MediaWorkPermit::issue(tx, Clock::now() + std::chrono::seconds(30));
  QString error;
  NativeWebRtcEngine sender;
  if (!NativeWebRtcEngine::initializeRuntime(QString::fromLocal8Bit(argv[1]), error) || !sender.prepare(tx))
    return 3;
  auto receiverBinding = b;
  receiverBinding.direction = Direction::Receive;
  std::swap(receiverBinding.localIdentity, receiverBinding.peerIdentity);
  receiverBinding.token.worker = std::string(32, '4');
  auto receiverPreparation = p;
  receiverPreparation.sourceResource.clear();
  receiverPreparation.source = {};
  receiverPreparation.sourceIdentity.clear();
  receiverPreparation.sourceContextDigest.clear();
  receiverPreparation.sourceTopologyGeneration = 0;
  QFile executable(QString::fromLocal8Bit(argv[2]));
  if (!executable.open(QIODevice::ReadOnly))
    return 4;
  WorkerRegistration registration{
      executable.fileName(),
      QString::fromLatin1(QCryptographicHash::hash(executable.readAll(), QCryptographicHash::Sha256).toHex()),
      b.backend, b.profile
  };
  WorkerProcess worker;
  bool started = false, quiesced = false, exited = false, failed = false;
  uint64_t presented = 0;
  worker.onEstablished = [&] {
    if (!worker.send(receiverPreparation) ||
        !worker.send(WorkerReceiveBinding{"127.0.0.1", tx.streamSourceResource, QString(32, '6'), 1}))
      failed = true;
  };
  worker.onMessage = [&](const WorkerMessage &m) {
    if (std::holds_alternative<WorkerReady>(m)) {
      started = worker.send(WorkerStart{}) && sender.start();
      if (!started)
        failed = true;
    }
    if (auto signal = std::get_if<WorkerMediaSignal>(&m))
      if (!sender.applySignal(*signal))
        failed = true;
    if (auto frame = std::get_if<WorkerPresented>(&m))
      if (frame->exposed && frame->evidence == PresentationEvidence::NativePaintConfirmed)
        ++presented;
    if (auto failure = std::get_if<WorkerFailure>(&m)) {
      std::cerr << failure->code.toStdString() << "\n";
      failed = true;
    }
    if (std::holds_alternative<WorkerQuiesced>(m))
      quiesced = true;
  };
  worker.onExited = [&](int code, QProcess::ExitStatus status) {
    std::cerr << "exit=" << code << " status=" << status << " ipc=" << worker.error().toStdString() << "\n";
    exited = true;
    if (code != 0 || status != QProcess::NormalExit)
      failed = true;
  };
  if (!worker.launch(registration, receiverBinding)) {
    std::cerr << worker.error().toStdString();
    return 5;
  }
  auto budget = std::make_shared<FrameBudget>(3, 32 * 1024 * 1024);
  auto pool = std::make_shared<FramePool>(std::string(32, '7'), 1, b.token.session, budget);
  QElapsedTimer elapsed;
  elapsed.start();
  qint64 next = 0;
  uint64_t sequence = 0;
  while (!failed && elapsed.elapsed() < 8000) {
    QCoreApplication::processEvents();
    if (started) {
      sender.poll();
      for (auto &signal : sender.takeSignals())
        if (!worker.send(signal))
          failed = true;
      for (auto &map : sender.takeCorrelations())
        if (!worker.send(map))
          failed = true;
      if (elapsed.elapsed() >= next) {
        next = elapsed.elapsed() + 33;
        if (auto ticket = pool->reserve(8294400, MemoryDomain::Cpu)) {
          auto pixels = std::make_shared<std::vector<uint32_t>>(1920 * 1080, 0xff28b43c);
          FrameAllocation allocation{pixels, 8294400, MemoryDomain::Cpu, pixels->data()};
          pixels.reset();
          FrameDescriptor d;
          d.session = b.token.session;
          d.sourceResource = tx.streamSourceResource.toStdString();
          d.epoch = 1;
          d.topologyGeneration = 1;
          d.sequence = ++sequence;
          d.captureNanoseconds =
              uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
          d.captureClockId = "lab.monotonic";
          d.captureTimeValid = true;
          d.width = d.logicalWidth = 1920;
          d.height = d.logicalHeight = 1080;
          d.visible = {0, 0, 1920, 1080};
          d.planes = {{0, 7680, 8294400}};
          d.color = ColorProfile::SdrBt709Full8;
          if (!pool->adopt(*ticket, std::move(allocation)) || !pool->publish(*ticket, std::move(d)) ||
              !sender.submit(CpuFrameLease::acquire(pool)))
            failed = true;
        }
      }
      if (!sender.error().isEmpty()) {
        std::cerr << sender.error().toStdString();
        failed = true;
      }
    }
    QThread::msleep(2);
  }
  std::cerr << "before stop: " << worker.error().toStdString() << " sender=" << sender.error().toStdString()
            << " frames=" << sender.statistics().inputFrames << " failed=" << failed << " started=" << started << "\n";
  worker.stop(StopReason::Revoked);
  tx.permit->revoke();
  sender.stop();
  pool->retire();
  elapsed.restart();
  while (!exited && elapsed.elapsed() < 6000) {
    QCoreApplication::processEvents();
    QThread::msleep(2);
  }
  std::cout << "{\"presented\":" << presented << ",\"quiesced\":" << quiesced << ",\"exited\":" << exited
            << ",\"failed\":" << failed << ",\"budgetReleased\":" << pool->drained() << "}\n";
  return !failed && presented >= 30 && quiesced && exited && pool->drained() ? 0 : 1;
}
