// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "PlatformWorkerContracts.h"
namespace deskflow::display::infra {
bool validOutputPreparation(const WorkerPrepareOutput &p, TimePoint now)
{
  const auto current = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  return validId(p.resourceId.toStdString(), 16) && validId(p.creationIntent.toStdString(), 16) &&
         validId(p.preCreateContextDigest.toStdString(), 32) && p.width == 1920 && p.height == 1080 &&
         p.refreshRate == 30 && current >= 0 && p.startupDeadlineNanoseconds > uint64_t(current) &&
         p.startupDeadlineNanoseconds - uint64_t(current) <= 10000000000ULL;
}
bool validOutputObservation(const WorkerOutputObserved &o)
{
  if (!validId(o.resourceId.toStdString(), 16) || !validName(o.nativeIdentity.toStdString(), 128) ||
      !o.topologyGeneration || !o.source.primary)
    return false;
  switch (o.source.kind) {
  case NativeSourceKind::CgDisplay:
  case NativeSourceKind::PipeWireNode:
    return o.source.primary <= UINT32_MAX && o.source.secondary == 0;
  case NativeSourceKind::WindowsDisplay:
    return true;
  default:
    return false;
  }
}
} // namespace deskflow::display::infra
