// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/supervision/MediaWorkPermit.h"
#include "display/supervision/WorkerProtocol.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QtEndian>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace deskflow::display::infra;
static void require(bool b, const char *why)
{
  if (!b)
    throw std::runtime_error(why);
}
static WorkerBinding binding(Direction direction = Direction::Send)
{
  return {
      {std::string(32, '1'), 1, 1, 1, std::string(32, '2'), std::string(32, '3')},
      "uid501.console",
      "infra1",
      "webrtc",
      "macos.webrtc.h264",
      QString(64, 'a'),
      QString(64, 'b'),
      QString(64, 'c'),
      QString(64, 'd'),
      direction
  };
}
static WorkerPrepare preparation(TimePoint now, Direction direction = Direction::Send)
{
  WorkerPrepare p;
  p.sourceResource = QString(32, '4');
  p.source = {NativeSourceKind::CgDisplay, 77, 0};
  p.sourceIdentity = "11111111-2222-3333-4444-555555555555";
  p.sourceContextDigest = QString(64, 'a');
  p.sourceTopologyGeneration = 1;
  if (direction == Direction::Receive) {
    p.sourceResource.clear();
    p.source = {};
    p.sourceIdentity.clear();
    p.sourceContextDigest.clear();
    p.sourceTopologyGeneration = 0;
  }
  p.allowedPeerAddresses = {"10.20.1.71"};
  p.startupDeadlineNanoseconds = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>((now + std::chrono::seconds(10)).time_since_epoch()).count()
  );
  return p;
}
struct Pair
{
  TimePoint now{};
  Direction direction;
  WorkerProtocol a, w;
  explicit Pair(Direction d = Direction::Send)
      : direction(d),
        a(WorkerSide::Agent, binding(d), QByteArray(32, 'x'), now),
        w(WorkerSide::Worker, binding(d), QByteArray(32, 'x'), now)
  {
  }
  void connect()
  {
    const auto ah = a.hello(), wh = w.hello();
    require(!ah.isEmpty() && !wh.isEmpty(), "missing hello");
    require(a.feed(wh, true, now) && w.feed(ah, true, now), "hello rejected");
    require(a.established() && w.established(), "handshake incomplete");
  }
  void start()
  {
    connect();
    require(w.feed(a.encode(preparation(now, direction), now), true, now), "prepare rejected");
    w.takeMessages();
    require(a.feed(w.encode(WorkerReady{}, now), true, now), "ready rejected");
    a.takeMessages();
    require(w.feed(a.encode(WorkerStart{}, now), true, now), "start rejected");
    w.takeMessages();
  }
};
static void handshake()
{
  Pair p;
  p.connect();
  require(p.a.hello().isEmpty(), "hello repeated");
  Pair wrong;
  WorkerProtocol fake(WorkerSide::Worker, binding(), QByteArray(32, 'z'), wrong.now);
  wrong.a.hello();
  require(!wrong.a.feed(fake.hello(), true, wrong.now), "wrong launch secret accepted");
  Pair untrusted;
  const auto a = untrusted.a.hello(), w = untrusted.w.hello();
  require(!untrusted.a.feed(w, false, untrusted.now), "unverified OS transport accepted");
  require(!a.isEmpty(), "missing agent hello");
}
static void bindingGates()
{
  for (int field = 0; field < 12; ++field) {
    auto b = binding();
    switch (field) {
    case 0:
      b.token.worker = std::string(32, '4');
      break;
    case 1:
      b.token.owner = std::string(32, '4');
      break;
    case 2:
      b.token.generation = 2;
      break;
    case 3:
      b.token.epoch = 2;
      break;
    case 4:
      b.planHash = QString(64, 'b');
      break;
    case 5:
      b.buildCompatibility = "infra2";
      break;
    case 6:
      b.localUserSession = "uid502.console";
      break;
    case 7:
      b.backend = "rdp";
      break;
    case 8:
      b.localIdentity = QString(64, 'e');
      break;
    case 9:
      b.peerIdentity = QString(64, 'e');
      break;
    case 10:
      b.controlBinding = QString(64, 'e');
      break;
    case 11:
      b.direction = Direction::Receive;
      break;
    }
    WorkerProtocol a(WorkerSide::Agent, binding(), QByteArray(32, 'x'), {}),
        w(WorkerSide::Worker, b, QByteArray(32, 'x'), {});
    a.hello();
    require(!a.feed(w.hello(), true, {}), "mismatched scope accepted");
  }
  WorkerProtocol invalid(WorkerSide::Agent, binding(), QByteArray(31, 'x'), {});
  require(invalid.failed(), "short secret accepted");
}
static void framing()
{
  Pair p;
  const auto ah = p.a.hello(), wh = p.w.hello();
  for (char c : wh)
    require(p.a.feed(QByteArray(1, c), true, p.now), "fragment rejected");
  require(p.w.feed(ah, true, p.now), "agent hello rejected");
  const auto one = p.w.encode(WorkerHeartbeat{}, p.now), two = p.w.encode(WorkerHeartbeat{}, p.now);
  require(p.a.feed(one + two, true, p.now) && p.a.takeMessages().size() == 2, "coalesced IPC lost");
  QByteArray oversized(4, Qt::Uninitialized);
  qToBigEndian(quint32(WorkerProtocol::MaxMessageBytes + 1), oversized.data());
  require(!p.a.feed(oversized, true, p.now), "oversized header accepted");
  Pair huge;
  huge.connect();
  require(!huge.a.feed(QByteArray(WorkerProtocol::MaxMessageBytes + 5, 'x'), true, {}), "oversized read accepted");
}
static void state()
{
  Pair p;
  p.connect();
  require(p.a.encode(WorkerStart{}, p.now).isEmpty() && p.a.failed(), "start before ready accepted");
  Pair wrong;
  wrong.connect();
  require(wrong.w.encode(WorkerStart{}, wrong.now).isEmpty(), "worker issued start");
  Pair live;
  live.start();
  const WorkerPresented frame{1, PresentationEvidence::NativePaintConfirmed, true};
  require(live.a.feed(live.w.encode(frame, live.now), true, live.now), "presentation rejected");
  auto events = live.a.takeMessages();
  require(events.size() == 1 && std::get<WorkerPresented>(events.front()) == frame, "typed frame corrupted");
  require(live.w.feed(live.a.encode(WorkerStop{StopReason::Revoked}, live.now), true, live.now), "stop rejected");
  require(live.a.feed(live.w.encode(WorkerQuiesced{}, live.now), true, live.now), "quiescence rejected");
  require(!live.a.encode(WorkerStart{}, live.now).size(), "restart after stop accepted");
  Pair race;
  race.start();
  const auto inFlight = race.w.encode(WorkerPresented{1, PresentationEvidence::Submitted, false}, {});
  const auto stop = race.a.encode(WorkerStop{}, {});
  require(
      race.a.feed(inFlight, true, {}) && race.a.takeMessages().empty(),
      "in-flight frame was dispatched or killed graceful stop"
  );
  require(race.w.feed(stop, true, {}), "stop race rejected");
  Pair spontaneous;
  spontaneous.start();
  require(spontaneous.w.encode(WorkerQuiesced{}, {}).isEmpty(), "unsolicited cleanup accepted");
}
static void replay()
{
  Pair p;
  p.start();
  const auto event = p.w.encode(WorkerPresented{1, PresentationEvidence::Submitted, false}, p.now);
  require(p.a.feed(event, true, p.now), "event rejected");
  p.a.takeMessages();
  require(!p.a.feed(event, true, p.now), "IPC replay accepted");
  Pair old, fresh;
  old.start();
  fresh.start();
  require(!fresh.a.feed(old.w.encode(WorkerHeartbeat{}, {}), true, {}), "old launch traffic accepted");
  Pair corrupt;
  corrupt.start();
  auto bytes = corrupt.w.encode(WorkerHeartbeat{}, {});
  bytes[bytes.size() - 3] ^= 1;
  require(!corrupt.a.feed(bytes, true, {}), "modified message accepted");
}
static void deadlines()
{
  Pair startup;
  startup.a.tick(startup.now + std::chrono::seconds(3));
  require(startup.a.failed(), "handshake deadline renewed");
  Pair p;
  p.start();
  p.now += std::chrono::milliseconds(1000);
  require(p.a.feed(p.w.encode(WorkerHeartbeat{}, p.now), true, p.now), "heartbeat rejected");
  // Sending does not renew the receive lease; a silent agent cannot keep worker alive.
  p.w.tick(p.now + std::chrono::milliseconds(500));
  require(p.w.failed(), "outbound traffic renewed lease");
  p.a.tick(p.now + std::chrono::milliseconds(1499));
  require(!p.a.failed(), "inbound lease expired early");
  p.a.tick(p.now + std::chrono::milliseconds(1500));
  require(p.a.failed(), "lease did not expire");
  Pair eof;
  eof.start();
  eof.w.disconnected();
  require(eof.w.failed() && !eof.w.established(), "parent EOF ignored");
}
static void queueBudget()
{
  Pair p;
  p.start();
  for (int i = 0; i < WorkerProtocol::MaxPendingMessages; ++i)
    require(p.a.feed(p.w.encode(WorkerHeartbeat{}, {}), true, {}), "queue filled too early");
  require(!p.a.feed(p.w.encode(WorkerHeartbeat{}, {}), true, {}), "unbounded IPC queue");
  require(p.a.takeMessages().empty(), "failed channel retained actionable commands");
  Pair f;
  f.start();
  require(f.a.feed(f.w.encode(WorkerFailure{"DeviceLost"}, {}), true, {}), "failure rejected");
  require(f.a.feed(f.w.encode(WorkerQuiesced{}, {}), true, {}), "failure cleanup rejected");
}
static void prepareContract()
{
  TimePoint now{};
  auto valid = preparation(now);
  require(validWorkerPrepare(valid, Direction::Send, now), "valid prepare rejected");
  auto receive = preparation(now, Direction::Receive);
  require(validWorkerPrepare(receive, Direction::Receive, now), "receive contract rejected");
  for (int i = 0; i < 10; ++i) {
    auto p = valid;
    switch (i) {
    case 0:
      p.rawFrameSlots = 7;
      break;
    case 1:
      p.rawFrameBytes = 16;
      break;
    case 2:
      p.allowedPeerAddresses = {"peer.example.com"};
      break;
    case 3:
      p.allowedPeerAddresses = {"127.0.0.1"};
      break;
    case 4:
      p.source.primary = 0;
      break;
    case 5:
      p.encodedQueueBytes = UINT64_MAX;
      break;
    case 6:
      p.security = SecurityProfile::RdpAuthenticatedTls13;
      break;
    case 7:
      p.startupDeadlineNanoseconds = 0;
      break;
    case 8:
      p.allowedPeerAddresses = {"0.0.0.0"};
      break;
    case 9:
      p.allowedPeerAddresses = {"10.20.1.71", "10.20.1.71"};
      break;
    }
    require(!validWorkerPrepare(p, Direction::Send, now), "unsafe native configuration accepted");
  }
  Pair noPrepare;
  noPrepare.connect();
  require(noPrepare.w.encode(WorkerReady{}, now).isEmpty(), "ready without explicit config");
  Pair once;
  once.start();
  require(once.a.encode(valid, now).isEmpty(), "configuration replaced after ready");
}
static void sourceBindingContract()
{
  TimePoint now{};
  const auto source = preparation(now);
  Pair pair;
  pair.connect();
  require(pair.w.feed(pair.a.encode(source, now), true, now), "source evidence IPC rejected");
  const auto messages = pair.w.takeMessages();
  require(
      messages.size() == 1 && std::get<WorkerPrepare>(messages.front()) == source,
      "source identity/topology evidence changed across IPC"
  );
  for (int i = 0; i < 7; ++i) {
    auto p = source;
    switch (i) {
    case 0:
      p.sourceIdentity.clear();
      break;
    case 1:
      p.sourceIdentity = "../../display";
      break;
    case 2:
      p.sourceIdentity = "11111111-2222-3333-4444-55555555555z";
      break;
    case 3:
      p.sourceIdentity = "{11111111-2222-3333-4444-555555555555}";
      break;
    case 4:
      p.sourceContextDigest.clear();
      break;
    case 5:
      p.sourceContextDigest = QString(64, 'z');
      break;
    case 6:
      p.sourceTopologyGeneration = 0;
      break;
    }
    require(!validWorkerPrepare(p, Direction::Send, now), "unbound capture source accepted");
  }
  auto receiver = preparation(now, Direction::Receive);
  receiver.sourceIdentity = source.sourceIdentity;
  require(!validWorkerPrepare(receiver, Direction::Receive, now), "receiver accepted source identity");
  receiver = preparation(now, Direction::Receive);
  receiver.sourceTopologyGeneration = 1;
  require(!validWorkerPrepare(receiver, Direction::Receive, now), "receiver accepted topology authority");
  auto linuxSource = source;
  linuxSource.source.kind = NativeSourceKind::PipeWireNode;
  linuxSource.sourceIdentity = "pipewire.node.serial.42";
  require(validWorkerPrepare(linuxSource, Direction::Send, now), "typed PipeWire evidence rejected");
}
static void signalBudgets()
{
  Pair p;
  p.start();
  WorkerMediaSignal s;
  s.kind = WebRtcSignalKind::Offer;
  s.description = QString(32768, 'x');
  const auto bytes = p.w.encode(s, p.now);
  require(bytes.size() > 8192 && bytes.size() <= 65540, "standard SDP cannot cross bounded IPC");
  require(p.a.feed(bytes, true, p.now), "bounded SDP rejected");
  const auto messages = p.a.takeMessages();
  require(messages.size() == 1 && std::get<WorkerMediaSignal>(messages.front()) == s, "SDP payload changed");
  Pair huge;
  huge.start();
  s.description.append('x');
  require(huge.w.encode(s, huge.now).isEmpty(), "oversized SDP accepted");
  Pair ice;
  ice.start();
  WorkerMediaSignal c;
  c.kind = WebRtcSignalKind::Ice;
  c.candidate = "candidate:1 1 UDP 2122260223 10.20.1.71 54321 typ host";
  for (int i = 0; i < 64; ++i) {
    require(ice.a.feed(ice.w.encode(c, ice.now), true, ice.now), "candidate budget too small");
    ice.a.takeMessages();
  }
  require(ice.w.encode(c, ice.now).isEmpty(), "unbounded ICE candidates");
  c.candidate.append('\n');
  require(!validMediaSignal(c), "candidate line injection accepted");
}
static void signalDirection()
{
  WorkerMediaSignal offer;
  offer.kind = WebRtcSignalKind::Offer;
  offer.description = "v=0\r\n";
  auto answer = offer;
  answer.kind = WebRtcSignalKind::Answer;
  Pair send;
  send.start();
  require(send.a.feed(send.w.encode(offer, send.now), true, send.now), "sender offer rejected");
  send.a.takeMessages();
  require(send.w.feed(send.a.encode(answer, send.now), true, send.now), "sender answer rejected");
  Pair wrong;
  wrong.start();
  require(wrong.a.encode(offer, wrong.now).isEmpty(), "agent impersonated local sender SDP");
  Pair receiver(Direction::Receive);
  receiver.start();
  require(
      receiver.w.feed(receiver.a.encode(offer, receiver.now), true, receiver.now), "receiver remote offer rejected"
  );
  require(
      receiver.a.feed(receiver.w.encode(answer, receiver.now), true, receiver.now), "receiver local answer rejected"
  );
  auto changed = offer;
  changed.description.append("s=new\r\n");
  require(send.w.encode(changed, send.now).isEmpty(), "implicit renegotiation accepted");
}
static void preparationDeadline()
{
  Pair p;
  p.connect();
  auto config = preparation(p.now);
  config.startupDeadlineNanoseconds = 1000000000;
  require(p.w.feed(p.a.encode(config, p.now), true, p.now), "timed prepare");
  p.w.takeMessages();
  p.now += std::chrono::milliseconds(900);
  require(p.a.feed(p.w.encode(WorkerReady{}, p.now), true, p.now), "ready before deadline");
  p.now += std::chrono::milliseconds(100);
  p.a.tick(p.now);
  p.w.tick(p.now);
  require(
      p.a.error() == "IpcPreparationDeadline" && p.w.error() == "IpcPreparationDeadline",
      "Ready renewed absolute preparation deadline"
  );
}
static WorkerBinding platformBinding()
{
  auto b = binding();
  b.purpose = WorkerPurpose::Platform;
  b.backend = "platform.macos.virtual-output";
  b.profile = "macos.virtual-output.cgdisplay.sdr1080.v1";
  b.peerIdentity.clear();
  b.controlBinding.clear();
  return b;
}
struct PlatformPair
{
  WorkerProtocol a{WorkerSide::Agent, platformBinding(), QByteArray(32, 'x'), {}};
  WorkerProtocol w{WorkerSide::Worker, platformBinding(), QByteArray(32, 'x'), {}};
  WorkerPrepareOutput config{QString(32, '4'), QString(32, '5'), QString(64, '6'), 10000000000ULL};
  WorkerOutputObserved claim{QString(32, '4'), "native-display-uuid", {NativeSourceKind::CgDisplay, 77, 0}, 1};
  void connect()
  {
    auto ah = a.hello(), wh = w.hello();
    require(a.feed(wh, true, {}) && w.feed(ah, true, {}), "platform handshake");
  }
  void start()
  {
    connect();
    require(w.feed(a.encode(config, {}), true, {}), "platform prepare");
    w.takeMessages();
    require(a.feed(w.encode(WorkerReady{}, {}), true, {}), "platform ready");
    a.takeMessages();
    require(w.feed(a.encode(WorkerStart{}, {}), true, {}), "platform start");
    w.takeMessages();
  }
};
static void platformContract()
{
  PlatformPair p;
  p.start();
  require(p.a.feed(p.w.encode(p.claim, {}), true, {}), "owned output claim");
  auto messages = p.a.takeMessages();
  require(messages.size() == 1 && std::get<WorkerOutputObserved>(messages.front()) == p.claim, "claim changed");
  require(p.w.feed(p.a.encode(WorkerConfigureOutput{p.config.resourceId}, {}), true, {}), "layout permission");
  require(p.a.feed(p.w.encode(WorkerOutputReady{p.config.resourceId}, {}), true, {}), "layout verification");
  PlatformPair early;
  early.start();
  require(early.a.encode(WorkerConfigureOutput{early.config.resourceId}, {}).isEmpty(), "layout before ownership");
  PlatformPair wrong;
  wrong.start();
  wrong.claim.source.kind = NativeSourceKind::PipeWireNode;
  require(wrong.w.encode(wrong.claim, {}).isEmpty(), "foreign backend source accepted");
  PlatformPair race;
  race.start();
  auto late = race.w.encode(race.claim, {});
  auto stop = race.a.encode(WorkerStop{}, {});
  require(race.w.feed(stop, true, {}), "platform stop race");
  require(race.a.feed(late, true, {}), "late resource ownership rejected");
  require(race.a.takeMessages().size() == 1, "late ownership suppressed instead of dispatched for cleanup");
  require(race.a.encode(WorkerConfigureOutput{race.config.resourceId}, {}).isEmpty(), "revoked layout accepted");
  PlatformPair lateReady;
  lateReady.start();
  require(lateReady.a.feed(lateReady.w.encode(lateReady.claim, {}), true, {}), "ready race ownership");
  lateReady.a.takeMessages();
  require(
      lateReady.w.feed(lateReady.a.encode(WorkerConfigureOutput{lateReady.config.resourceId}, {}), true, {}),
      "ready race configure"
  );
  const auto prepared = lateReady.w.encode(WorkerOutputReady{lateReady.config.resourceId}, {});
  lateReady.a.encode(WorkerStop{}, {});
  require(
      lateReady.a.feed(prepared, true, {}) && lateReady.a.takeMessages().empty(), "late ready dispatched after stop"
  );
  auto config = p.config;
  config.width = 3840;
  require(!validOutputPreparation(config, {}), "unbounded native mode accepted");
}
static void purposeIsolation()
{
  PlatformPair p;
  p.connect();
  require(p.a.encode(preparation({}), {}).isEmpty(), "media configuration entered platform helper");
  PlatformPair frame;
  frame.start();
  require(
      frame.w.encode(WorkerPresented{1, PresentationEvidence::Submitted, false}, {}).isEmpty(),
      "platform reported media"
  );
  Pair media;
  media.start();
  require(media.w.encode(PlatformPair{}.claim, {}).isEmpty(), "media worker claimed output ownership");
  auto invalid = platformBinding();
  invalid.peerIdentity = QString(64, 'c');
  WorkerProtocol bad(WorkerSide::Agent, invalid, QByteArray(32, 'x'), {});
  require(bad.failed(), "platform silently accepted media peer scope");
  WorkerProtocol agent(WorkerSide::Agent, platformBinding(), QByteArray(32, 'x'), {});
  WorkerProtocol worker(WorkerSide::Worker, binding(), QByteArray(32, 'x'), {});
  agent.hello();
  require(!agent.feed(worker.hello(), true, {}), "cross-purpose hello accepted");
}
static void mediaPermit()
{
  const TimePoint now{};
  MediaWorkContext c;
  c.binding = binding();
  c.preparation = preparation(now);
  c.localAddress = "10.20.1.72";
  c.streamSourceResource = c.preparation.sourceResource;
  c.streamTopologyGeneration = 1;
  const auto expires = now + std::chrono::seconds(20);
  auto permit = MediaWorkPermit::issue(c, expires, now);
  require(bool(permit) && permit->matches(c, now), "valid permit rejected");
  require(!permit->active(expires) && !permit->active(now - std::chrono::nanoseconds(1)), "time boundary accepted");
  require(!MediaWorkPermit::issue(c, now, now), "expired issue accepted");
  for (int field = 0; field < 12; ++field) {
    auto other = c;
    switch (field) {
    case 0:
      ++other.binding.token.attempt;
      break;
    case 1:
      ++other.binding.token.epoch;
      break;
    case 2:
      other.binding.token.owner = std::string(32, '9');
      break;
    case 3:
      other.binding.planHash = QString(64, '9');
      break;
    case 4:
      other.binding.controlBinding = QString(64, '9');
      break;
    case 5:
      other.binding.direction = Direction::Receive;
      break;
    case 6:
      other.preparation.source.primary++;
      break;
    case 7:
      ++other.streamTopologyGeneration;
      break;
    case 8:
      other.localAddress = "10.20.1.73";
      break;
    case 9:
      other.streamSourceResource = QString(32, '9');
      break;
    case 10:
      other.binding.localUserSession = "other.session";
      break;
    case 11:
      ++other.preparation.rawFrameBytes;
      break;
    }
    require(!permit->matches(other, now), "scope mutation accepted");
  }
  const auto &b = c.binding;
  const auto &p = c.preparation;
  auto capture = [&](uint64_t generation) {
    return permit->allowsCapture(
        b.token.owner, b.token.session, b.token.epoch, b.planHash, b.localUserSession, p.sourceResource, p.source,
        p.sourceIdentity, p.sourceContextDigest, generation, now
    );
  };
  require(capture(1) && !capture(2), "capture generation scope failed");
  auto invalid = c;
  invalid.localAddress = "0.0.0.0";
  require(!MediaWorkPermit::issue(invalid, expires, now), "wildcard accepted");
  invalid = c;
  invalid.streamSourceResource = QString(32, '9');
  require(!MediaWorkPermit::issue(invalid, expires, now), "foreign stream accepted");
  invalid = c;
  invalid.binding.token.owner.clear();
  require(!MediaWorkPermit::issue(invalid, expires, now), "invalid binding accepted");
  auto receiver = c;
  receiver.binding = binding(Direction::Receive);
  receiver.preparation = preparation(now, Direction::Receive);
  auto rp = MediaWorkPermit::issue(receiver, expires, now);
  require(
      bool(rp) && !rp->allowsCapture(
                      b.token.owner, b.token.session, b.token.epoch, b.planHash, b.localUserSession, p.sourceResource,
                      p.source, p.sourceIdentity, p.sourceContextDigest, 1, now
                  ),
      "receiver allowed capture"
  );
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i)
    threads.emplace_back([&] {
      for (int j = 0; j < 1000; ++j) {
        permit->active(now);
        permit->revoke();
      }
    });
  for (auto &t : threads)
    t.join();
  require(!permit->active(now) && !permit->matches(c, now) && !capture(1), "revocation reversed");
}
static void receiverBinding()
{
  auto setup = [](Pair &p) {
    p.connect();
    require(p.w.feed(p.a.encode(preparation(p.now, Direction::Receive), p.now), true, p.now), "prepare");
    p.w.takeMessages();
    require(
        p.w.feed(
            p.a.encode(WorkerReceiveBinding{"10.20.1.72", QString(32, '4'), QString(32, '5'), 1}, p.now), true, p.now
        ),
        "target"
    );
    require(
        std::get<WorkerReceiveBinding>(p.w.takeMessages().front()).targetResource == QString(32, '5'),
        "target roundtrip"
    );
    require(p.a.feed(p.w.encode(WorkerReady{}, p.now), true, p.now), "ready");
    p.a.takeMessages();
    require(p.w.feed(p.a.encode(WorkerStart{}, p.now), true, p.now), "start");
    p.w.takeMessages();
  };
  WebRtcFrameCorrelation map{QString(32, '4'), "capture.clock", 1, 1, 1, 1, 1, 0, UINT32_MAX, false};
  Pair valid(Direction::Receive);
  setup(valid);
  require(valid.w.feed(valid.a.encode(map, valid.now), true, valid.now), "correlation wire");
  require(std::get<WebRtcFrameCorrelation>(valid.w.takeMessages().front()) == map, "correlation roundtrip");
  require(valid.w.feed(valid.a.encode(WorkerStop{}, valid.now), true, valid.now), "stop");
  require(valid.a.encode(map, valid.now).isEmpty(), "correlation after stop");
  for (int field = 0; field < 3; ++field) {
    Pair wrong(Direction::Receive);
    setup(wrong);
    auto bad = map;
    if (field == 0)
      bad.sourceResource = QString(32, '6');
    if (field == 1)
      ++bad.topologyGeneration;
    if (field == 2)
      ++bad.epoch;
    require(wrong.a.encode(bad, wrong.now).isEmpty(), "wrong correlation scope");
  }
  Pair early(Direction::Receive);
  early.start();
  require(early.a.encode(map, early.now).isEmpty(), "unbound correlation");
}
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  try {
    require(argc == 2, "case required");
    const std::string c = argv[1];
    if (c == "WorkerReceiveBinding")
      receiverBinding();
    else if (c == "MediaWorkPermit")
      mediaPermit();
    else if (c == "WorkerHandshake")
      handshake();
    else if (c == "WorkerBinding")
      bindingGates();
    else if (c == "WorkerFraming")
      framing();
    else if (c == "WorkerState")
      state();
    else if (c == "WorkerReplay")
      replay();
    else if (c == "WorkerDeadlines")
      deadlines();
    else if (c == "WorkerQueue")
      queueBudget();
    else if (c == "WorkerPreparationContract")
      prepareContract();
    else if (c == "WorkerSourceBinding")
      sourceBindingContract();
    else if (c == "WorkerSignalBudgets")
      signalBudgets();
    else if (c == "WorkerSignalDirection")
      signalDirection();
    else if (c == "WorkerPlatformContract")
      platformContract();
    else if (c == "WorkerPurposeIsolation")
      purposeIsolation();
    else if (c == "WorkerPreparationDeadline")
      preparationDeadline();
    else
      throw std::runtime_error("unknown case");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
