// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "display/core/InfrastructureTypes.h"
#include <array>
#include <memory>
#include <mutex>

namespace deskflow::display::infra {
enum class PixelFormat
{
  Bgra8,
  Nv12,
  I420
};
struct FramePlane
{
  uint64_t offset = 0, stride = 0, length = 0;
};
struct FrameRect
{
  uint32_t x = 0, y = 0, width = 0, height = 0;
};
struct FrameDescriptor
{
  std::string session, sourceResource;
  uint64_t topologyGeneration = 0, sequence = 0, captureNanoseconds = 0;
  uint32_t epoch = 0, width = 0, height = 0, logicalWidth = 0, logicalHeight = 0, rotation = 0;
  PixelFormat pixelFormat = PixelFormat::Bgra8;
  ColorProfile color = ColorProfile::SdrBt709Limited8;
  CursorMode cursor = CursorMode::Embedded;
  MemoryDomain memory = MemoryDomain::Cpu;
  std::vector<FramePlane> planes;
  FrameRect visible;
  std::vector<FrameRect> damage;
  bool captureTimeValid = false, repeated = false, unchanged = false;
};
struct FrameLimits
{
  uint32_t maxWidth = 1920, maxHeight = 1080;
  uint64_t maxBytes = 64 * 1024 * 1024;
};
bool validateFrame(const FrameDescriptor &, uint64_t allocationBytes, const FrameLimits & = {});

// Shared by all pools of a worker, including retired epochs. Every allocation
// (CPU or GPU) counts, not just currently queued frames. No implicit expansion.
struct FrameAllocation;
class FrameBudget : public std::enable_shared_from_this<FrameBudget>
{
public:
  struct Usage
  {
    uint32_t surfaces = 0;
    uint64_t bytes = 0;
    uint32_t quarantined = 0;
  };
  explicit FrameBudget(uint32_t surfaces = 6, uint64_t bytes = 64 * 1024 * 1024);
  Usage usage() const;
  // Trusted platform executor only: all importing contexts must be terminated.
  // Orphaned GPU allocations otherwise remain pinned and count against budget.
  bool releaseQuarantineAfterContextTermination();

private:
  friend class FramePool;
  bool acquire(uint64_t);
  void release(uint64_t);
  void quarantine(FrameAllocation);
  std::vector<std::pair<uint64_t, std::shared_ptr<void>>> m_quarantine;
  std::shared_ptr<FrameBudget> m_selfPin;
  mutable std::mutex m_mutex;
  Usage m_used;
  uint32_t m_maxSurfaces;
  uint64_t m_maxBytes;
};
enum class SlotState
{
  Free,
  Writing,
  Published,
  Reading,
  Released
};
struct FrameTicket
{
  std::string pool;
  uint32_t slot = 0, epoch = 0;
  uint64_t generation = 0;
  bool operator==(const FrameTicket &) const = default;
};
struct FrameAllocation
{
  // Native owner must complete destruction on its platform thread before its
  // deleter returns, or retain a separate charge until deferred destruction.
  // Deleters run outside core locks. The core never
  // interprets a pointer or imports a GPU handle. bytes must be actual allocation.
  std::shared_ptr<void> owner;
  uint64_t bytes = 0;
  MemoryDomain memory = MemoryDomain::Cpu;
  // Optional local CPU view, never serialized. Trusted importer guarantees
  // allocationBytes of accessible storage and ties its lifetime to owner.
  const void *cpuData = nullptr;
};
struct ReadFrame
{
  FrameTicket ticket;
  FrameDescriptor descriptor;
  std::shared_ptr<const void> owner;
  const void *cpuData = nullptr;
  uint64_t allocationBytes = 0;
};
class FramePool
{
public:
  FramePool(std::string instance, uint32_t epoch, std::string session, std::shared_ptr<FrameBudget>, FrameLimits = {});
  ~FramePool();
  FramePool(const FramePool &) = delete;
  FramePool &operator=(const FramePool &) = delete;
  // Provider reserves before allocating: provider callbacks must not allocate
  // beyond the returned ticket. adopt() is called once after successful import.
  std::optional<FrameTicket> reserve(uint64_t allocationBytes, MemoryDomain);
  bool adopt(const FrameTicket &, FrameAllocation);
  bool abortWrite(const FrameTicket &);
  bool publish(const FrameTicket &, FrameDescriptor, bool producerFenceComplete = false);
  std::optional<ReadFrame> readNewest();
  std::optional<ReadFrame> readNewestCpu();
  // Every borrowed owner copy must be gone before releaseRead. A GPU reader
  // supplies a unique nonzero fence token even if it completed synchronously.
  bool releaseRead(const FrameTicket &, uint64_t fenceToken = 0);
  bool fenceCompleted(const FrameTicket &, uint64_t fenceToken);
  void retire(); // Never force-reclaims a Reading/Released GPU surface.
  bool drained() const;
  uint64_t droppedRawFrames() const;

private:
  std::optional<ReadFrame> readNewest(bool requireCpuView);
  struct Slot
  {
    SlotState state = SlotState::Free;
    uint64_t generation = 0, bytes = 0, fence = 0;
    MemoryDomain memory = MemoryDomain::Cpu;
    FrameAllocation allocation;
    FrameDescriptor descriptor;
  };
  bool matches(const FrameTicket &, SlotState) const;
  FrameTicket ticket(uint32_t) const;
  struct ReclaimBatch
  {
    std::shared_ptr<FrameBudget> budget;
    std::vector<FrameAllocation> allocations;
    ~ReclaimBatch();
  };
  void free(uint32_t, ReclaimBatch &);
  std::string m_instance, m_session;
  uint32_t m_epoch;
  std::shared_ptr<FrameBudget> m_budget;
  FrameLimits m_limits;
  std::array<Slot, 6> m_slots;
  mutable std::mutex m_mutex;
  uint64_t m_lastSequence = 0, m_lastTimestamp = 0, m_drops = 0;
  bool m_retired = false, m_valid = false;
};
// Shared callback lifetime for native encoders/appsrc. It never interprets an
// opaque native owner as pixels, and needs no platform facade on destruction.
// CPU view importers must supply an owner safe to destroy on a consumer thread.
class CpuFrameLease final
{
public:
  static std::shared_ptr<CpuFrameLease> acquire(std::shared_ptr<FramePool>);
  ~CpuFrameLease();
  CpuFrameLease(const CpuFrameLease &) = delete;
  CpuFrameLease &operator=(const CpuFrameLease &) = delete;
  const FrameDescriptor &descriptor() const
  {
    return m_frame->descriptor;
  }
  const void *data() const
  {
    return m_frame->cpuData;
  }
  uint64_t allocationBytes() const
  {
    return m_frame->allocationBytes;
  }

private:
  explicit CpuFrameLease(std::shared_ptr<FramePool> pool) : m_pool(std::move(pool))
  {
  }
  std::shared_ptr<FramePool> m_pool;
  std::optional<ReadFrame> m_frame;
};
} // namespace deskflow::display::infra
