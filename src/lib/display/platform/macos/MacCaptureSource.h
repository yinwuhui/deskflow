// SPDX-License-Identifier: MIT
#pragma once
#include "display/frame/FrameModel.h"
#include <QString>
#include <atomic>
#include <memory>
namespace deskflow::display::infra {
class MacResourceProvider;
struct MacCaptureStatistics
{
  uint64_t samples = 0, complete = 0, published = 0, budgetDrops = 0;
  int lastStatus = -1, stopError = 0;
  bool nativeStopped = false;
};
// Minted only from this provider's live, Active virtual-output record. It is
// local evidence, not a serializable network authorization or recoverable lease.
class MacCaptureBinding
{
public:
  const std::string &resource() const
  {
    return m_resource;
  }

private:
  friend class MacResourceProvider;
  friend class MacCaptureSource;
  MacCaptureBinding() = default;
  std::string m_resource, m_session;
  QString m_identity, m_topologyDigest;
  std::shared_ptr<std::atomic<bool>> m_live, m_claimed;
  uint32_t m_display = 0, m_epoch = 0;
  uint64_t m_generation = 0;
};
// Main-thread facade with an active QGuiApplication/native event loop.
// start() accepts asynchronous preparation, not proof of frames. Samples run on
// a private serial queue. No UI/permission
// prompts, source fallback, network or encoding. CPU ReadFrame owner is BGRA data.
class MacCaptureSource
{
public:
  MacCaptureSource();
  ~MacCaptureSource();
  bool start(const MacCaptureBinding &, TimePoint deadline);
  void poll();
  void stop();
  bool stopped() const;
  QString error() const;
  std::optional<ReadFrame> readNewest();
  std::shared_ptr<CpuFrameLease> takeCpuLease();
  bool releaseRead(const FrameTicket &);
  FrameBudget::Usage usage() const;
  MacCaptureStatistics statistics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
} // namespace deskflow::display::infra
