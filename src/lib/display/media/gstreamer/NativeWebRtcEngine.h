// SPDX-License-Identifier: MIT
#pragma once
#include "display/frame/FrameModel.h"
#include "display/supervision/WorkerProtocol.h"
#include <memory>
namespace deskflow::display::infra {
struct NativeWebRtcConfiguration
{
  WorkerBinding binding;
  WorkerPrepare preparation;
  // From the committed media plan. On receive this is the remote stream identity,
  // never local capture authority. ICE binds only this verified local address.
  QString localAddress, streamSourceResource;
  uint64_t streamTopologyGeneration = 0;
};
using NativeFrameCorrelation = WebRtcFrameCorrelation;
struct NativeWebRtcStatistics
{
  uint64_t inputFrames = 0, decodedFrames = 0, gccUpdates = 0, encoderUpdates = 0, deniedPackets = 0;
  uint32_t estimatedBitrate = 0, encoderKbitrate = 0;
  bool constrainedBaseline = false, addressPairApproved = false;
  uint64_t compatibleProfileLabels = 0;
};
// Isolated-worker engine SPI. No device authorization or capability publication.
// Caller supplies authenticated plan/signaling, exact capture, and presentation.
// Public instance calls use one owner thread. Internal streaming callbacks are
// synchronized; exported CPU leases may be released on consumer threads.
class NativeWebRtcEngine
{
public:
  static bool initializeRuntime(const QString &privateRuntimeRoot, QString &error);
  NativeWebRtcEngine();
  ~NativeWebRtcEngine();
  bool prepare(const NativeWebRtcConfiguration &);
  bool start();
  bool applySignal(const WorkerMediaSignal &);
  bool submit(std::shared_ptr<CpuFrameLease>);
  std::shared_ptr<CpuFrameLease> takeDecoded();
  std::vector<WorkerMediaSignal> takeSignals();
  std::vector<NativeFrameCorrelation> takeCorrelations();
  bool applyCorrelation(const NativeFrameCorrelation &);
  void poll();
  bool refresh();
  bool stop();
  bool stopped() const;
  QString error() const;
  NativeWebRtcStatistics statistics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
} // namespace deskflow::display::infra
