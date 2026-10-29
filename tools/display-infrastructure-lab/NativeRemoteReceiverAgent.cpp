// SPDX-License-Identifier: MIT
// Manual SSH-controlled diagnostic supervisor; never a product trust service.
#include "display/control/ControlBodyRegistry.h"
#include "display/supervision/WorkerProcess.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
using namespace deskflow::display::infra;
static void emitEvent(QJsonObject value)
{
  std::cout << QJsonDocument(value).toJson(QJsonDocument::Compact).constData() << '\n' << std::flush;
}
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  if (argc != 3)
    return 2;
  const QString local = QString::fromLocal8Bit(argv[1]), peer = QString::fromLocal8Bit(argv[2]);
  QHostAddress localIp(local), peerIp(peer);
  if (localIp.protocol() != QAbstractSocket::IPv4Protocol || peerIp.protocol() != QAbstractSocket::IPv4Protocol ||
      localIp.isNull() || peerIp.isNull() || localIp.isLoopback() || peerIp.isLoopback() || localIp.isMulticast() ||
      peerIp.isMulticast())
    return 2;
  QTemporaryDir imageDirectory;
  const auto image = imageDirectory.filePath("receiver");
  if (!QFile::copy(QCoreApplication::applicationDirPath() + "/busymouse-display-media-receiver", image) ||
      !QFile::setPermissions(image, QFileDevice::ReadOwner | QFileDevice::ExeOwner))
    return 3;
  QFile executable(image);
  if (!executable.open(QIODevice::ReadOnly))
    return 3;
  const auto digest =
      QString::fromLatin1(QCryptographicHash::hash(executable.readAll(), QCryptographicHash::Sha256).toHex());
  WorkerProcess worker;
  WorkerPrepare preparation;
  WorkerBinding binding;
  WorkerReceiveBinding target;
  bool configured = false, stopping = false, quiesced = false, failed = false;
  uint64_t presented = 0;
  auto stop = [&] {
    if (!stopping) {
      stopping = true;
      worker.stop(StopReason::ConnectionLost);
    }
  };
  auto fail = [&](const char *code) {
    failed = true;
    emitEvent({{"kind", "failure"}, {"code", code}});
    stop();
  };
  worker.onEstablished = [&] {
    if (!worker.send(preparation) || !worker.send(target))
      fail("RemotePrepareWriteFailed");
  };
  worker.onMessage = [&](const WorkerMessage &message) {
    if (std::holds_alternative<WorkerReady>(message)) {
      if (!worker.send(WorkerStart{})) {
        fail("RemoteStartFailed");
        return;
      }
      emitEvent({{"kind", "ready"}});
    }
    if (auto signal = std::get_if<WorkerMediaSignal>(&message)) {
      ControlError error;
      emitEvent({{"kind", "signal"}, {"body", encodeWebRtcSignalBody({1, *signal}, error)}});
    }
    if (auto frame = std::get_if<WorkerPresented>(&message))
      if (frame->exposed && frame->evidence == PresentationEvidence::NativePaintConfirmed)
        emitEvent({{"kind", "presented"}, {"count", QString::number(++presented)}});
    if (auto error = std::get_if<WorkerFailure>(&message)) {
      failed = true;
      emitEvent({{"kind", "failure"}, {"code", error->code}});
      stop();
    }
    if (std::holds_alternative<WorkerQuiesced>(message))
      quiesced = true;
  };
  worker.onExited = [&](int code, QProcess::ExitStatus status) {
    const bool cleaned = quiesced && code == 0 && status == QProcess::NormalExit;
    const bool clean = cleaned && !failed;
    emitEvent(
        {{"kind", "stopped"},
         {"clean", clean},
         {"quiesced", quiesced},
         {"workerExitedNormally", code == 0 && status == QProcess::NormalExit},
         {"presented", QString::number(presented)},
         {"workerError", worker.error()},
         {"workerExitCode", code},
         {"workerExitStatus", int(status)}}
    );
    app.exit(cleaned ? 0 : 1);
  };
  QByteArray buffer;
  fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
  QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
  QObject::connect(&input, &QSocketNotifier::activated, [&] {
    char bytes[4096];
    const auto n = ::read(STDIN_FILENO, bytes, sizeof(bytes));
    if (n == 0) {
      input.setEnabled(false);
      if (configured)
        stop();
      else
        app.exit(1);
      return;
    }
    if (n < 0)
      return;
    buffer.append(bytes, n);
    if (buffer.size() > 65536) {
      fail("RemoteInputBudget");
      return;
    }
    while (buffer.contains('\n')) {
      const auto line = buffer.left(buffer.indexOf('\n'));
      buffer.remove(0, line.size() + 1);
      ControlError error;
      const auto object = parseBoundedObject(line, 65536, error);
      if (!object) {
        fail("RemoteInputMalformed");
        return;
      }
      const auto kind = object->value("kind").toString();
      if (kind == "configure" && !configured) {
        binding.token = {
            object->value("session").toString().toStdString(),
            1,
            1,
            1,
            QUuid::createUuid().toString(QUuid::Id128).toStdString(),
            QUuid::createUuid().toString(QUuid::Id128).toStdString()
        };
        binding.localUserSession = QString("diagnostic.linux.uid.%1").arg(geteuid());
        binding.buildCompatibility = "remote-receiver-lab1";
        binding.backend = "webrtc";
        binding.profile = "diagnostic.webrtc.h264.control-map.v1";
        binding.planHash = object->value("planHash").toString();
        binding.controlBinding = object->value("controlBinding").toString();
        binding.localIdentity = QString(64, 'b');
        binding.peerIdentity = QString(64, 'a');
        binding.direction = Direction::Receive;
        const auto topology = parseCounter(object->value("topologyGeneration").toString().toStdString());
        if (!validWorkerBinding(binding) || !topology) {
          fail("RemoteConfigurationScope");
          app.exit(1);
          return;
        }
        preparation.allowedPeerAddresses = {peer};
        preparation.startupDeadlineNanoseconds = uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                (Clock::now() + std::chrono::seconds(10)).time_since_epoch()
            )
                .count()
        );
        target = {
            local, object->value("sourceResource").toString(), QUuid::createUuid().toString(QUuid::Id128), *topology
        };
        if (!validId(target.sourceResource.toStdString(), 16)) {
          fail("RemoteSourceScope");
          app.exit(1);
          return;
        }
        configured = true;
        if (!worker.launch({image, digest, binding.backend, binding.profile}, binding)) {
          fail("RemoteLaunchFailed");
          app.exit(1);
          return;
        }
      } else if (kind == "stop") {
        stop();
      } else if (configured && stopping && (kind == "signal" || kind == "correlation")) {
        // Authenticated SSH bytes already in flight are cleanup, not a new command.
        continue;
      } else if (configured && !stopping && kind == "signal") {
        const auto signal = decodeWebRtcSignalBody(object->value("body").toObject(), error);
        if (!signal || !worker.send(signal->signal)) {
          fail("RemoteSignalRejected");
          return;
        }
      } else if (configured && !stopping && kind == "correlation") {
        const auto map = decodeFrameCorrelationBody(object->value("body").toObject(), error);
        if (!map || !worker.send(*map)) {
          fail("RemoteCorrelationRejected");
          return;
        }
      } else {
        fail("RemoteInputState");
        return;
      }
    }
  });
  QTimer::singleShot(60000, &app, [&] {
    if (configured)
      stop();
    else
      app.exit(1);
  });
  return app.exec();
}
