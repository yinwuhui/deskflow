// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "display/core/InfrastructureSession.h"
#include <QByteArray>
#include <QString>
#include <variant>

namespace deskflow::display::infra {
// This is control IPC only. Native surfaces use OS handle transfer separately.
struct WorkerBinding {
  SessionToken token;
  QString localUserSession, buildCompatibility, backend, profile, planHash;
  bool operator==(const WorkerBinding&) const = default;
};
enum class WorkerSide { Agent, Worker };
enum class StopReason { Cancelled, Revoked, ConnectionLost, Deadline, Shutdown };
struct WorkerStart { bool operator==(const WorkerStart&) const = default; };
struct WorkerStop { StopReason reason = StopReason::Cancelled; bool operator==(const WorkerStop&) const = default; };
struct WorkerHeartbeat { bool operator==(const WorkerHeartbeat&) const = default; };
struct WorkerRefresh { bool operator==(const WorkerRefresh&) const = default; };
struct WorkerReady { bool operator==(const WorkerReady&) const = default; };
struct WorkerPresented { uint64_t frame = 0; PresentationEvidence evidence = PresentationEvidence::Decoded;
  bool exposed = false; bool operator==(const WorkerPresented&) const = default; };
struct WorkerQuiesced { bool operator==(const WorkerQuiesced&) const = default; };
struct WorkerFailure { QString code; bool operator==(const WorkerFailure&) const = default; };
using WorkerMessage = std::variant<WorkerStart, WorkerStop, WorkerHeartbeat, WorkerRefresh,
  WorkerReady, WorkerPresented, WorkerQuiesced, WorkerFailure>;

// Single event-loop owner. Both ends require an independently authenticated local
// transport. A secret delivered over an inherited private channel authenticates
// the launch instance; it must never appear in argv, environment or diagnostics.
class WorkerProtocol {
public:
  static constexpr qsizetype MaxMessageBytes = 8192;
  static constexpr qsizetype MaxPendingMessages = 32;
  WorkerProtocol(WorkerSide, WorkerBinding, QByteArray startupSecret, TimePoint now);
  ~WorkerProtocol();
  WorkerProtocol(const WorkerProtocol&) = delete;
  WorkerProtocol& operator=(const WorkerProtocol&) = delete;
  QByteArray hello(); // Exactly once; length prefixed like every other message.
  bool feed(const QByteArray&, bool authenticatedLocalTransport, TimePoint now);
  QByteArray encode(const WorkerMessage&, TimePoint now);
  std::vector<WorkerMessage> takeMessages();
  void tick(TimePoint now);
  void disconnected();
  bool established() const { return m_established && !failed(); }
  bool failed() const { return !m_error.isEmpty(); }
  QString error() const { return m_error; } // Fixed local codes; no peer-provided strings.
private:
  bool consume(const QByteArray&, TimePoint now);
  bool acceptMessage(const WorkerMessage&, bool outbound);
  void fail(const QString&);
  QByteArray context() const;
  WorkerSide m_side;
  WorkerBinding m_binding;
  QByteArray m_secret, m_nonce, m_remoteNonce, m_key, m_buffer;
  QString m_error;
  TimePoint m_handshakeDeadline, m_leaseDeadline;
  uint64_t m_tx = 0, m_rx = 0, m_lastFrame = 0;
  bool m_helloSent = false, m_established = false, m_ready = false, m_started = false,
       m_stopping = false, m_quiesced = false;
  std::vector<WorkerMessage> m_pending;
};
} // namespace deskflow::display::infra
