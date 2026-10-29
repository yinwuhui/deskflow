// SPDX-License-Identifier: MIT
#pragma once
// Diagnostic transport only. SSH host authentication does not make the fixture
// peer identities into product certificate identities or a production grant.
#include "display/control/ControlBodyRegistry.h"
#include "display/media/gstreamer/NativeWebRtcEngine.h"
#include <QJsonDocument>
#include <QProcess>
using namespace deskflow::display::infra;
class LabReceiver
{
public:
  LabReceiver(QString localAddress = {}, QString senderAddress = {})
      : m_remote(!localAddress.isEmpty()),
        m_local(std::move(localAddress)),
        m_sender(std::move(senderAddress))
  {
    QObject::connect(&m_process, &QProcess::readyReadStandardOutput, [this] { read(); });
    QObject::connect(&m_process, &QProcess::errorOccurred, [this] { m_error = "RemoteAgentProcessError"; });
    QObject::connect(
        &m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        [this](int code, QProcess::ExitStatus status) {
          read();
          m_exited = true;
          if ((code != 0 || status != QProcess::NormalExit) && m_error.isEmpty())
            m_error = "RemoteAgentAbnormalExit";
        }
    );
  }
  ~LabReceiver()
  {
    if (m_process.state() != QProcess::NotRunning) {
      m_process.closeWriteChannel();
      m_process.waitForFinished(6000);
    }
  }
  bool prepare(const NativeWebRtcConfiguration &config)
  {
    if (!m_remote)
      return m_engine.prepare(config);
    m_config = config;
    m_process.setProgram("/usr/bin/ssh");
    m_process.setArguments(
        {"-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "ywh@10.20.1.71", "env", "XDG_RUNTIME_DIR=/run/user/1000",
         "WAYLAND_DISPLAY=wayland-0", "QT_QPA_PLATFORM=wayland",
         "/home/ywh/code/github/deskflow/build/bin/NativeRemoteReceiverAgent", m_local, m_sender}
    );
    QObject::connect(&m_process, &QProcess::started, [this] {
      write(
          {{"kind", "configure"},
           {"session", QString::fromStdString(m_config.binding.token.session)},
           {"planHash", m_config.binding.planHash},
           {"controlBinding", m_config.binding.controlBinding},
           {"sourceResource", m_config.streamSourceResource},
           {"topologyGeneration", QString::number(m_config.streamTopologyGeneration)}}
      );
    });
    m_process.start();
    return true;
  }
  bool start()
  {
    return m_remote || m_engine.start();
  }
  bool ready() const
  {
    return !m_remote || m_ready;
  }
  void poll()
  {
    if (!m_remote)
      m_engine.poll();
  }
  QString error() const
  {
    return m_remote ? m_error : m_engine.error();
  }
  bool applySignal(const WorkerMediaSignal &value)
  {
    if (m_disconnecting)
      return true;
    if (!m_remote)
      return m_engine.applySignal(value);
    ControlError error;
    return write({{"kind", "signal"}, {"body", encodeWebRtcSignalBody({1, value}, error)}});
  }
  bool applyCorrelation(const WebRtcFrameCorrelation &value)
  {
    if (m_disconnecting)
      return true;
    if (!m_remote)
      return m_engine.applyCorrelation(value);
    ControlError error;
    return write({{"kind", "correlation"}, {"body", encodeFrameCorrelationBody(value, error)}});
  }
  std::vector<WorkerMediaSignal> takeSignals()
  {
    if (!m_remote)
      return m_engine.takeSignals();
    return std::exchange(m_signals, {});
  }
  std::shared_ptr<CpuFrameLease> takeDecoded()
  {
    return m_remote ? nullptr : m_engine.takeDecoded();
  }
  bool stop()
  {
    if (!m_remote)
      return m_engine.stop();
    m_stopping = true;
    if (m_disconnecting || m_exited)
      return true;
    return write({{"kind", "stop"}});
  }
  bool stopped() const
  {
    return m_remote ? (m_exited && m_clean) : m_engine.stopped();
  }
  void disconnectControl()
  {
    m_disconnecting = true;
    m_stopping = true;
    m_process.closeWriteChannel();
  }
  QJsonObject workerDiagnostic() const
  {
    return m_workerDiagnostic;
  }
  QString lateError() const
  {
    return m_lateError;
  }
  bool remote() const
  {
    return m_remote;
  }
  uint64_t presented() const
  {
    return m_presented;
  }

private:
  bool write(const QJsonObject &body)
  {
    const auto bytes = QJsonDocument(body).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > 65536 || m_process.bytesToWrite() + bytes.size() > 131072)
      return false;
    return m_process.write(bytes) == bytes.size();
  }
  void read()
  {
    m_buffer += m_process.readAllStandardOutput();
    if (m_buffer.size() > 131072) {
      m_error = "RemoteAgentReadBudget";
      m_process.closeWriteChannel();
      return;
    }
    while (m_buffer.contains('\n')) {
      const auto line = m_buffer.left(m_buffer.indexOf('\n'));
      m_buffer.remove(0, line.size() + 1);
      ControlError error;
      const auto o = parseBoundedObject(line, 65536, error);
      if (!o) {
        m_error = "RemoteAgentInvalidEvent";
        return;
      }
      const auto kind = o->value("kind").toString();
      if (kind == "ready")
        m_ready = true;
      else if (kind == "failure") {
        if (m_stopping)
          m_lateError = o->value("code").toString();
        else if (m_error.isEmpty())
          m_error = o->value("code").toString();
      } else if (kind == "presented")
        m_presented = o->value("count").toString().toULongLong();
      else if (kind == "stopped") {
        m_clean = o->value("quiesced").toBool() && o->value("workerExitedNormally").toBool();
        m_workerDiagnostic = *o;
      } else if (kind == "signal") {
        auto signal = decodeWebRtcSignalBody(o->value("body").toObject(), error);
        if (!signal || m_signals.size() >= 72) {
          m_error = "RemoteAgentSignalBudget";
          return;
        }
        m_signals.push_back(signal->signal);
      } else {
        m_error = "RemoteAgentUnknownEvent";
        return;
      }
    }
  }
  bool m_remote = false, m_ready = false, m_exited = false, m_clean = false, m_disconnecting = false,
       m_stopping = false;
  QString m_local, m_sender, m_error, m_lateError;
  QProcess m_process;
  QByteArray m_buffer;
  NativeWebRtcConfiguration m_config;
  QJsonObject m_workerDiagnostic;
  NativeWebRtcEngine m_engine;
  std::vector<WorkerMediaSignal> m_signals;
  uint64_t m_presented = 0;
};
