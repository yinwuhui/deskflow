// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "display/core/InfrastructureTypes.h"
#include <QString>
#include <QStringList>
namespace deskflow::display::infra {
enum class NativeSourceKind
{
  None,
  CgDisplay,
  PipeWireNode,
  WindowsDisplay
};
struct NativeSourceReference
{
  NativeSourceKind kind = NativeSourceKind::None;
  uint64_t primary = 0;
  uint32_t secondary = 0;
  bool operator==(const NativeSourceReference &) const = default;
};
// Delivered by the trusted local agent after durable resource observation.
// No filesystem path, executable, shell text or peer-selectable plugin exists.
struct WorkerPrepare
{
  WireProtocol protocol = WireProtocol::WebRtc;
  VideoFormat video;
  SecurityProfile security = SecurityProfile::WebRtcAuthenticatedDtlsSrtp;
  QString sourceResource;
  NativeSourceReference source;
  QStringList allowedPeerAddresses;
  bool allowLoopbackDiagnostics = false;
  uint64_t startupDeadlineNanoseconds = 0; // Same-host steady clock, not a network timestamp.
  uint32_t rawFrameSlots = 6;
  uint64_t rawFrameBytes = 64 * 1024 * 1024;
  uint32_t encodedQueueFrames = 2;
  uint64_t encodedQueueBytes = 4 * 1024 * 1024;
  bool operator==(const WorkerPrepare &) const = default;
};
enum class WebRtcSignalKind
{
  Offer,
  Answer,
  Ice,
  EndOfCandidates
};
struct WorkerMediaSignal
{
  WebRtcSignalKind kind = WebRtcSignalKind::Offer;
  QString description, candidate, mid;
  uint32_t mediaLine = 0;
  bool operator==(const WorkerMediaSignal &) const = default;
};
// Structural budgets only. The native adapter must still parse standard SDP/ICE
// with its media engine and verify negotiated format, fingerprint and IP policy.
bool validMediaSignal(const WorkerMediaSignal &);
bool validWorkerPrepare(const WorkerPrepare &, Direction localDirection, TimePoint now);
} // namespace deskflow::display::infra
