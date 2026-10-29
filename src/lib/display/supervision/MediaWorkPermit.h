// SPDX-License-Identifier: MIT
#pragma once
#include "WorkerProtocol.h"
#include <atomic>
#include <memory>
#include <utility>
namespace deskflow::display::infra {
struct MediaWorkContext
{
  WorkerBinding binding;
  WorkerPrepare preparation;
  QString localAddress, streamSourceResource;
  uint64_t streamTopologyGeneration = 0;
  bool operator==(const MediaWorkContext &) const = default;
};
// Trusted local owner issues this AFTER its authorization/barrier checks. This
// class verifies structure/scope and enforces irreversible local revocation; it
// does not authenticate a device or grant permission from peer JSON. It is never
// serialized. Frames already borrowed retain their ownership until drained.
class MediaWorkPermit
{
public:
  static std::shared_ptr<MediaWorkPermit>
  issue(const MediaWorkContext &, TimePoint expires, TimePoint now = Clock::now());
  bool active(TimePoint now = Clock::now()) const;
  bool matches(const MediaWorkContext &, TimePoint now = Clock::now()) const;
  bool allowsCapture(
      const std::string &owner, const std::string &session, uint32_t epoch, const QString &planHash,
      const QString &userSession, const QString &resource, const NativeSourceReference &, const QString &identity,
      const QString &contextDigest, uint64_t topologyGeneration, TimePoint now = Clock::now()
  ) const;
  void revoke()
  {
    m_revoked.store(true, std::memory_order_release);
  }
  MediaWorkPermit(const MediaWorkPermit &) = delete;
  MediaWorkPermit &operator=(const MediaWorkPermit &) = delete;

private:
  MediaWorkPermit(MediaWorkContext context, TimePoint expires, TimePoint issued)
      : m_context(std::move(context)),
        m_expires(expires),
        m_issued(issued)
  {
  }
  const MediaWorkContext m_context;
  const TimePoint m_expires, m_issued;
  std::atomic<bool> m_revoked{false};
};
} // namespace deskflow::display::infra
