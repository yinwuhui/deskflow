// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/Protocol.h"
#include "display/Session.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <string>

using namespace deskflow::display;
using namespace std::chrono_literals;

static void require(bool condition, const char *reason)
{
  if (!condition) throw std::runtime_error(reason);
}

template <class F> static void protocolError(F&& operation)
{
  bool thrown = false;
  try { operation(); } catch (const ProtocolError&) { thrown = true; }
  require(thrown, "Expected protocol rejection");
}

template <class F> static void logicError(F&& operation)
{
  bool thrown = false;
  try { operation(); } catch (const std::logic_error&) { thrown = true; }
  require(thrown, "Expected state/argument rejection");
}

static SessionId identity(uint8_t last = 1)
{
  SessionId session{};
  session.back() = last;
  return session;
}

static Message packet(MessageType type, uint64_t sequence = 1, size_t bytes = 0)
{
  Message message;
  message.header = {type, static_cast<uint32_t>(bytes), identity(), 1, sequence};
  message.payload.resize(bytes, 0xA5);
  return message;
}

static void goldenHeader()
{
  Header header{MessageType::VideoFrame, 0x003FFFFF, {}, 0x01020304, 0x0102030405060708ULL};
  for (uint8_t i = 0; i < 16; ++i) header.session[i] = i;
  const std::array<uint8_t,40> expected = {
    0x44,0x46,0x56,0x44,1,0,0,0x80,0,0x3F,0xFF,0xFF,
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    1,2,3,4,1,2,3,4,5,6,7,8};
  require(encodeHeader(header) == expected, "Wire header differs from independently specified vector");
  require(decodeHeader(expected) == header, "Golden header did not decode");
}

static void fragmented()
{
  const auto message = packet(MessageType::Ping, 1, 5);
  const auto wire = encodeMessage(message);
  for (size_t split = 0; split <= wire.size(); ++split) {
    FrameReader reader(Channel::Control, identity());
    auto first = reader.feed(std::span(wire).first(split));
    auto second = reader.feed(std::span(wire).subspan(split));
    first.insert(first.end(), second.begin(), second.end());
    require(first.size() == 1 && first.front() == message, "Arbitrary TCP split lost frame");
    reader.finish();
  }
  FrameReader reader(Channel::Control);
  size_t count = 0;
  for (auto byte : wire) count += reader.feed(std::span(&byte, 1)).size();
  require(count == 1 && reader.bufferedBytes() == 0, "One-byte feeding did not drain");
}

static void coalesced()
{
  std::vector<uint8_t> wire;
  for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
    const auto frame = encodeMessage(packet(MessageType::Ping, sequence, sequence - 1));
    wire.insert(wire.end(), frame.begin(), frame.end());
  }
  FrameReader reader(Channel::Control);
  const auto messages = reader.feed(wire);
  require(messages.size() == 3 && messages[0].payload.empty() && messages[2].header.sequence == 3,
          "Coalesced or zero-payload frame lost");
  reader.finish();
}

static void malformedHeaders()
{
  for (size_t offset : {size_t(0),size_t(4),size_t(5),size_t(6)}) {
    auto wire = encodeMessage(packet(MessageType::Ping));
    wire[offset] = 0xFF;
    FrameReader reader(Channel::Control);
    protocolError([&]{ reader.feed(wire); });
    require(reader.failed(), "Bad header must poison parser");
    protocolError([&]{ reader.feed({}); });
  }
  auto wire = encodeMessage(packet(MessageType::Ping));
  std::fill(wire.begin()+32, wire.end(), 0);
  FrameReader reader(Channel::Control);
  protocolError([&]{ reader.feed(wire); });
}

static void truncated()
{
  const auto wire = encodeMessage(packet(MessageType::Ping, 1, 4));
  for (size_t length : {size_t(1),size_t(39),size_t(40),size_t(43)}) {
    FrameReader reader(Channel::Control);
    reader.feed(std::span(wire).first(length));
    protocolError([&]{ reader.finish(); });
  }
}

static void payloadLimits()
{
  for (auto type : {MessageType::Ping, MessageType::InputEvent, MessageType::VideoFrame}) {
    auto wire = encodeMessage(packet(type));
    wire[8]=0xFF; wire[9]=0xFF; wire[10]=0xFF; wire[11]=0xFF;
    FrameReader reader(type == MessageType::Ping ? Channel::Control :
                       (type == MessageType::VideoFrame ? Channel::Video : Channel::Input));
    protocolError([&]{ reader.feed(wire); });
    require(reader.bufferedBytes() <= headerSize, "Oversized advertised payload was allocated");
  }
  FrameReader reader(Channel::Control);
  std::vector<uint8_t> huge(controlPayloadLimit + 1);
  protocolError([&]{ reader.feed(huge); });
}

static void videoBoundary()
{
  const auto expected = packet(MessageType::VideoFrame, 1, videoPayloadLimit);
  const auto wire = encodeMessage(expected);
  FrameReader reader(Channel::Video, identity(), 1);
  size_t count = 0;
  for (size_t offset=0; offset<wire.size(); offset+=4093) {
    const auto messages = reader.feed(std::span(wire).subspan(offset, std::min(size_t(4093), wire.size()-offset)));
    if (!messages.empty()) require(messages.front() == expected, "Maximum legal video payload corrupted");
    count += messages.size();
    require(reader.bufferedBytes() <= headerSize + videoPayloadLimit, "Reader exceeded frame budget");
  }
  require(count == 1, "Maximum legal frame did not complete");
  reader.finish();
  protocolError([&]{ encodeMessage(packet(MessageType::VideoFrame, 1, videoPayloadLimit+1)); });
}

