// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/supervision/WorkerProtocol.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QtEndian>
#include <iostream>
#include <stdexcept>
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
  if (direction == Direction::Receive) {
    p.sourceResource.clear();
    p.source = {};
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
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  try {
    require(argc == 2, "case required");
    const std::string c = argv[1];
    if (c == "WorkerHandshake")
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
    else if (c == "WorkerSignalBudgets")
      signalBudgets();
    else if (c == "WorkerSignalDirection")
      signalDirection();
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
