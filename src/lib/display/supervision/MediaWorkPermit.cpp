// SPDX-License-Identifier: MIT
#include "MediaWorkPermit.h"
#include <QHostAddress>
namespace deskflow::display::infra {
std::shared_ptr<MediaWorkPermit> MediaWorkPermit::issue(const MediaWorkContext &c, TimePoint expires, TimePoint now)
{
  QHostAddress local;
  const auto &b = c.binding;
  const auto &p = c.preparation;
  if (!validWorkerBinding(b) || b.purpose != WorkerPurpose::Media || !validWorkerPrepare(p, b.direction, now) ||
      expires <= now || (p.protocol == WireProtocol::WebRtc ? b.backend != "webrtc" : b.backend != "rdp") ||
      !validId(c.streamSourceResource.toStdString(), 16) || !c.streamTopologyGeneration ||
      !local.setAddress(c.localAddress) || local.isNull() || local.isMulticast() || local == QHostAddress::AnyIPv4 ||
      local == QHostAddress::AnyIPv6 || local == QHostAddress::Broadcast ||
      (local.isLoopback() && !p.allowLoopbackDiagnostics) ||
      (b.direction == Direction::Send &&
       (c.streamSourceResource != p.sourceResource || c.streamTopologyGeneration != p.sourceTopologyGeneration)))
    return {};
  return std::shared_ptr<MediaWorkPermit>(new MediaWorkPermit(c, expires, now));
}
bool MediaWorkPermit::active(TimePoint now) const
{
  return !m_revoked.load(std::memory_order_acquire) && now >= m_issued && now < m_expires;
}
bool MediaWorkPermit::matches(const MediaWorkContext &context, TimePoint now) const
{
  return active(now) && context == m_context;
}
bool MediaWorkPermit::allowsCapture(
    const std::string &owner, const std::string &session, uint32_t epoch, const QString &plan, const QString &user,
    const QString &resource, const NativeSourceReference &source, const QString &identity, const QString &digest,
    uint64_t generation, TimePoint now
) const
{
  const auto &b = m_context.binding;
  const auto &p = m_context.preparation;
  return active(now) && b.direction == Direction::Send && b.token.owner == owner && b.token.session == session &&
         b.token.epoch == epoch && b.planHash == plan && b.localUserSession == user && p.sourceResource == resource &&
         p.source == source && p.sourceIdentity == identity && p.sourceContextDigest == digest &&
         p.sourceTopologyGeneration == generation && p.video.cursor == CursorMode::Embedded && p.rawFrameSlots >= 3 &&
         p.rawFrameBytes >= uint64_t(3) * 1920 * 1080 * 4;
}
} // namespace deskflow::display::infra