static void channelIsolation()
{
  for (auto channel : {Channel::Control, Channel::Input, Channel::Video}) {
    FrameReader reader(channel);
    const auto type = channel == Channel::Video ? MessageType::InputEvent : MessageType::VideoFrame;
    protocolError([&]{ reader.feed(encodeMessage(packet(type))); });
  }
  FrameReader reader(Channel::Video);
  require(reader.feed(encodeMessage(packet(MessageType::ChannelBind))).size() == 1, "First channel binding rejected");
  protocolError([&]{ reader.feed(encodeMessage(packet(MessageType::ChannelBind, 2))); });
}

static void replayIsolation()
{
  FrameReader reader(Channel::Control, identity());
  reader.feed(encodeMessage(packet(MessageType::Ping, 5)));
  protocolError([&]{ reader.feed(encodeMessage(packet(MessageType::Ping, 5))); });
  FrameReader wrongSession(Channel::Control, identity(2));
  protocolError([&]{ wrongSession.feed(encodeMessage(packet(MessageType::Ping))); });
  FrameReader wrongEpoch(Channel::Video, identity(), 2);
  protocolError([&]{ wrongEpoch.feed(encodeMessage(packet(MessageType::VideoFrame))); });
}

static void activate(SessionState& session)
{
  const auto generation = session.begin();
  require(!session.advance(generation, Event::FirstFrame), "Streaming before authorization permitted");
  for (auto event : {Event::Connected,Event::Authorized,Event::Agreed,Event::FirstFrame})
    require(session.advance(generation,event), "Valid activation rejected");
  require(session.state() == State::Streaming, "Session not streaming");
}

static void cancellation()
{
  SessionState session;
  const auto old = session.begin();
  const auto stopping = session.stop();
  require(stopping > old && session.stop() == stopping, "Stop must invalidate once and be idempotent");
  require(!session.advance(old,Event::Connected), "Cancelled callback advanced state");
  require(session.advance(stopping,Event::CleanupFailed), "Cleanup failure rejected");
  logicError([&]{ session.begin(); });
  require(session.advance(stopping,Event::CleanupSucceeded), "Retry cleanup failed");
  const auto current = session.begin();
  require(!session.advance(old,Event::Connected) && current > stopping, "Old retry callback accepted");
}

static void recovery()
{
  SessionState session;
  activate(session);
  auto generation = session.generation();
  require(session.advance(generation,Event::Reconfigure), "Reconfigure rejected");
  require(!session.advance(generation,Event::FirstFrame), "Old format callback completed reconfigure");
  generation = session.generation();
  require(session.advance(generation,Event::FirstFrame), "New config first frame rejected");
  require(session.advance(generation,Event::Interrupted), "Recovery rejected");
  require(!session.advance(generation,Event::FirstFrame), "Stale capture resurrected session");
  generation = session.generation();
  require(session.advance(generation,Event::Reconnected), "Reconnect rejected");
  require(session.state() == State::Preparing, "Reconnect skipped resource preparation");
}

static void leaseOwnership()
{
  InputLease lease;
  const auto now = InputLease::TimePoint{};
  const PressedInput key{PressedInput::Kind::Key, 4};
  lease.grant(identity(),1,1,now,2s);
  require(!lease.accept(identity(2),1,1,now,key,true), "Wrong owner accepted");
  require(!lease.accept(identity(),2,1,now,key,true), "Wrong lease accepted");
  require(!lease.accept(identity(),1,2,now,key,true), "Old epoch accepted");
  require(lease.accept(identity(),1,1,now,key,true), "Valid key rejected");
  require(lease.accept(identity(),1,1,now,key,true), "Repeated key-down rejected");
  const auto released = lease.revoke();
  require(released.size()==1 && released[0]==key && lease.revoke().empty(), "ReleaseAll duplicated or lost key");
  logicError([&]{ lease.grant(identity(),1,1,now,2s); });
}

static void leaseExpiry()
{
  InputLease lease;
  const auto now = InputLease::TimePoint{};
  const PressedInput button{PressedInput::Kind::Button,1};
  lease.grant(identity(),1,1,now,2s);
  lease.accept(identity(),1,1,now,button,true);
  require(!lease.accept(identity(),1,1,now+2s,button,false), "Expired input accepted");
  require(!lease.renew(identity(),1,1,now+2s,2s), "Expired lease resurrected by renewal");
  require(lease.expire(now+2s).size()==1 && lease.expire(now+3s).empty(), "Expiration did not release exactly once");
  lease.grant(identity(),2,2,now+3s,2s);
  lease.accept(identity(),2,2,now+3s,button,true);
  require(lease.grant(identity(2),3,2,now+4s,2s).size()==1, "Handoff failed to release old input");
  logicError([&]{ lease.grant(identity(),4,1,now,3s); });
}

int main(int argc, char **argv)
{
  const std::pair<const char *, std::function<void()>> tests[] = {
    {"GoldenHeader",goldenHeader},{"Fragmented",fragmented},{"Coalesced",coalesced},
    {"MalformedHeaders",malformedHeaders},{"Truncated",truncated},{"PayloadLimits",payloadLimits},
    {"VideoBoundary",videoBoundary},{"ChannelIsolation",channelIsolation},{"ReplayIsolation",replayIsolation},
    {"Cancellation",cancellation},{"Recovery",recovery},{"LeaseOwnership",leaseOwnership},{"LeaseExpiry",leaseExpiry}};
  size_t count=0;
  for (const auto& [name,test] : tests) {
    if (argc>1 && std::string(argv[1])!=name) continue;
    try { test(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& error) { std::cerr << "FAIL " << name << ": " << error.what() << '\n'; return 1; }
    ++count;
  }
  return count ? 0 : 2;
}
