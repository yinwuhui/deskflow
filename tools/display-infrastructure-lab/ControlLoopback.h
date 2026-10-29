// SPDX-License-Identifier: MIT
#pragma once
#include "display/control/ControlBodyRegistry.h"
#include "display/media/gstreamer/NativeWebRtcEngine.h"
#include "display/supervision/WorkerProcess.h"
#include <QUuid>
#include <stdexcept>
using namespace deskflow::display::infra;
// In-memory developer transport only. The identity assertion comes from this
// fixture, NOT a TLS/device-authentication proof. Exercise the actual control
// wire codec, binding, sequence and exact session/plan scope before SDK calls.
class LabControlLoopback
{
public:
  LabControlLoopback(NativeWebRtcConfiguration &send, NativeWebRtcConfiguration &receive)
      : m_left(send.binding.localIdentity, receive.binding.localIdentity, "lab.sender"),
        m_right(receive.binding.localIdentity, send.binding.localIdentity, "lab.receiver"),
        m_sender(send.binding.localIdentity),
        m_receiver(receive.binding.localIdentity),
        m_session(QString::fromStdString(send.binding.token.session)),
        m_plan(send.binding.planHash),
        m_attempt(send.binding.token.attempt),
        m_epoch(send.binding.token.epoch)
  {
    if (send.binding.direction != Direction::Send || receive.binding.direction != Direction::Receive ||
        send.binding.peerIdentity != m_receiver || receive.binding.peerIdentity != m_sender ||
        receive.binding.token.session != send.binding.token.session || receive.binding.token.attempt != m_attempt ||
        receive.binding.token.epoch != m_epoch || receive.binding.planHash != m_plan)
      throw std::runtime_error("ControlLoopbackScopeMismatch");
    ControlError error;
    if (!m_left.acceptHello(m_right.hello(), m_receiver, "lab.sender", error) ||
        !m_right.acceptHello(m_left.hello(), m_sender, "lab.receiver", error) || m_left.id() != m_right.id())
      throw std::runtime_error("ControlLoopbackBindingFailed");
    send.binding.controlBinding = receive.binding.controlBinding = m_left.id();
    // Explicit diagnostic owner grant, before either engine is prepared.
    send.permit = MediaWorkPermit::issue(send, Clock::now() + std::chrono::seconds(30));
    receive.permit = MediaWorkPermit::issue(receive, Clock::now() + std::chrono::seconds(30));
    if (!send.permit || !receive.permit)
      throw std::runtime_error("DiagnosticPermitRejected");
  }
  LabControlLoopback(const LabControlLoopback &) = delete;
  LabControlLoopback &operator=(const LabControlLoopback &) = delete;
  void pump(NativeWebRtcEngine &send, NativeWebRtcEngine &receive)
  {
    for (const auto &s : send.takeSignals())
      signal(s, true, receive);
    for (const auto &s : receive.takeSignals())
      signal(s, false, send);
    for (const auto &mapping : send.takeCorrelations()) {
      ControlError error;
      const auto body = encodeFrameCorrelationBody(mapping, error);
      if (body.isEmpty())
        fail(error);
      const auto envelope = transfer("FrameCorrelation", body, true);
      const auto decoded = decodeFrameCorrelationBody(envelope.body, error);
      if (!decoded)
        fail(error);
      if (decoded->epoch != m_epoch)
        throw std::runtime_error("FrameCorrelationEpochMismatch");
      if (!receive.applyCorrelation(*decoded))
        throw std::runtime_error(
            ("FrameCorrelationRejected/" + receive.error() + "/frame=" + QString::number(mapping.frame)).toStdString()
        );
      ++m_correlations;
    }
    if (!receive.takeCorrelations().empty())
      throw std::runtime_error("ReverseFrameCorrelationDenied");
  }
  template <class Receiver> void fromSenderWorker(const WorkerMessage &message, Receiver &receiver)
  {
    if (auto s = std::get_if<WorkerMediaSignal>(&message))
      signal(*s, true, receiver);
    if (auto map = std::get_if<WebRtcFrameCorrelation>(&message)) {
      ControlError error;
      const auto wire = transfer("FrameCorrelation", encodeFrameCorrelationBody(*map, error), true);
      const auto decoded = decodeFrameCorrelationBody(wire.body, error);
      if (!decoded || decoded->epoch != m_epoch || !receiver.applyCorrelation(*decoded))
        throw std::runtime_error("WorkerFrameCorrelationRejected");
      ++m_correlations;
    }
  }
  template <class Receiver> void toSenderWorker(Receiver &receiver, WorkerProcess &sender)
  {
    for (const auto &s : receiver.takeSignals()) {
      ControlError error;
      const auto wire = transfer("MediaSignal", encodeWebRtcSignalBody({++m_rightSignals, s}, error), false);
      const auto decoded = decodeWebRtcSignalBody(wire.body, error);
      if (!decoded || !sender.send(decoded->signal))
        throw std::runtime_error("WorkerReturnSignalRejected");
    }
  }
  uint64_t serializedCorrelations() const
  {
    return m_correlations;
  }

private:
  [[noreturn]] static void fail(const ControlError &error)
  {
    throw std::runtime_error(("SerializedControlRejected/" + error.code).toStdString());
  }
  Envelope transfer(const QString &type, const QJsonObject &body, bool fromSender)
  {
    auto &source = fromSender ? m_left : m_right;
    auto &target = fromSender ? m_right : m_left;
    Envelope e{type, QUuid::createUuid().toString(QUuid::Id128), m_session, m_plan, m_attempt, 0, body};
    ControlError error;
    const auto bytes = source.encode(e, error);
    if (bytes.isEmpty())
      fail(error);
    const auto received =
        target.receive(bytes, fromSender ? m_sender : m_receiver, fromSender ? "lab.receiver" : "lab.sender", error);
    if (!received)
      fail(error);
    if (received->sessionId != m_session || received->attempt != m_attempt || received->planHash != m_plan)
      throw std::runtime_error("SerializedControlScopeMismatch");
    return *received;
  }
  template <class Receiver> void signal(const WorkerMediaSignal &s, bool fromSender, Receiver &target)
  {
    auto &sequence = fromSender ? m_leftSignals : m_rightSignals;
    if (sequence == UINT64_MAX)
      throw std::runtime_error("ControlSignalSequenceExhausted");
    ControlError error;
    const auto body = encodeWebRtcSignalBody({++sequence, s}, error);
    if (body.isEmpty())
      fail(error);
    const auto e = transfer("MediaSignal", body, fromSender);
    const auto decoded = decodeWebRtcSignalBody(e.body, error);
    if (!decoded || decoded->sequence != sequence)
      fail(error);
    if (!target.applySignal(decoded->signal))
      throw std::runtime_error(target.error().toStdString());
  }
  ControlBinding m_left, m_right;
  QString m_sender, m_receiver, m_session, m_plan;
  uint64_t m_attempt, m_correlations = 0, m_leftSignals = 0, m_rightSignals = 0;
  uint32_t m_epoch;
};
