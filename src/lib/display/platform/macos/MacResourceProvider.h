// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "MacCaptureSource.h"
#include "MacVirtualOutput.h"
#include "display/resource/ResourceExecutor.h"
#include "display/supervision/WorkerProcess.h"
namespace deskflow::display::infra {
// One immutable owner/attempt per instance, on the resource executor thread.
// A reconstructed provider may prove absence, but never adopts/terminates a PID.
class MacResourceProvider final : public PlatformResourceProvider
{
public:
  MacResourceProvider(DurableResourceLedger &, WorkerRegistration, WorkerBinding);
  ~MacResourceProvider() override;
  ProviderRegistration registration();
  QString lastError() const { return !m_failure.isEmpty() ? m_failure : (m_process ? m_process->error() : QString{}); }
  std::optional<MacCaptureBinding>
  captureBinding(const QString &resource, TimePoint deadline, QString *error = nullptr);
  ResourceContextSnapshot snapshot(const ResourceRecord &, TimePoint) override;
  NativeCreation create(const ResourceRecord &, TimePoint) override;
  bool configure(const ResourceRecord &, TimePoint) override;
  bool quiesce(const ResourceRecord &, TimePoint) override;
  NativeInspection inspect(const ResourceRecord &, TimePoint) override;
  bool release(const ResourceRecord &, const ResourceEvidence &, TimePoint) override;

private:
  bool scope(const ResourceRecord &) const;
  bool wait(const std::function<bool()> &, TimePoint);
  bool stop(TimePoint);
  bool helperAbsent(const ResourceRecord &) const;
  const ResourceRecord *dependency(const ResourceRecord &) const;
  ResourceEvidence owned() const;
  DurableResourceLedger &m_ledger;
  WorkerRegistration m_registration;
  WorkerBinding m_binding;
  Qt::HANDLE m_thread;
  std::unique_ptr<WorkerProcess> m_process;
  QString m_workerId, m_outputId, m_failure;
  std::shared_ptr<std::atomic<bool>> m_captureLive = std::make_shared<std::atomic<bool>>(true);
  std::shared_ptr<std::atomic<bool>> m_captureClaimed = std::make_shared<std::atomic<bool>>(false);
  std::optional<WorkerOutputObserved> m_claim;
  bool m_established = false, m_ready = false, m_outputReady = false, m_exited = false, m_stopping = false;
};
} // namespace deskflow::display::infra
