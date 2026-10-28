// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/frame/FrameModel.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace deskflow::display::infra;
static void require(bool b, const char *why)
{
  if (!b)
    throw std::runtime_error(why);
}
static const std::string session(32, '1'), instance(32, '2');
static FrameDescriptor descriptor(uint64_t seq = 1, MemoryDomain memory = MemoryDomain::Cpu)
{
  FrameDescriptor d;
  d.session = session;
  d.sourceResource = std::string(32, '3');
  d.topologyGeneration = 1;
  d.sequence = seq;
  d.captureNanoseconds = seq;
  d.epoch = 1;
  d.width = d.height = d.logicalWidth = d.logicalHeight = 2;
  d.planes = {{0, 8, 16}};
  d.visible = {0, 0, 2, 2};
  d.captureTimeValid = true;
  d.memory = memory;
  return d;
}
static FrameAllocation allocation(MemoryDomain m = MemoryDomain::Cpu)
{
  return {std::make_shared<std::vector<unsigned char>>(16), 16, m};
}
static FrameTicket put(FramePool &p, uint64_t seq, MemoryDomain m = MemoryDomain::Cpu)
{
  auto t = p.reserve(16, m);
  require(t.has_value(), "reserve");
  require(p.adopt(*t, allocation(m)), "adopt");
  require(p.publish(*t, descriptor(seq, m), m != MemoryDomain::Cpu), "publish");
  return *t;
}
static void layouts()
{
  auto d = descriptor();
  require(validateFrame(d, 16), "BGRA valid rejected");
  d.pixelFormat = PixelFormat::Nv12;
  d.planes = {{0, 2, 4}, {4, 2, 2}};
  require(validateFrame(d, 6), "NV12 valid rejected");
  d.pixelFormat = PixelFormat::I420;
  d.planes = {{0, 2, 4}, {4, 1, 1}, {5, 1, 1}};
  require(validateFrame(d, 6), "I420 valid rejected");
  for (int i = 0; i < 9; ++i) {
    d = descriptor();
    switch (i) {
    case 0:
      d.planes[0].stride = UINT64_MAX;
      break;
    case 1:
      d.planes[0].offset = UINT64_MAX;
      break;
    case 2:
      d.planes[0].length = UINT64_MAX;
      break;
    case 3:
      d.width = UINT32_MAX;
      break;
    case 4:
      d.visible = {UINT32_MAX, 0, 2, 2};
      break;
    case 5:
      d.captureTimeValid = false;
      break;
    case 6:
      d.epoch = 0;
      break;
    case 7:
      d.damage.assign(17, {0, 0, 1, 1});
      break;
    case 8:
      d.pixelFormat = PixelFormat::Nv12;
      d.planes = {{0, 2, 4}, {2, 2, 2}};
      break;
    }
    require(!validateFrame(d, 16), "unsafe layout accepted");
  }
}
static void budget()
{
  auto b = std::make_shared<FrameBudget>(2, 32);
  FramePool p(instance, 1, session, b), other(std::string(32, '4'), 1, session, b);
  auto a = p.reserve(16, MemoryDomain::Cpu), c = other.reserve(16, MemoryDomain::Cpu);
  require(a && c, "shared reserve");
  require(!p.reserve(1, MemoryDomain::Cpu), "surface budget escaped across pools");
  require(b->usage().bytes == 32 && b->usage().surfaces == 2, "incorrect actual budget");
  require(p.abortWrite(*a) && other.abortWrite(*c), "abort reservation");
  require(b->usage().bytes == 0, "reservation leaked");
  auto tooBig = p.reserve(UINT64_MAX, MemoryDomain::Cpu);
  require(!tooBig, "overflow allocation accepted");
  auto invalid = std::make_shared<FrameBudget>(7, UINT64_MAX);
  FramePool bad(instance, 1, session, invalid);
  require(!bad.reserve(1, MemoryDomain::Cpu), "invalid cap expanded");
}
static void freshness()
{
  auto b = std::make_shared<FrameBudget>();
  FramePool p(instance, 1, session, b);
  put(p, 1);
  put(p, 2);
  const auto ticket = put(p, 3);
  auto r = p.readNewest();
  require(r && r->descriptor.sequence == 3 && r->ticket == ticket, "oldest frame delivered");
  require(p.droppedRawFrames() == 2 && b->usage().surfaces == 1, "old raw frames retained");
  require(!p.releaseRead(ticket), "borrowed owner released early");
  r->owner.reset();
  require(p.releaseRead(ticket), "CPU release");
  require(p.drained() && b->usage().surfaces == 0, "CPU allocation not reclaimed");
}
static void staleTickets()
{
  auto b = std::make_shared<FrameBudget>();
  FramePool p(instance, 1, session, b);
  auto old = p.reserve(16, MemoryDomain::Cpu);
  require(old && p.abortWrite(*old), "abort old ticket");
  auto fresh = p.reserve(16, MemoryDomain::Cpu);
  require(fresh && fresh->generation > old->generation, "slot reused generation");
  require(!p.adopt(*old, allocation()) && !p.abortWrite(*old), "stale writer changed new slot");
  require(p.adopt(*fresh, allocation()), "new adopt");
  auto d = descriptor();
  d.epoch = 2;
  require(!p.publish(*fresh, d), "wrong epoch published");
  require(p.publish(*fresh, descriptor()), "current publish");
  auto r = p.readNewest();
  r->owner.reset();
  require(p.releaseRead(*fresh), "release");
  require(!p.releaseRead(*fresh), "double release");
  auto t = p.reserve(16, MemoryDomain::Cpu);
  require(t && p.adopt(*t, allocation()), "next allocation");
  require(!p.publish(*t, descriptor(1)), "frame counter replay");
  require(p.abortWrite(*t), "cleanup replay");
}
static void fences()
{
  auto b = std::make_shared<FrameBudget>(1, 16);
  FramePool p(instance, 1, session, b);
  auto t = p.reserve(16, MemoryDomain::IOSurface);
  require(t && p.adopt(*t, allocation(MemoryDomain::IOSurface)), "native adopt");
  require(!p.publish(*t, descriptor(1, MemoryDomain::IOSurface)), "producer fence ignored");
  require(p.publish(*t, descriptor(1, MemoryDomain::IOSurface), true), "producer completion");
  auto read = p.readNewest();
  read->owner.reset();
  require(!p.releaseRead(*t), "consumer fence missing");
  require(p.releaseRead(*t, 37), "reader release");
  require(!p.reserve(16, MemoryDomain::Cpu), "GPU pending surface reused");
  require(!p.fenceCompleted(*t, 38), "wrong fence completed");
  require(p.fenceCompleted(*t, 37), "actual fence rejected");
  require(p.drained() && b->usage().bytes == 0, "GPU allocation retained");
}
static void retirement()
{
  auto b = std::make_shared<FrameBudget>(2, 32);
  FramePool p(instance, 1, session, b);
  auto reading = put(p, 1, MemoryDomain::IOSurface);
  auto r = p.readNewest();
  auto writing = p.reserve(16, MemoryDomain::Cpu);
  require(writing.has_value(), "writer reserve");
  p.retire();
  require(
      !p.reserve(1, MemoryDomain::Cpu) && !p.readNewest() && !p.drained(), "retired pool accepted work or force drained"
  );
  require(p.abortWrite(*writing), "cancelled writer cannot acknowledge");
  r->owner.reset();
  require(p.releaseRead(reading, 1) && p.fenceCompleted(reading, 1), "retired reader cannot finish");
  require(p.drained(), "retirement stuck");
}
static void quarantine()
{
  auto b = std::make_shared<FrameBudget>(1, 16);
  std::optional<ReadFrame> reading;
  {
    FramePool p(instance, 1, session, b);
    put(p, 1, MemoryDomain::IOSurface);
    reading = p.readNewest();
  }
  require(b->usage().bytes == 16 && b->usage().quarantined == 1, "orphan GPU allocation not pinned");
  FramePool next(std::string(32, '4'), 2, session, b);
  require(!next.reserve(16, MemoryDomain::Cpu), "new epoch bypassed pinned allocation");
  require(!b->releaseQuarantineAfterContextTermination(), "live reader owner ignored");
  reading->owner.reset();
  require(
      b->releaseQuarantineAfterContextTermination() && b->usage().bytes == 0,
      "verified context teardown did not clear quarantine"
  );
  {
    FramePool writing(instance, 1, session, b);
    require(writing.reserve(16, MemoryDomain::Cpu).has_value(), "orphan writer");
  }
  require(b->usage().quarantined == 1, "in-flight allocation reservation silently freed");
  require(b->releaseQuarantineAfterContextTermination(), "orphan reservation not released");
}
static void deleterOutsideLock()
{
  auto b = std::make_shared<FrameBudget>(1, 16);
  FramePool p(instance, 1, session, b);
  bool deleted = false;
  FrameAllocation a{
      std::shared_ptr<void>(
          new int(1),
          [&](void *ptr) {
            p.drained();
            deleted = true;
            delete static_cast<int *>(ptr);
          }
      ),
      16, MemoryDomain::Cpu
  };
  auto t = p.reserve(16, MemoryDomain::Cpu);
  require(t && p.adopt(*t, std::move(a)), "reentrant owner adopt");
  require(p.abortWrite(*t) && deleted, "deleter ran while lock held");
}
static void cpuCallbackLifetime()
{
  auto budget = std::make_shared<FrameBudget>();
  auto pool = std::make_shared<FramePool>(instance, 1, session, budget);
  auto pixels = std::make_shared<std::vector<unsigned char>>(16, 0x5a);
  const void *view = pixels->data();
  require(view != pixels.get(), "fixture must distinguish owner from pixel view");
  auto ticket = pool->reserve(16, MemoryDomain::Cpu);
  require(ticket.has_value(), "reserve CPU callback fixture");
  FrameAllocation allocation{pixels, 16, MemoryDomain::Cpu, view};
  pixels.reset();
  require(pool->adopt(*ticket, std::move(allocation)) && pool->publish(*ticket, descriptor()), "publish CPU view");
  auto lease = CpuFrameLease::acquire(pool);
  require(
      lease && lease->data() == view && lease->allocationBytes() == 16 && lease->descriptor().sequence == 1,
      "CPU lease lost actual view or metadata"
  );
  auto callbackOwner = lease;
  pool->retire();
  require(!pool->drained() && !CpuFrameLease::acquire(pool), "retired borrowed CPU frame forced free");
  pool.reset();
  lease.reset();
  require(budget->usage().bytes == 16, "callback input freed before last consumer");
  std::atomic<bool> valid{false};
  std::thread callback([owner = std::move(callbackOwner), &valid] {
    valid.store(static_cast<const unsigned char *>(owner->data())[15] == 0x5a);
  });
  callback.join();
  require(
      valid.load() && budget->usage().bytes == 0 && budget->usage().surfaces == 0,
      "foreign-thread callback did not preserve/release CPU input"
  );
}
static void cpuViewGates()
{
  require(!CpuFrameLease::acquire({}), "null CPU pool accepted");
  auto budget = std::make_shared<FrameBudget>();
  auto opaque = std::make_shared<FramePool>(instance, 1, session, budget);
  put(*opaque, 1);
  require(!CpuFrameLease::acquire(opaque), "opaque owner interpreted as CPU pixels");
  opaque->retire();
  require(opaque->drained(), "CPU view refusal consumed opaque ticket");
  auto gpu = std::make_shared<FramePool>(instance, 1, session, budget);
  put(*gpu, 1, MemoryDomain::IOSurface);
  require(!CpuFrameLease::acquire(gpu), "GPU owner interpreted as CPU pixels");
  gpu->retire();
  require(gpu->drained() && budget->usage().bytes == 0, "GPU refusal borrowed or forged a fence");
}
int main(int argc, char **argv)
{
  try {
    require(argc == 2, "case required");
    const std::string c = argv[1];
    if (c == "FrameLayouts")
      layouts();
    else if (c == "FrameBudget")
      budget();
    else if (c == "FrameFreshness")
      freshness();
    else if (c == "FrameTickets")
      staleTickets();
    else if (c == "FrameFences")
      fences();
    else if (c == "FrameRetirement")
      retirement();
    else if (c == "FrameQuarantine")
      quarantine();
    else if (c == "FrameCpuCallbackLifetime")
      cpuCallbackLifetime();
    else if (c == "FrameCpuViewGates")
      cpuViewGates();
    else if (c == "FrameDeleter")
      deleterOutsideLock();
    else
      throw std::runtime_error("unknown case");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
