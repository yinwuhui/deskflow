// SPDX-License-Identifier: MIT
#pragma once
// Manual desktop-lab owner. No production service/IPC installation is implied.
#include "display/platform/macos/MacResourceProvider.h"
#include <QEventLoop>
#include <QScopedValueRollback>
#include <QTimer>
#include <iostream>
#include <mutex>
#include <utility>
using namespace deskflow::display::infra;
class LabMacResourceThread final : public QThread
{
public:
  struct Snapshot
  {
    bool initialized = false, cleanupDone = false, ownerClosed = false, isolated = false;
    uint64_t bindingVersion = 0;
    uint32_t recordCount = 0;
    ResourceResult creation, cleanup;
    QString providerError;
    std::optional<MacCaptureBinding> binding;
  };
  LabMacResourceThread(
      QString path, WorkerRegistration registration, WorkerBinding binding, ResourceRecord worker,
      ResourceRecord output, ResourceAuthorization authorization, std::shared_ptr<std::atomic<bool>> mediaQuiesced
  )
      : m_path(std::move(path)),
        m_registration(std::move(registration)),
        m_binding(std::move(binding)),
        m_worker(std::move(worker)),
        m_output(std::move(output)),
        m_auth(std::move(authorization)),
        m_mediaQuiesced(std::move(mediaQuiesced)),
        m_caller(QThread::currentThreadId())
  {
  }
  ~LabMacResourceThread() override
  {
    if (!isRunning())
      return;
    if (m_auth.revoked)
      m_auth.revoked->store(true, std::memory_order_release);
    requestInterruption();
    // This developer process must never destroy a running QThread or falsely
    // close its ledger. A stuck native/kernel call requires process isolation;
    // retain the audit directory and fail the entire lab if it cannot join.
    if (!wait(10000)) {
      std::cerr << "ResourceThreadJoinUnconfirmed; audit ledger retained\n" << std::flush;
      std::_Exit(76);
    }
  }
  Snapshot snapshot() const
  {
    std::lock_guard lock(m_mutex);
    return m_snapshot;
  }
  bool refreshBinding(TimePoint deadline)
  {
    std::lock_guard lock(m_mutex);
    if (m_refresh || m_closing || m_snapshot.cleanupDone)
      return false;
    m_bindingDeadline = deadline;
    m_refresh = true;
    return true;
  }
  bool cleanup(TimePoint deadline)
  {
    std::lock_guard lock(m_mutex);
    if (m_closing || m_snapshot.cleanupDone)
      return false;
    if (m_auth.revoked)
      m_auth.revoked->store(true, std::memory_order_release);
    m_cleanupDeadline = deadline;
    m_cleanupRequested = true;
    m_closing = true;
    return true;
  }

protected:
  void run() override
  {
    @autoreleasepool {
      try {
        DurableResourceLedger ledger(m_path);
        auto provider = std::make_shared<MacResourceProvider>(ledger, m_registration, m_binding);
        ResourceExecutor executor(ledger, m_worker.userSession, m_worker.bootInstance, [&](const QString &owner) {
          return owner == m_worker.owner && m_mediaQuiesced->load(std::memory_order_acquire);
        });
        auto registration = provider->registration();
        registration.implementation = provider;
        ResourceResult created;
        if (!executor.registerProvider(std::move(registration)))
          created.primaryError = "ResourceProviderRegistration";
        else {
          created = executor.create(m_worker, m_auth, m_auth.expires);
          if (created.outcome == ResourceOutcome::Active)
            created = executor.create(m_output, m_auth, m_auth.expires);
        }
        QString bindingError;
        auto binding =
            created.outcome == ResourceOutcome::Active
                ? provider->captureBinding(m_output.id, Clock::now() + std::chrono::seconds(3), &bindingError)
                : std::optional<MacCaptureBinding>{};
        {
          std::lock_guard lock(m_mutex);
          m_snapshot.creation = created;
          m_snapshot.recordCount = uint32_t(ledger.records().size());
          m_snapshot.binding = std::move(binding);
          m_snapshot.bindingVersion = 1;
          m_snapshot.providerError = provider->lastError() + bindingError;
          m_snapshot.isolated = QThread::currentThreadId() != m_caller;
          m_snapshot.initialized = true;
        }
        QEventLoop loop;
        QTimer timer;
        timer.setInterval(5);
        bool working = false;
        QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
          if (working)
            return;
          QScopedValueRollback<bool> busy(working, true);
          bool cleanup = false, refresh = false;
          TimePoint deadline;
          {
            std::lock_guard lock(m_mutex);
            cleanup = std::exchange(m_cleanupRequested, false) || isInterruptionRequested();
            if (cleanup) {
              m_closing = true;
              deadline = isInterruptionRequested() ? Clock::now() + std::chrono::seconds(5) : m_cleanupDeadline;
            } else if (std::exchange(m_refresh, false)) {
              refresh = true;
              deadline = m_bindingDeadline;
            }
          }
          if (cleanup) {
            if (m_auth.revoked)
              m_auth.revoked->store(true, std::memory_order_release);
            const auto result = executor.cleanupOwner(m_worker.owner, deadline);
            {
              std::lock_guard lock(m_mutex);
              m_snapshot.cleanup = result;
              m_snapshot.recordCount = uint32_t(ledger.records().size());
              m_snapshot.ownerClosed = ledger.activeOwner().isEmpty();
              m_snapshot.binding.reset();
              m_snapshot.cleanupDone = true;
              m_snapshot.providerError = provider->lastError();
            }
            loop.quit();
          } else if (refresh) {
            QString error;
            auto binding = provider->captureBinding(m_output.id, deadline, &error);
            std::lock_guard lock(m_mutex);
            m_snapshot.binding = std::move(binding);
            ++m_snapshot.bindingVersion;
            m_snapshot.providerError = provider->lastError() + error;
          }
        });
        timer.start();
        loop.exec();
      } catch (const std::exception &) {
        std::lock_guard lock(m_mutex);
        m_snapshot.creation.primaryError = "ResourceOwnerThreadException";
        m_snapshot.initialized = true;
      }
    }
  }

private:
  QString m_path;
  WorkerRegistration m_registration;
  WorkerBinding m_binding;
  ResourceRecord m_worker, m_output;
  ResourceAuthorization m_auth;
  std::shared_ptr<std::atomic<bool>> m_mediaQuiesced;
  Qt::HANDLE m_caller;
  mutable std::mutex m_mutex;
  Snapshot m_snapshot;
  bool m_refresh = false, m_cleanupRequested = false, m_closing = false;
  TimePoint m_bindingDeadline{}, m_cleanupDeadline{};
};
