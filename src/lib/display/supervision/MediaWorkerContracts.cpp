// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "MediaWorkerContracts.h"
#include <QHostAddress>
#include <QSet>
#include <QUuid>
#include <algorithm>
namespace deskflow::display::infra {
bool validFrameCorrelation(const WebRtcFrameCorrelation &m)
{
  return validId(m.sourceResource.toStdString(), 16) && validName(m.captureClockId.toStdString(), 56) &&
         m.topologyGeneration && m.frame && m.captureFrame && m.captureNanoseconds && m.epoch;
}
bool validMediaSignal(const WorkerMediaSignal &s)
{
  if (s.mediaLine != 0 || s.mid.size() > 64 || (!s.mid.isEmpty() && !validName(s.mid.toStdString(), 64)))
    return false;
  switch (s.kind) {
  case WebRtcSignalKind::Offer:
  case WebRtcSignalKind::Answer:
    return !s.description.isEmpty() && s.description.toUtf8().size() <= 32768 && !s.description.contains(QChar(0)) &&
           s.candidate.isEmpty() && s.mid.isEmpty();
  case WebRtcSignalKind::Ice:
    return s.description.isEmpty() && !s.candidate.isEmpty() && s.candidate.toUtf8().size() <= 1024 &&
           std::all_of(s.candidate.begin(), s.candidate.end(), [](QChar c) {
             return c.unicode() >= 32 && c.unicode() <= 126;
           });
  case WebRtcSignalKind::EndOfCandidates:
    return s.description.isEmpty() && s.candidate.isEmpty() && s.mid.isEmpty();
  }
  return false;
}
bool validWorkerPrepare(const WorkerPrepare &p, Direction local, TimePoint now)
{
  if ((p.protocol != WireProtocol::Rdp && p.protocol != WireProtocol::WebRtc) || !validFormat(p.video, p.protocol) ||
      p.rawFrameSlots < 1 || p.rawFrameSlots > 6 || p.rawFrameBytes < uint64_t(p.video.width) * p.video.height * 4 ||
      p.rawFrameBytes > 64 * 1024 * 1024 || p.encodedQueueFrames < 1 || p.encodedQueueFrames > 2 ||
      p.encodedQueueBytes < 65536 || p.encodedQueueBytes > 4 * 1024 * 1024 || p.allowedPeerAddresses.isEmpty() ||
      p.allowedPeerAddresses.size() > 16)
    return false;
  if ((p.protocol == WireProtocol::WebRtc && p.security != SecurityProfile::WebRtcAuthenticatedDtlsSrtp) ||
      (p.protocol == WireProtocol::Rdp && p.security != SecurityProfile::RdpAuthenticatedTls13))
    return false;
  const auto current = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  if (current < 0 || p.startupDeadlineNanoseconds <= uint64_t(current) ||
      p.startupDeadlineNanoseconds - uint64_t(current) > 10000000000ULL)
    return false;
  QSet<QHostAddress> addresses;
  for (const auto &text : p.allowedPeerAddresses) {
    QHostAddress address;
    if (text.size() > 128 || !address.setAddress(text) || address.isNull() || address.isMulticast() ||
        address == QHostAddress::AnyIPv4 || address == QHostAddress::AnyIPv6 || address == QHostAddress::Broadcast ||
        (!p.allowLoopbackDiagnostics && address.isLoopback()) || addresses.contains(address))
      return false;
    addresses.insert(address);
  }
  if (local == Direction::Receive)
    return p.sourceResource.isEmpty() && p.source == NativeSourceReference{} && p.sourceIdentity.isEmpty() &&
           p.sourceContextDigest.isEmpty() && p.sourceTopologyGeneration == 0;
  if (local != Direction::Send || !validId(p.sourceResource.toStdString(), 16) || !p.source.primary ||
      !p.sourceTopologyGeneration || !validId(p.sourceContextDigest.toStdString(), 32) ||
      !validName(p.sourceIdentity.toStdString(), 128))
    return false;
  switch (p.source.kind) {
  case NativeSourceKind::CgDisplay:
    return p.source.primary <= UINT32_MAX && p.source.secondary == 0 && !QUuid(p.sourceIdentity).isNull() &&
           QUuid(p.sourceIdentity).toString(QUuid::WithoutBraces).toLower() == p.sourceIdentity;
  case NativeSourceKind::PipeWireNode:
    return p.source.primary <= UINT32_MAX && p.source.secondary == 0;
  case NativeSourceKind::WindowsDisplay:
    return true;
  default:
    return false;
  }
}
} // namespace deskflow::display::infra
