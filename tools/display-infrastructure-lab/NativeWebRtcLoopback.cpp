// SPDX-License-Identifier: MIT
// Manual developer fixture: synthetic frames, local signaling and loopback only.
#include "ControlLoopback.h"
#include "display/media/gstreamer/H264CodecEvidence.h"
#include "display/media/gstreamer/NativeWebRtcEngine.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <iostream>
#include <stdexcept>
using namespace deskflow::display::infra;
static void require(bool value, const char *why)
{
  if (!value)
    throw std::runtime_error(why);
}
static NativeWebRtcConfiguration configuration(Direction direction)
{
  NativeWebRtcConfiguration c;
  auto &b = c.binding;
  auto &p = c.preparation;
  b.token = {
      std::string(32, '1'), 1, 1, 1, std::string(32, '2'), std::string(32, direction == Direction::Send ? '3' : '4')
  };
  b.localUserSession = "diagnostic.local";
  b.buildCompatibility = "native-webrtc-lab1";
  b.backend = "webrtc";
  b.profile = "diagnostic.webrtc.h264.control-map.v1";
  b.planHash = QString(64, 'd');
  b.controlBinding = QString(64, 'c');
  b.localIdentity = QString(64, direction == Direction::Send ? 'a' : 'b');
  b.peerIdentity = QString(64, direction == Direction::Send ? 'b' : 'a');
  b.direction = direction;
  p.allowedPeerAddresses = {"127.0.0.1"};
  p.allowLoopbackDiagnostics = true;
  p.startupDeadlineNanoseconds = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>((Clock::now() + std::chrono::seconds(10)).time_since_epoch())
          .count()
  );
  c.localAddress = "127.0.0.1";
  c.streamSourceResource = QString(32, '5');
  c.streamTopologyGeneration = 1;
  if (direction == Direction::Send) {
    p.sourceResource = c.streamSourceResource;
    p.source = {NativeSourceKind::CgDisplay, 77, 0};
    p.sourceIdentity = "11111111-2222-3333-4444-555555555555";
    p.sourceContextDigest = QString(64, 'a');
    p.sourceTopologyGeneration = 1;
  }
  c.permit = MediaWorkPermit::issue(c, Clock::now() + std::chrono::seconds(30));
  return c;
}
static void policyChecks()
{
  uint32_t checks = 0;
  for (int mode = 0; mode < 3; ++mode) {
    auto c = configuration(Direction::Receive);
    if (mode == 0)
      c.permit.reset();
    if (mode == 1)
      c.permit->revoke();
    if (mode == 2)
      c.binding.token.attempt++;
    NativeWebRtcEngine engine;
    require(!engine.prepare(c), "invalid media permit admitted");
    require(engine.stop(), "rejected engine stop failed");
    ++checks;
  }
  // Header-only metadata fixtures, not complete coded-frame/SPS certification.
  const QByteArray constrained = QByteArray::fromHex("0142c028ffe100");
  auto headerEvidence = [](const QByteArray &bytes, const char *label = "baseline") {
    auto *caps = gst_caps_new_simple(
        "video/x-h264", "stream-format", G_TYPE_STRING, "avc", "alignment", G_TYPE_STRING, "au", "profile",
        G_TYPE_STRING, label, nullptr
    );
    auto *buffer = gst_buffer_new_allocate(nullptr, bytes.size(), nullptr);
    gst_buffer_fill(buffer, 0, bytes.constData(), bytes.size());
    gst_caps_set_simple(caps, "codec_data", GST_TYPE_BUFFER, buffer, nullptr);
    gst_buffer_unref(buffer);
    const auto result = inspectH264CodecEvidence(caps);
    gst_caps_unref(caps);
    return result;
  };
  require(
      headerEvidence(constrained).state == H264Evidence::ConstrainedBaseline,
      "compatible label hid actual constrained baseline header"
  );
  ++checks;
  for (auto bytes :
       {QByteArray::fromHex("01428028ffe100"), QByteArray::fromHex("01640028ffe100"),
        QByteArray::fromHex("0042c028ffe100"), QByteArray::fromHex("0142c000ffe100"), constrained.left(6),
        QByteArray(65537, 'x')}) {
    require(
        headerEvidence(bytes, "constrained-baseline").state == H264Evidence::Rejected,
        "declared constrained baseline bypassed actual header gate"
    );
    ++checks;
  }
  auto *pending = gst_caps_from_string("video/x-h264,stream-format=avc,alignment=au,profile=constrained-baseline");
  require(
      inspectH264CodecEvidence(pending).state == H264Evidence::Pending,
      "missing header treated as actual profile evidence"
  );
  gst_caps_unref(pending);
  ++checks;

  {
    NativeWebRtcEngine engine;
    auto c = configuration(Direction::Send);
    c.preparation.allowLoopbackDiagnostics = false;
    require(!engine.prepare(c), "production loopback admitted");
    ++checks;
  }
  {
    NativeWebRtcEngine engine;
    auto c = configuration(Direction::Send);
    c.streamTopologyGeneration = 2;
    require(!engine.prepare(c), "source generation mismatch admitted");
    ++checks;
  }
  // Queue before asynchronous signaller startup, then validate using libnice.
  for (const char *candidate :
       {"candidate:1 1 UDP 2122260223 192.0.2.1 50000 typ host",
        "candidate:1 1 TCP 2122260223 127.0.0.1 50000 typ host tcptype active",
        "candidate:1 1 UDP 2122260223 127.0.0.1 50000 typ relay raddr 127.0.0.1 rport 1"}) {
    NativeWebRtcEngine engine;
    require(engine.prepare(configuration(Direction::Receive)) && engine.start(), "policy engine start");
    WorkerMediaSignal signal;
    signal.kind = WebRtcSignalKind::Ice;
    signal.candidate = QString::fromLatin1(candidate);
    engine.applySignal(signal);
    QElapsedTimer deadline;
    deadline.start();
    while (engine.error().isEmpty() && deadline.elapsed() < 2000) {
      engine.poll();
      QCoreApplication::processEvents();
      QThread::msleep(2);
    }
    require(engine.error() == "IceCandidateDenied", "unsafe ICE candidate was not rejected");
    require(engine.stop(), "policy engine stop");
    ++checks;
  }
  {
    NativeWebRtcEngine engine;
    require(engine.prepare(configuration(Direction::Receive)) && engine.start(), "map policy start");
    NativeFrameCorrelation m;
    m.sourceResource = QString(32, '5');
    m.topologyGeneration = m.epoch = m.frame = m.captureFrame = m.captureNanoseconds = 1;
    m.captureClockId = "lab.monotonic";
    m.ssrc = 10;
    m.rtpTimestamp = 20;
    auto wrong = m;
    wrong.topologyGeneration = 2;
    ControlError error;
    const auto decodedWrong = decodeFrameCorrelationBody(encodeFrameCorrelationBody(wrong, error), error);
    require(decodedWrong && !engine.applyCorrelation(*decodedWrong), "wrong generation mapping admitted");
    require(engine.applyCorrelation(m) && engine.applyCorrelation(m), "idempotent mapping rejected");
    wrong = m;
    wrong.captureNanoseconds = 2;
    require(
        !engine.applyCorrelation(wrong) && engine.error() == "FrameCorrelationConflict", "conflicting mapping admitted"
    );
    require(!engine.takeDecoded(), "map without RTP fabricated a decoded frame");
    require(engine.stop(), "map policy stop");
    checks += 4;
  }
  std::cout << "{\"policyChecks\":" << checks << ",\"passed\":true}\n";
}
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  try {
    require(
        argc == 2 || (argc == 3 && QString::fromLocal8Bit(argv[2]) == "--policy"),
        "usage: NativeWebRtcLoopback <private-runtime-root> [--policy]"
    );
    QString error;
    require(NativeWebRtcEngine::initializeRuntime(QString::fromLocal8Bit(argv[1]), error), error.toUtf8().constData());
    if (argc == 3) {
      policyChecks();
      return 0;
    }
    NativeWebRtcEngine tx, rx;
    auto send = configuration(Direction::Send), receive = configuration(Direction::Receive);
    LabControlLoopback control(send, receive);
    require(tx.prepare(send), qPrintable(tx.error()));
    require(rx.prepare(receive), qPrintable(rx.error()));
    require(rx.start(), qPrintable(rx.error()));
    require(tx.start(), qPrintable(tx.error()));
    auto budget = std::make_shared<FrameBudget>(3, 32 * 1024 * 1024);
    auto pool = std::make_shared<FramePool>(std::string(32, '6'), 1, send.binding.token.session, budget);
    QElapsedTimer elapsed;
    elapsed.start();
    qint64 next = 0;
    uint64_t sequence = 0, decoded = 0, markers = 0;
    std::shared_ptr<CpuFrameLease> heldDecoded, heldInput;
    while (elapsed.elapsed() < 8000) {
      QCoreApplication::processEvents();
      tx.poll();
      rx.poll();
      control.pump(tx, rx);
      if (elapsed.elapsed() >= next) {
        next = elapsed.elapsed() + 33;
        if (auto ticket = pool->reserve(8294400, MemoryDomain::Cpu)) {
          auto owner = std::make_shared<std::vector<uint32_t>>(1920 * 1080);
          uint32_t seed = uint32_t(++sequence);
          for (auto &pixel : *owner) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            pixel = seed | 0xff000000u;
          }
          for (size_t y = 100; y < 400; ++y)
            for (size_t x = 100; x < 600; ++x)
              (*owner)[y * 1920 + x] = 0xff28b43c;
          const void *data = owner->data();
          FrameAllocation allocation{owner, 8294400, MemoryDomain::Cpu, data};
          owner.reset();
          FrameDescriptor d;
          d.session = send.binding.token.session;
          d.sourceResource = send.streamSourceResource.toStdString();
          d.epoch = 1;
          d.topologyGeneration = 1;
          d.sequence = sequence;
          d.captureNanoseconds =
              uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
          d.captureClockId = "lab.monotonic";
          d.captureTimeValid = true;
          d.width = d.logicalWidth = 1920;
          d.height = d.logicalHeight = 1080;
          d.visible = {0, 0, 1920, 1080};
          d.planes = {{0, 7680, 8294400}};
          d.color = ColorProfile::SdrBt709Full8;
          require(
              pool->adopt(*ticket, std::move(allocation)) && pool->publish(*ticket, std::move(d)),
              "synthetic pool publish"
          );
          auto lease = CpuFrameLease::acquire(pool);
          heldInput = lease;
          require(lease && tx.submit(std::move(lease)), qPrintable(tx.error()));
        }
      }
      if (auto frame = rx.takeDecoded()) {
        require(
            frame->descriptor().sourceResource == send.streamSourceResource.toStdString() &&
                frame->descriptor().captureClockId == "remote.lab.monotonic" && frame->descriptor().captureTimeValid &&
                frame->descriptor().captureSequence > 0,
            "decoded source/capture evidence missing"
        );
        const auto *pixel =
            static_cast<const unsigned char *>(frame->data()) + 200 * frame->descriptor().planes[0].stride + 200 * 4;
        if (pixel[1] > 100 && pixel[1] > pixel[0] + 40 && pixel[1] > pixel[2] + 40)
          ++markers;
        heldDecoded = frame;
        ++decoded;
      }
      require(tx.error().isEmpty(), qPrintable(tx.error()));
      require(rx.error().isEmpty(), qPrintable(rx.error()));
      QThread::msleep(2);
    }
    const auto stats = tx.statistics(), received = rx.statistics();
    send.permit->revoke();
    receive.permit->revoke();
    WorkerMediaSignal deniedSignal;
    deniedSignal.kind = WebRtcSignalKind::Ice;
    deniedSignal.candidate = "candidate:1 1 UDP 2122260223 127.0.0.1 50000 typ host";
    NativeFrameCorrelation deniedMap;
    deniedMap.sourceResource = receive.streamSourceResource;
    deniedMap.topologyGeneration = receive.streamTopologyGeneration;
    deniedMap.epoch = receive.binding.token.epoch;
    deniedMap.frame = deniedMap.captureFrame = deniedMap.captureNanoseconds = 1;
    deniedMap.captureClockId = "lab.monotonic";
    require(
        validMediaSignal(deniedSignal) && validFrameCorrelation(deniedMap) && bool(heldInput), "invalid revoke fixture"
    );
    require(
        !tx.refresh() && !rx.takeDecoded() && !rx.applyCorrelation(deniedMap) && !tx.applySignal(deniedSignal) &&
            tx.takeSignals().empty() && tx.takeCorrelations().empty(),
        "revoked engine accepted new work"
    );
    require(!tx.submit(heldInput), "revoked submit accepted");
    tx.poll();
    rx.poll();
    require(tx.error() == "MediaPermitInactive" && rx.error() == "MediaPermitInactive", "revoke not observed");
    heldInput.reset();
    pool->retire();
    const bool stoppedTx = tx.stop();
    const bool leaseBlockedStop = heldDecoded && !rx.stop();
    heldDecoded.reset();
    const bool stoppedRx = rx.stopped();
    QJsonObject report{
        {"scope", "synthetic local engine/standard signaling/real RTP correlation only; no product authorization or "
                  "visible presentation"},
        {"serializedCorrelations", QString::number(control.serializedCorrelations())},
        {"decodedFrames", QString::number(decoded)},
        {"markerFrames", QString::number(markers)},
        {"leaseBlockedStop", leaseBlockedStop},
        {"revokedEntrypointsRejected", true},
        {"inputFrames", QString::number(stats.inputFrames)},
        {"receiverPublished", QString::number(received.decodedFrames)},
        {"receiverCompatibleProfileLabels", QString::number(received.compatibleProfileLabels)},
        {"constrainedBaseline", stats.constrainedBaseline},
        {"addressPairApproved", stats.addressPairApproved},
        {"gccUpdates", QString::number(stats.gccUpdates)},
        {"encoderUpdates", QString::number(stats.encoderUpdates)},
        {"stopped", stoppedTx && stoppedRx},
        {"sourceBudgetReleased", pool->drained() && budget->usage().bytes == 0}
    };
    std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
    require(
        control.serializedCorrelations() >= decoded && decoded >= 30 && markers >= 30 && leaseBlockedStop &&
            stats.constrainedBaseline && stats.addressPairApproved && stats.gccUpdates > 0 && stats.encoderUpdates > 0,
        "native media evidence incomplete"
    );
    require(stoppedTx && stoppedRx && pool->drained() && budget->usage().bytes == 0, "native media did not quiesce");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
