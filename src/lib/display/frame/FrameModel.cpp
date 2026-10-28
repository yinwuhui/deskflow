// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "FrameModel.h"
#include <algorithm>
#include <limits>
namespace deskflow::display::infra {
namespace {
bool multiply(uint64_t a, uint64_t b, uint64_t &out)
{
  if (b && a > UINT64_MAX / b)
    return false;
  out = a * b;
  return true;
}
bool rect(const FrameRect &r, uint32_t w, uint32_t h)
{
  return r.width && r.height && r.x < w && r.y < h && r.width <= w - r.x && r.height <= h - r.y;
}
bool domain(MemoryDomain m)
{
  return int(m) >= int(MemoryDomain::Cpu) && int(m) <= int(MemoryDomain::BackendNative);
}
} // namespace
bool validateFrame(const FrameDescriptor &f, uint64_t bytes, const FrameLimits &limits)
{
  if (!validId(f.session, 16) || !validId(f.sourceResource, 16) || !f.topologyGeneration || !f.sequence || !f.epoch ||
      !f.captureTimeValid || !f.captureNanoseconds || !f.width || !f.height || f.width > limits.maxWidth ||
      f.height > limits.maxHeight || !f.logicalWidth || !f.logicalHeight || f.logicalWidth > 8192 ||
      f.logicalHeight > 8192 || !bytes || bytes > limits.maxBytes || !rect(f.visible, f.width, f.height) ||
      f.damage.size() > 16 || !domain(f.memory) ||
      (f.rotation != 0 && f.rotation != 90 && f.rotation != 180 && f.rotation != 270) ||
      (f.color != ColorProfile::SdrBt709Limited8 && f.color != ColorProfile::SdrBt709Full8) ||
      (f.cursor != CursorMode::Embedded && f.cursor != CursorMode::Metadata))
    return false;
  for (const auto &r : f.damage)
    if (!rect(r, f.width, f.height))
      return false;
  std::array<uint64_t, 3> rows{}, minimum{};
  size_t count = 0;
  switch (f.pixelFormat) {
  case PixelFormat::Bgra8:
    count = 1;
    rows[0] = f.height;
    minimum[0] = uint64_t(f.width) * 4;
    break;
  case PixelFormat::Nv12:
    if ((f.width | f.height) & 1)
      return false;
    count = 2;
    rows = {f.height, f.height / 2, 0};
    minimum = {f.width, f.width, 0};
    break;
  case PixelFormat::I420:
    if ((f.width | f.height) & 1)
      return false;
    count = 3;
    rows = {f.height, f.height / 2, f.height / 2};
    minimum = {f.width, f.width / 2, f.width / 2};
    break;
  default:
    return false;
  }
  if (f.planes.size() != count)
    return false;
  for (size_t i = 0; i < count; ++i) {
    const auto &p = f.planes[i];
    uint64_t needed = 0;
    if (p.stride < minimum[i] || !multiply(p.stride, rows[i], needed) || p.length < needed || p.offset > bytes ||
        p.length > bytes - p.offset)
      return false;
    for (size_t j = 0; j < i; ++j) {
      const auto &q = f.planes[j];
      if (p.offset < q.offset + q.length && q.offset < p.offset + p.length)
        return false;
    }
  }
  return true;
}
FrameBudget::FrameBudget(uint32_t surfaces, uint64_t bytes)
    : m_maxSurfaces(surfaces <= 6 ? surfaces : 0),
      m_maxBytes(bytes <= 64 * 1024 * 1024 ? bytes : 0)
{
  m_quarantine.reserve(6);
}
FrameBudget::Usage FrameBudget::usage() const
{
  std::lock_guard lock(m_mutex);
  auto value = m_used;
  value.quarantined = uint32_t(m_quarantine.size());
  return value;
}
bool FrameBudget::acquire(uint64_t n)
{
  std::lock_guard lock(m_mutex);
  if (!n || m_used.surfaces >= m_maxSurfaces || n > m_maxBytes - m_used.bytes)
    return false;
  ++m_used.surfaces;
  m_used.bytes += n;
  return true;
}
void FrameBudget::release(uint64_t n)
{
  std::lock_guard lock(m_mutex);
  if (m_used.surfaces && n <= m_used.bytes) {
    --m_used.surfaces;
    m_used.bytes -= n;
  }
}
void FrameBudget::quarantine(FrameAllocation a)
{
  std::lock_guard lock(m_mutex);
  m_quarantine.emplace_back(a.bytes, std::move(a.owner));
  m_selfPin = shared_from_this();
}
bool FrameBudget::releaseQuarantineAfterContextTermination()
{
  std::vector<std::pair<uint64_t, std::shared_ptr<void>>> discarded;
  std::shared_ptr<FrameBudget> hold;
  {
    std::lock_guard lock(m_mutex);
    // A retained application reference is still a live allocation even when GPU
    // contexts have gone. Do not reclaim the accounting ahead of that reference.
    for (const auto &entry : m_quarantine)
      if (entry.second && entry.second.use_count() != 1)
        return false;
    discarded = std::move(m_quarantine);
    hold = std::move(m_selfPin);
  }
  // Resource deleters may marshal to a platform thread and must not run under
  // this lock. The provider must count any deferred destruction in its own cap.
  for (auto &entry : discarded) {
    entry.second.reset();
    release(entry.first);
  }
  discarded.clear();
  return true;
}
FramePool::FramePool(
    std::string instance, uint32_t epoch, std::string session, std::shared_ptr<FrameBudget> budget, FrameLimits limits
)
    : m_instance(std::move(instance)),
      m_session(std::move(session)),
      m_epoch(epoch),
      m_budget(std::move(budget)),
      m_limits(limits)
{
  m_valid = validId(m_instance, 16) && validId(m_session, 16) && m_epoch && m_budget && limits.maxWidth &&
            limits.maxHeight && limits.maxWidth <= 8192 && limits.maxHeight <= 8192 && limits.maxBytes &&
            limits.maxBytes <= 64 * 1024 * 1024;
}
FramePool::~FramePool()
{
  ReclaimBatch reclaim{m_budget, {}};
  std::lock_guard lock(m_mutex);
  for (uint32_t i = 0; i < m_slots.size(); ++i) {
    auto &s = m_slots[i];
    if (s.state == SlotState::Free)
      continue;
    if (s.state == SlotState::Writing || s.state == SlotState::Reading || s.state == SlotState::Released ||
        s.allocation.owner.use_count() != 1) {
      s.allocation.bytes = s.bytes;
      m_budget->quarantine(std::move(s.allocation));
      s.bytes = 0;
      s.state = SlotState::Free;
    } else
      free(i, reclaim);
  }
}
FrameTicket FramePool::ticket(uint32_t i) const
{
  return {m_instance, i, m_epoch, m_slots[i].generation};
}
bool FramePool::matches(const FrameTicket &t, SlotState state) const
{
  return t.pool == m_instance && t.epoch == m_epoch && t.slot < m_slots.size() && t.generation &&
         t.generation == m_slots[t.slot].generation && m_slots[t.slot].state == state;
}
FramePool::ReclaimBatch::~ReclaimBatch()
{
  for (auto &a : allocations) {
    a.owner.reset();
    budget->release(a.bytes);
  }
}
void FramePool::free(uint32_t i, ReclaimBatch &reclaim)
{
  auto &s = m_slots[i];
  s.allocation.bytes = s.bytes;
  reclaim.allocations.push_back(std::move(s.allocation));
  s.allocation = {};
  s.descriptor = {};
  s.bytes = 0;
  s.fence = 0;
  s.state = SlotState::Free;
}
std::optional<FrameTicket> FramePool::reserve(uint64_t bytes, MemoryDomain memory)
{
  std::lock_guard lock(m_mutex);
  if (!m_valid || m_retired || !domain(memory) || !bytes || bytes > m_limits.maxBytes)
    return {};
  for (uint32_t i = 0; i < m_slots.size(); ++i) {
    auto &s = m_slots[i];
    if (s.state != SlotState::Free || s.generation == UINT64_MAX)
      continue;
    if (!m_budget->acquire(bytes))
      return {};
    ++s.generation;
    s.bytes = bytes;
    s.memory = memory;
    s.state = SlotState::Writing;
    return ticket(i);
  }
  return {};
}
bool FramePool::adopt(const FrameTicket &t, FrameAllocation allocation)
{
  std::lock_guard lock(m_mutex);
  if (!matches(t, SlotState::Writing) || m_retired)
    return false;
  auto &s = m_slots[t.slot];
  if (s.allocation.owner || !allocation.owner || allocation.owner.use_count() != 1 || allocation.bytes != s.bytes ||
      allocation.memory != s.memory)
    return false;
  s.allocation = std::move(allocation);
  return true;
}
bool FramePool::abortWrite(const FrameTicket &t)
{
  ReclaimBatch reclaim{m_budget, {}};
  std::lock_guard lock(m_mutex);
  if (!matches(t, SlotState::Writing))
    return false;
  auto &s = m_slots[t.slot];
  if (s.allocation.owner && s.allocation.owner.use_count() != 1)
    return false;
  free(t.slot, reclaim);
  return true;
}
bool FramePool::publish(const FrameTicket &t, FrameDescriptor descriptor, bool producerFenceComplete)
{
  std::lock_guard lock(m_mutex);
  if (!matches(t, SlotState::Writing) || m_retired)
    return false;
  auto &s = m_slots[t.slot];
  if (!s.allocation.owner || s.allocation.owner.use_count() != 1 || descriptor.session != m_session ||
      descriptor.epoch != m_epoch || descriptor.memory != s.memory || descriptor.sequence <= m_lastSequence ||
      descriptor.captureNanoseconds <= m_lastTimestamp || (s.memory != MemoryDomain::Cpu && !producerFenceComplete) ||
      !validateFrame(descriptor, s.bytes, m_limits))
    return false;
  m_lastSequence = descriptor.sequence;
  m_lastTimestamp = descriptor.captureNanoseconds;
  s.descriptor = std::move(descriptor);
  s.state = SlotState::Published;
  return true;
}
std::optional<ReadFrame> FramePool::readNewest()
{
  return readNewest(false);
}
std::optional<ReadFrame> FramePool::readNewestCpu()
{
  return readNewest(true);
}
std::optional<ReadFrame> FramePool::readNewest(bool requireCpuView)
{
  ReclaimBatch reclaim{m_budget, {}};
  std::lock_guard lock(m_mutex);
  if (m_retired)
    return {};
  std::optional<uint32_t> newest;
  for (uint32_t i = 0; i < m_slots.size(); ++i)
    if (m_slots[i].state == SlotState::Published &&
        (!newest || m_slots[i].descriptor.sequence > m_slots[*newest].descriptor.sequence))
      newest = i;
  if (!newest ||
      (requireCpuView && (m_slots[*newest].memory != MemoryDomain::Cpu || !m_slots[*newest].allocation.cpuData)))
    return {};
  for (uint32_t i = 0; i < m_slots.size(); ++i)
    if (i != *newest && m_slots[i].state == SlotState::Published) {
      free(i, reclaim);
      ++m_drops;
    }
  auto &s = m_slots[*newest];
  s.state = SlotState::Reading;
  return ReadFrame{ticket(*newest), s.descriptor, s.allocation.owner, s.allocation.cpuData, s.bytes};
}
bool FramePool::releaseRead(const FrameTicket &t, uint64_t fence)
{
  ReclaimBatch reclaim{m_budget, {}};
  std::lock_guard lock(m_mutex);
  if (!matches(t, SlotState::Reading))
    return false;
  auto &s = m_slots[t.slot];
  if (s.allocation.owner.use_count() != 1 || (s.memory != MemoryDomain::Cpu && !fence))
    return false;
  s.state = SlotState::Released;
  s.fence = fence;
  if (s.memory == MemoryDomain::Cpu && !fence)
    free(t.slot, reclaim);
  return true;
}
bool FramePool::fenceCompleted(const FrameTicket &t, uint64_t fence)
{
  ReclaimBatch reclaim{m_budget, {}};
  std::lock_guard lock(m_mutex);
  if (!matches(t, SlotState::Released) || !fence || fence != m_slots[t.slot].fence ||
      m_slots[t.slot].allocation.owner.use_count() != 1)
    return false;
  free(t.slot, reclaim);
  return true;
}
void FramePool::retire()
{
  ReclaimBatch reclaim{m_budget, {}};
  std::lock_guard lock(m_mutex);
  m_retired = true;
  for (uint32_t i = 0; i < m_slots.size(); ++i) {
    auto &s = m_slots[i];
    if (s.state == SlotState::Published) {
      free(i, reclaim);
      ++m_drops;
    }
    // A producer may still be writing. Cancellation does not grant permission
    // to release its memory; it must acknowledge by abortWrite().
  }
}
bool FramePool::drained() const
{
  std::lock_guard lock(m_mutex);
  return std::all_of(m_slots.begin(), m_slots.end(), [](const auto &s) { return s.state == SlotState::Free; });
}
uint64_t FramePool::droppedRawFrames() const
{
  std::lock_guard lock(m_mutex);
  return m_drops;
}
std::shared_ptr<CpuFrameLease> CpuFrameLease::acquire(std::shared_ptr<FramePool> pool)
{
  if (!pool)
    return {};
  // Allocate callback ownership before borrowing a slot, so allocation failure
  // cannot strand a Reading ticket.
  auto lease = std::shared_ptr<CpuFrameLease>(new CpuFrameLease(std::move(pool)));
  lease->m_frame = lease->m_pool->readNewestCpu();
  return lease->m_frame ? lease : std::shared_ptr<CpuFrameLease>{};
}
CpuFrameLease::~CpuFrameLease()
{
  if (!m_frame)
    return;
  const auto ticket = m_frame->ticket;
  m_frame.reset();
  m_pool->releaseRead(ticket);
}
} // namespace deskflow::display::infra
