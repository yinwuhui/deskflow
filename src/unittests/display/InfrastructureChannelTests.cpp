// SPDX-License-Identifier: MIT
#include "display/InfrastructureControlChannel.h"
#include "display/control/ControlBodyRegistry.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLocalServer>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <stdexcept>
using namespace deskflow::display;
using namespace deskflow::display::infra;
static void require(bool value, const char *message)
{
  if (!value)
    throw std::runtime_error(message);
}
static void until(const std::function<bool()> &ready)
{
  QElapsedTimer clock;
  clock.start();
  while (!ready() && clock.elapsed() < 3000) {
    QCoreApplication::processEvents();
    QThread::msleep(1);
  }
  require(ready(), "core IPC fixture deadline");
}
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir directory;
    PeerUsageService usage(nullptr, directory.filePath("usage.ini"));
    DisplayService display({}, directory.filePath("display.ini"));
    InputRouteAdapter input(&usage, &display);
    const QString local(64, 'a'), peer(64, 'b'), connection(32, 'c');
    QLocalServer server;
    require(server.listen(directory.filePath("core")), "listen");
    CoreInputChannel core(&input, &usage, nullptr, server.fullServerName());
    InfrastructureControlChannel channel(local, &core, &usage);
    ControlBinding remote(peer, local, "remote.connection");
    QLocalSocket *socket = nullptr;
    QByteArray buffer;
    int ready = 0, lost = 0, received = 0;
    QList<QJsonObject> sent;
    QObject::connect(
        &channel, &InfrastructureControlChannel::bindingReady,
        [&](const QString &, const QString &, const QString &id) {
          require(id == remote.id(), "transcript differs");
          ++ready;
        }
    );
    QObject::connect(&channel, &InfrastructureControlChannel::bindingLost, [&] { ++lost; });
    QObject::connect(&channel, &InfrastructureControlChannel::envelopeReceived, [&] { ++received; });
    auto event = [&](const QJsonObject &body, const QString &id = QString()) {
      QJsonObject value{{"peerId", peer}, {"baseConnectionId", id.isEmpty() ? connection : id}, {"body", body}};
      socket->write("peerUsageControlEvent=" + QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n');
    };
    QObject::connect(&server, &QLocalServer::newConnection, [&] {
      socket = server.nextPendingConnection();
      QObject::connect(socket, &QLocalSocket::readyRead, [&] {
        buffer += socket->readAll();
        while (buffer.contains('\n')) {
          auto line = buffer.left(buffer.indexOf('\n'));
          buffer.remove(0, line.size() + 1);
          if (line.startsWith("hello=")) {
            socket->write(line + '\n');
            QJsonObject record{
                {"peerId", peer},
                {"baseConnectionId", connection},
                {"baseConnected", true},
                {"inputControlSupported", true},
                {"inputActive", false},
                {"inputPermissionsReady", true},
                {"originalInputRole", "source"}
            };
            socket->write("peerUsageConnection=" + QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n');
          } else if (line.startsWith("inputControl=")) {
            const auto body = QJsonDocument::fromJson(line.mid(13)).object().value("body").toObject();
            if (body.value("protocol") == "display-infra-bind/1") {
              ControlError error;
              require(
                  remote.acceptHello(
                      QJsonDocument(body).toJson(QJsonDocument::Compact), local, "remote.connection", error
                  ),
                  "remote hello"
              );
              event(QJsonDocument::fromJson(remote.hello()).object());
            } else if (body.value("protocol") == "display-infra/1")
              sent.append(body);
          }
        }
      });
    });
    until([&] { return ready == 1; });
    Envelope stop{
        "Stop",
        QString(32, 'd'),
        QString(32, 'e'),
        QString(64, 'f'),
        1,
        0,
        {{"operationId", QString(32, '1')}, {"reason", "Cancelled"}, {"lastEpoch", 1}, {"lastSequence", "0"}}
    };
    ControlError error;
    const auto wire = remote.encode(stop, error);
    require(!wire.isEmpty(), "remote stop encode");
    event(QJsonDocument::fromJson(wire).object(), QString(32, '2'));
    QCoreApplication::processEvents();
    require(received == 0, "wrong connection admitted");
    event(QJsonDocument::fromJson(wire).object());
    until([&] { return received == 1; });
    event(QJsonDocument::fromJson(wire).object());
    WorkerMediaSignal offer;
    offer.description = QString(20000, 'x');
    Envelope huge = stop;
    huge.type = "MediaSignal";
    huge.body = encodeWebRtcSignalBody({1, offer}, error);
    require(!huge.body.isEmpty(), "oversized fixture malformed");
    require(!channel.send(connection, huge), "wrapped core budget bypass");
    require(channel.send(connection, stop), "valid control send");
    until([&] { return sent.size() == 1; });
    require(sent.front().value("sequence") == "1", "rejected send consumed sequence");
    require(received == 1, "replay admitted");
    socket->disconnectFromServer();
    until([&] { return lost == 1; });
    require(!channel.send(connection, stop), "disconnected binding retained");
    std::cout << "core socket binding, receive/replay, wrapped budget and disconnect passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
