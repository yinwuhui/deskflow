// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "MacResourceProvider.h"
#include <QCoreApplication>
#include <algorithm>
namespace deskflow::display::infra {
namespace {
constexpr auto providerId = "macos.cgdisplay.helper.v1";
constexpr auto policy = "native-helper-and-system-absence.v1";
QString outputReference(const WorkerOutputObserved &claim)
{
  return QString("cgoutput1:%1:%2").arg(claim.source.primary).arg(claim.nativeIdentity);
}
std::optional<std::pair<uint32_t, QString>> parseOutput(const QString &s)
{
  const auto parts = s.split(':');
  if (parts.size() != 3 || parts[0] != "cgoutput1")
    return {};
  bool ok = false;
  auto id = parts[1].toULongLong(&ok);
  if (!ok || !id || id > UINT32_MAX || QString::number(id) != parts[1] || parts[2].size() != 36 ||
      parts[2].contains(QChar(0)))
    return {};
  return std::pair<uint32_t, QString>{uint32_t(id), parts[2]};
}
NativeInspection absent(const QString &ref)
{
  return {Presence::Absent, {}, {"systemAbsenceCheck", ref, "verified"}, false};
}
} // namespace
MacResourceProvider::MacResourceProvider(DurableResourceLedger &l, WorkerRegistration r, WorkerBinding b)
    : m_ledger(l),
      m_registration(std::move(r)),
      m_binding(std::move(b)),
      m_thread(QThread::currentThreadId())
{
}
MacResourceProvider::~MacResourceProvider() = default;
ProviderRegistration MacResourceProvider::registration()
{
  // Caller supplies shared ownership explicitly; no hidden raw-this ownership.
  return {providerId, "1", policy, {"worker", "virtualOutput"}, {}, {}};
}
bool MacResourceProvider::scope(const ResourceRecord &r) const
{
  return m_thread == QThread::currentThreadId() && r.provider == providerId && r.providerVersion == "1" &&
         r.verificationPolicy == policy && r.owner.toStdString() == m_binding.token.owner &&
         r.session.toStdString() == m_binding.token.session && r.planHash == m_binding.planHash &&
         r.userSession == m_binding.localUserSession && m_binding.purpose == WorkerPurpose::Platform &&
         m_binding.backend == "platform.macos.virtual-output";
}
bool MacResourceProvider::wait(const std::function<bool()> &done, TimePoint deadline)
{
  while (!done() && Clock::now() < deadline) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
    QThread::msleep(1);
  }
  return done();
}
ResourceEvidence MacResourceProvider::owned() const
{
  return {"verifiedHelperRegistry", QString::fromStdString(m_binding.token.worker), "verified"};
}
ResourceContextSnapshot MacResourceProvider::snapshot(const ResourceRecord &r, TimePoint deadline)
{
  if (!scope(r) || Clock::now() >= deadline || r.bootInstance != macBootInstanceReference() ||
      r.userSession != macUserSessionReference())
    return {};
  const auto d = macDesktopState();
  if (!d.onConsole || !d.lockKnown || d.locked)
    return {};
  const auto inventory = macDisplayInventory();
  return {inventory.complete, inventory.digest};
}
const ResourceRecord *MacResourceProvider::dependency(const ResourceRecord &r) const
{
  if (r.dependencies.size() != 1)
    return nullptr;
  const auto it = m_ledger.records().constFind(r.dependencies.front());
  if (it == m_ledger.records().cend() || it->kind != "worker" || !scope(*it) || it->bootInstance != r.bootInstance ||
      it->userSession != r.userSession)
    return nullptr;
  return &it.value();
}
NativeCreation MacResourceProvider::create(const ResourceRecord &r, TimePoint deadline)
{
  if (!scope(r) || r.bootInstance != macBootInstanceReference() || r.userSession != macUserSessionReference() ||
      m_stopping || Clock::now() >= deadline || m_ledger.records().value(r.id).phase != ResourcePhase::Intent)
    return {CreationCertainty::NotCreated, {}, {}};
  if (r.kind == "worker") {
    if (m_process || !r.dependencies.isEmpty())
      return {CreationCertainty::NotCreated, {}, {}};
    m_workerId = r.id;
    m_process = std::make_unique<WorkerProcess>();
    m_process->onEstablished = [this] { m_established = true; };
    m_process->onExited = [this](int, QProcess::ExitStatus) { m_exited = true; };
    m_process->onMessage = [this](const WorkerMessage &m) {
      if (std::holds_alternative<WorkerReady>(m))
        m_ready = true;
      else if (const auto *c = std::get_if<WorkerOutputObserved>(&m))
        m_claim = *c;
      else if (const auto *ready = std::get_if<WorkerOutputReady>(&m))
        m_outputReady = ready->resourceId == m_outputId;
      else if (const auto *failure = std::get_if<WorkerFailure>(&m))
        m_failure = failure->code;
    };
    if (!m_process->launch(m_registration, m_binding))
      return {};
    if (!wait([&] { return m_established || m_exited; }, deadline) || !m_established || m_exited ||
        !m_process->nativeIdentity())
      return {};
    return {CreationCertainty::Created, m_process->nativeIdentity()->encode(), owned()};
  }
  const auto *helper = dependency(r);
  if (r.kind != "virtualOutput" || !helper || helper->phase != ResourcePhase::Active || helper->id != m_workerId ||
      !m_process || m_exited || !m_outputId.isEmpty())
    return {CreationCertainty::NotCreated, {}, {}};
  m_outputId = r.id;
  WorkerPrepareOutput p{
      r.id, r.creationIntent, r.preCreateContextDigest,
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count())
  };
  if (!m_process->send(p) || !wait([&] { return m_ready || m_exited || !m_failure.isEmpty(); }, deadline) || !m_ready ||
      m_exited || !m_failure.isEmpty() || !m_process->send(WorkerStart{}))
    return {};
  if (!wait([&] { return m_claim || m_exited || !m_failure.isEmpty(); }, deadline) || !m_claim ||
      m_claim->resourceId != r.id || m_claim->source.kind != NativeSourceKind::CgDisplay)
    return {};
  return {CreationCertainty::Created, outputReference(*m_claim), owned()};
}
bool MacResourceProvider::configure(const ResourceRecord &r, TimePoint deadline)
{
  if (!scope(r) || m_stopping || m_exited || !m_process || r.phase != ResourcePhase::Observed ||
      m_ledger.records().value(r.id).phase != ResourcePhase::Observed)
    return false;
  if (r.kind == "worker")
    return r.id == m_workerId && m_established;
  if (r.id != m_outputId || !m_claim || r.platformReference != outputReference(*m_claim) ||
      !m_process->send(WorkerConfigureOutput{r.id}))
    return false;
  return wait([&] { return m_outputReady || m_exited || !m_failure.isEmpty(); }, deadline) && m_outputReady &&
         !m_exited && m_failure.isEmpty();
}
bool MacResourceProvider::stop(TimePoint deadline)
{
  m_stopping = true;
  if (!m_process || m_exited)
    return true;
  m_process->stop(StopReason::Cancelled);
  const auto graceful = std::min(deadline, Clock::now() + std::chrono::seconds(4));
  if (wait([&] { return m_exited; }, graceful))
    return true;
  m_process->terminate();
  return wait([&] { return m_exited; }, deadline);
}
bool MacResourceProvider::helperAbsent(const ResourceRecord &r) const
{
  if (r.id == m_workerId && m_process && m_exited)
    return true;
  if (r.bootInstance != macBootInstanceReference())
    return !r.bootInstance.isEmpty() && !macBootInstanceReference().isEmpty();
  const auto identity = NativeProcessIdentity::decode(r.platformReference);
  return identity && nativeProcessPresence(*identity) == NativeProcessPresence::OriginalAbsent;
}
bool MacResourceProvider::quiesce(const ResourceRecord &r, TimePoint deadline)
{
  if (!scope(r))
    return false;
  if (r.kind == "worker" && r.id == m_workerId)
    return stop(deadline);
  const auto *helper = r.kind == "virtualOutput" ? dependency(r) : nullptr;
  if (helper && helper->id == m_workerId && m_process)
    return stop(deadline);
  // Recovery is read-only. Never signal a live process discovered by persisted PID.
  return r.kind == "worker" ? helperAbsent(r) : helper && helperAbsent(*helper);
}
NativeInspection MacResourceProvider::inspect(const ResourceRecord &r, TimePoint deadline)
{
  if (!scope(r) || Clock::now() >= deadline)
    return {};
  if (r.kind == "worker") {
    if (helperAbsent(r))
      return absent("native-process-absent." + r.id);
    if (r.id == m_workerId && m_process && !m_exited && m_established && m_process->nativeIdentity() &&
        nativeProcessPresence(*m_process->nativeIdentity()) == NativeProcessPresence::Exact)
      return {Presence::Owned, m_process->nativeIdentity()->encode(), owned(), true};
    return {Presence::Ambiguous, {}, {}, false};
  }
  if (r.kind != "virtualOutput")
    return {};
  const auto inventory = macDisplayInventory();
  if (!inventory.complete)
    return {};
  const auto *helper = dependency(r);
  auto ref = parseOutput(r.platformReference);
  if (!r.platformReference.isEmpty() && !ref)
    return {};
  if (!ref && r.id == m_outputId && m_claim)
    ref = std::pair<uint32_t, QString>{uint32_t(m_claim->source.primary), m_claim->nativeIdentity};
  if (ref) {
    auto it = std::find_if(inventory.displays.begin(), inventory.displays.end(), [&](const auto &d) {
      return d.identity == ref->second;
    });
    if (it == inventory.displays.end() && helper && helperAbsent(*helper))
      return absent("native-output-absent." + r.id);
    if (it != inventory.displays.end() && it->id == ref->first && r.id == m_outputId && m_claim && !m_exited &&
        m_process && outputReference(*m_claim) == QString("cgoutput1:%1:%2").arg(ref->first).arg(ref->second)) {
      double right = 0;
      for (const auto &d : inventory.displays)
        if (d.id != it->id)
          right = std::max(right, d.bounds.right());
      const bool topology = m_outputReady && it->active && !it->mirrored && it->pixelWidth == 1920 &&
                            it->pixelHeight == 1080 && it->bounds.width() == 1920 && it->bounds.height() == 1080 &&
                            it->bounds.x() >= right && it->bounds.y() == 0;
      return {Presence::Owned, outputReference(*m_claim), owned(), topology};
    }
    return {Presence::Ambiguous, {}, {}, false};
  }
  if (helper && helperAbsent(*helper) && !macResourceHintPresent(inventory, r.owner, r.id))
    return absent("native-coupled-output-absent." + r.id);
  return {Presence::Ambiguous, {}, {}, false};
}
bool MacResourceProvider::release(const ResourceRecord &r, const ResourceEvidence &, TimePoint deadline)
{
  if (!scope(r))
    return false;
  return (r.id == m_workerId || r.id == m_outputId) && m_process && stop(deadline);
}
} // namespace deskflow::display::infra
