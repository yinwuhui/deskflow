// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "Protocol.h"

#include <algorithm>
#include <limits>

namespace deskflow::display {
namespace {
void put(std::span<uint8_t> bytes, size_t offset, uint64_t value, size_t width)
{
  for (size_t i = 0; i < width; ++i) bytes[offset + width - 1 - i] = static_cast<uint8_t>(value >> (i * 8));
}

uint64_t get(std::span<const uint8_t> bytes, size_t offset, size_t width)
{
  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i) value = (value << 8) | bytes[offset + i];
  return value;
}

uint32_t limit(MessageType type)
{
  const auto value = static_cast<uint16_t>(type);
  if (value >= 1 && value <= 20) return controlPayloadLimit;
  if (type == MessageType::InputEvent || type == MessageType::ReleaseAll) return inputPayloadLimit;
  if (type == MessageType::VideoFrame) return videoPayloadLimit;
  throw ProtocolError("Unknown message type");
}

void validateHeader(const Header& header)
{
  if (header.payloadLength > limit(header.type)) throw ProtocolError("Payload exceeds type limit");
  if (!header.sequence) throw ProtocolError("Sequence must be nonzero");
}
} // namespace

std::array<uint8_t, headerSize> encodeHeader(const Header& header)
{
  validateHeader(header);
  std::array<uint8_t, headerSize> bytes{};
  bytes[0] = 'D'; bytes[1] = 'F'; bytes[2] = 'V'; bytes[3] = 'D';
  bytes[4] = 1; bytes[5] = 0;
  put(bytes, 6, static_cast<uint16_t>(header.type), 2);
  put(bytes, 8, header.payloadLength, 4);
  std::copy(header.session.begin(), header.session.end(), bytes.begin() + 12);
  put(bytes, 28, header.epoch, 4);
  put(bytes, 32, header.sequence, 8);
  return bytes;
}

Header decodeHeader(std::span<const uint8_t> bytes)
{
  if (bytes.size() != headerSize) throw ProtocolError("Header must contain exactly 40 bytes");
  if (bytes[0] != 'D' || bytes[1] != 'F' || bytes[2] != 'V' || bytes[3] != 'D') throw ProtocolError("Invalid DFVD magic");
  if (bytes[4] != 1 || bytes[5] != 0) throw ProtocolError("Unsupported DFVD version");
  Header header;
  header.type = static_cast<MessageType>(get(bytes, 6, 2));
  header.payloadLength = static_cast<uint32_t>(get(bytes, 8, 4));
  std::copy_n(bytes.begin() + 12, 16, header.session.begin());
  header.epoch = static_cast<uint32_t>(get(bytes, 28, 4));
  header.sequence = get(bytes, 32, 8);
  validateHeader(header);
  return header;
}

std::vector<uint8_t> encodeMessage(Message message)
{
  if (message.payload.size() > limit(message.header.type)) throw ProtocolError("Payload exceeds type limit");
  message.header.payloadLength = static_cast<uint32_t>(message.payload.size());
  const auto bytes = encodeHeader(message.header);
  std::vector<uint8_t> result;
  result.reserve(bytes.size() + message.payload.size());
  result.insert(result.end(), bytes.begin(), bytes.end());
  result.insert(result.end(), message.payload.begin(), message.payload.end());
  return result;
}

FrameReader::FrameReader(Channel channel, std::optional<SessionId> session, std::optional<uint32_t> epoch)
    : m_channel(channel), m_session(session), m_epoch(epoch)
{
}

[[noreturn]] void FrameReader::reject(const char *reason)
{
  m_failed = true;
  m_header.reset();
  m_headerBytes = 0;
  m_payload.clear();
  throw ProtocolError(reason);
}

void FrameReader::validate(const Header& header)
{
  const bool binding = header.type == MessageType::ChannelBind && m_lastSequence == 0;
  const auto type = static_cast<uint16_t>(header.type);
  bool allowed = false;
  switch (m_channel) {
  case Channel::Control: allowed = type >= 1 && type <= 19; break;
  case Channel::Input: allowed = binding || header.type == MessageType::InputEvent || header.type == MessageType::ReleaseAll; break;
  case Channel::Video: allowed = binding || header.type == MessageType::VideoFrame; break;
  }
  if (!allowed) reject("Message not allowed on this channel");
  if (header.sequence <= m_lastSequence) reject("Repeated or decreasing sequence");
  if (m_session && header.session != *m_session) reject("Session mismatch");
  if (m_epoch && header.epoch != *m_epoch) reject("Epoch mismatch");
}

std::vector<Message> FrameReader::feed(std::span<const uint8_t> bytes)
{
  if (m_failed) throw ProtocolError("Reader is poisoned");
  // Bound work and output batching; callers read TLS in <=64 KiB chunks.
  if (bytes.size() > controlPayloadLimit) reject("Feed chunk exceeds 64 KiB");
  std::vector<Message> messages;
  while (!bytes.empty()) {
    if (!m_header) {
      const size_t size = std::min(headerSize - m_headerBytes, bytes.size());
      std::copy_n(bytes.begin(), size, m_headerBuffer.begin() + m_headerBytes);
      m_headerBytes += size;
      bytes = bytes.subspan(size);
      if (m_headerBytes != headerSize) continue;
      try {
        m_header = decodeHeader(m_headerBuffer);
      } catch (const ProtocolError&) {
        m_failed = true;
        throw;
      }
      validate(*m_header);
      m_payload.reserve(m_header->payloadLength);
    }
    const size_t size = std::min(static_cast<size_t>(m_header->payloadLength) - m_payload.size(), bytes.size());
    m_payload.insert(m_payload.end(), bytes.begin(), bytes.begin() + size);
    bytes = bytes.subspan(size);
    if (m_payload.size() == m_header->payloadLength) {
      m_lastSequence = m_header->sequence;
      messages.push_back({*m_header, std::move(m_payload)});
      m_payload = {};
      m_header.reset();
      m_headerBytes = 0;
    }
  }
  return messages;
}

void FrameReader::finish()
{
  if (m_failed) throw ProtocolError("Reader is poisoned");
  if (m_headerBytes || m_header) reject("Truncated frame at EOF");
}

} // namespace deskflow::display
