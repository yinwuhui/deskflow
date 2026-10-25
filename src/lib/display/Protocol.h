// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace deskflow::display {

using SessionId = std::array<uint8_t, 16>;
constexpr size_t headerSize = 40;
constexpr uint32_t controlPayloadLimit = 64 * 1024;
constexpr uint32_t inputPayloadLimit = 4 * 1024;
constexpr uint32_t videoPayloadLimit = 4 * 1024 * 1024;

enum class Channel { Control, Input, Video };
enum class MessageType : uint16_t {
  Hello = 1, Capabilities = 2, StartRequest = 3, StartAccepted = 4,
  StartRejected = 5, StreamConfig = 6, StreamReady = 7, PresentAck = 8,
  RequestKeyframe = 9, InputLeaseRequest = 10, InputLeaseGrant = 11,
  InputLeaseRevoke = 12, Reconfigure = 13, ReconfigureAck = 14,
  Stats = 15, Ping = 16, Pong = 17, Stop = 18, Stopped = 19,
  ChannelBind = 20, InputEvent = 64, ReleaseAll = 65, VideoFrame = 128
};

struct Header {
  MessageType type = MessageType::Hello;
  uint32_t payloadLength = 0;
  SessionId session{};
  uint32_t epoch = 0;
  uint64_t sequence = 1;
  bool operator==(const Header&) const = default;
};

struct Message {
  Header header;
  std::vector<uint8_t> payload;
  bool operator==(const Message&) const = default;
};

class ProtocolError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

std::array<uint8_t, headerSize> encodeHeader(const Header& header);
Header decodeHeader(std::span<const uint8_t> bytes);
std::vector<uint8_t> encodeMessage(Message message);

// One reader per TLS connection. Authentication, channel binding, JSON schema,
// codec validation and operation authorization must happen above this layer.
// Any parse error poisons the connection; callers must close it, not resync.
class FrameReader {
public:
  explicit FrameReader(Channel channel, std::optional<SessionId> session = {},
                       std::optional<uint32_t> epoch = {});
  std::vector<Message> feed(std::span<const uint8_t> bytes);
  void finish();
  bool failed() const { return m_failed; }
  size_t bufferedBytes() const { return m_headerBytes + m_payload.size(); }

private:
  [[noreturn]] void reject(const char *reason);
  void validate(const Header& header);
  Channel m_channel;
  std::optional<SessionId> m_session;
  std::optional<uint32_t> m_epoch;
  std::array<uint8_t, headerSize> m_headerBuffer{};
  size_t m_headerBytes = 0;
  std::optional<Header> m_header;
  std::vector<uint8_t> m_payload;
  uint64_t m_lastSequence = 0;
  bool m_failed = false;
};

} // namespace deskflow::display
