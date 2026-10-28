// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "MediaWorkerContracts.h"
namespace deskflow::display::infra {
// Local resource helper messages, never media/network signaling. Agent must
// persist this exact creation intent before configuring the helper.
struct WorkerPrepareOutput
{
  QString resourceId, creationIntent, preCreateContextDigest;
  uint64_t startupDeadlineNanoseconds = 0;
  uint32_t width = 1920, height = 1080, refreshRate = 30;
  bool operator==(const WorkerPrepareOutput &) const = default;
};
struct WorkerOutputObserved
{
  QString resourceId, nativeIdentity;
  NativeSourceReference source;
  uint64_t topologyGeneration = 0;
  bool operator==(const WorkerOutputObserved &) const = default;
};
struct WorkerConfigureOutput
{
  QString resourceId;
  bool operator==(const WorkerConfigureOutput &) const = default;
};
struct WorkerOutputReady
{
  QString resourceId;
  bool operator==(const WorkerOutputReady &) const = default;
};
bool validOutputPreparation(const WorkerPrepareOutput &, TimePoint now);
bool validOutputObservation(const WorkerOutputObserved &);
} // namespace deskflow::display::infra
