// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/supervision/WorkerProcess.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif
#include <iostream>
#include <stdexcept>
using namespace deskflow::display::infra;
static void require(bool b, const char *why)
{
  if (!b)
    throw std::runtime_error(why);
}
static bool until(const std::function<bool()> &f, int ms = 6000)
{
  QElapsedTimer timer;
  timer.start();
  while (!f() && timer.elapsed() < ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(1);
  }
  return f();
}
static WorkerBinding binding(QString profile = "fixture.clean")
{
  return {
      {std::string(32, '1'), 1, 1, 1, std::string(32, '2'), std::string(32, '3')},
      "test.user",
      "infra1",
      "webrtc",
      profile,
      QString(64, 'a'),
      QString(64, 'b'),
      QString(64, 'c'),
      QString(64, 'd'),
      Direction::Send
  };
}
static WorkerPrepare preparation(TimePoint now)
{
  WorkerPrepare p;
  p.sourceResource = QString(32, '4');
  p.source = {NativeSourceKind::CgDisplay, 77, 0};
  p.sourceIdentity = "11111111-2222-3333-4444-555555555555";
  p.sourceContextDigest = QString(64, 'a');
  p.sourceTopologyGeneration = 1;
  p.allowedPeerAddresses = {"10.20.1.71"};
  p.startupDeadlineNanoseconds = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>((now + std::chrono::seconds(10)).time_since_epoch()).count()
  );
  return p;
}
static WorkerRegistration registration(const WorkerBinding &b)
{
  // Ubuntu's build umask produces group-writable binaries. Exercise the real
  // registration policy with a private, immutable-to-other-users fixture copy.
  static QTemporaryDir directory;
  require(directory.isValid(), "private fixture directory");
  const auto fixturePath = directory.path() + "/worker-fixture";
  if (!QFileInfo::exists(fixturePath)) {
    require(QFile::copy(QCoreApplication::applicationFilePath(), fixturePath), "copy worker fixture");
    require(
        QFile::setPermissions(
            fixturePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                             QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther |
                             QFileDevice::ExeOther
        ),
        "secure fixture permissions"
    );
  }
  QFile f(fixturePath);
  require(f.open(QIODevice::ReadOnly), "read own executable");
  QCryptographicHash h(QCryptographicHash::Sha256);
  require(h.addData(&f), "hash own executable");
  return {QFileInfo(f).canonicalFilePath(), QString::fromLatin1(h.result().toHex()), b.backend, b.profile};
}
static int fixture(QCoreApplication &app)
{
  QString error;
  auto boot = readWorkerBootstrap(error);
  if (!boot)
    return 42;
  const auto expectedBinding = boot->binding;
  const auto profile = boot->binding.profile;
  std::optional<WorkerSourceLease> source;
  if (profile == "fixture.slow")
    QThread::sleep(4);
  if (profile == "fixture.wrongpeer")
    ++boot->parentPid;
  WorkerConnection connection(std::move(*boot));
  connection.onShutdown = [&] { app.exit(profile == "fixture.finalclose" ? 0 : 43); };

  connection.onMessage = [&](const WorkerMessage &m) {
    if (auto prepare = std::get_if<WorkerPrepare>(&m)) {
      if (profile == "fixture.sourcelease") {
        source = connection.takeSourceLease();
        if (!source || !source->active() || source->binding() != expectedBinding || source->preparation() != *prepare ||
            connection.takeSourceLease()) {
          connection.send(WorkerFailure{"SourceLeaseInvalid"});
          return;
        }
      }
      connection.send(WorkerReady{});
    }
    if (std::holds_alternative<WorkerStart>(m)) {
      if (profile == "fixture.crash")
        std::_Exit(17);
      connection.send(WorkerPresented{1, PresentationEvidence::NativePaintConfirmed, true});
    }
    if (std::holds_alternative<WorkerStop>(m) && profile != "fixture.ignorestop") {
      if (source && source->active()) {
        connection.send(WorkerFailure{"SourceLeaseNotRevoked"});
        return;
      }
      connection.send(WorkerQuiesced{});
      if (profile == "fixture.finalclose")
        connection.disconnectAfterWrites();
      else
        QTimer::singleShot(50, &app, [&] { app.exit(0); });
    }
  };
  QTimer::singleShot(0, &connection, [&] { connection.connectToAgent(); });
  return app.exec();
}
static bool launchPrepared(WorkerProcess &p, const WorkerBinding &b)
{
  auto callback = p.onEstablished;
  p.onEstablished = [&p, callback] {
    require(p.send(preparation(Clock::now())), "prepare native worker");
    if (callback)
      callback();
  };
  return p.launch(registration(b), b);
}
static void lifecycle(bool finalClose = false, bool sourceLease = false)
{
  WorkerProcess p;
  auto b = binding(sourceLease ? "fixture.sourcelease" : finalClose ? "fixture.finalclose" : "fixture.clean");
  bool established = false, presented = false, quiesced = false, exited = false;
  int exit = -1;
  p.onEstablished = [&] { established = true; };
  p.onMessage = [&](const WorkerMessage &m) {
    if (std::holds_alternative<WorkerReady>(m))
      require(p.send(WorkerStart{}), "start write");
    if (std::holds_alternative<WorkerPresented>(m)) {
      presented = true;
      p.stop(StopReason::Cancelled);
    }
    if (std::holds_alternative<WorkerQuiesced>(m))
      quiesced = true;
  };
  p.onExited = [&](int code, QProcess::ExitStatus) {
    exit = code;
    exited = true;
  };
  require(launchPrepared(p, b), "launch");
  require(until([&] { return exited; }), "worker did not exit");
  if (!p.error().isEmpty())
    std::cerr << p.error().toStdString() << '\n';
  require(
      established && presented && quiesced && exit == 0 && p.error().isEmpty(), "lifecycle or native credentials failed"
  );
  require(!p.launch(registration(b), b), "supervisor reused launch identity");
}
static void executableBinding()
{
  auto b = binding();
  auto r = registration(b);
  QProcess process;
  require(prepareWorkerProcess(process, r, {"--execution-marker"}), "prepare registered image");
#ifdef Q_OS_LINUX
  // Replace the pathname after validation, before start. The replacement must
  // never run: the descriptor refers to the original now-unlinked executable.
  const auto original = r.executable + ".original";
  require(QFile::rename(r.executable, original), "move validated executable");
  QFile replacement(r.executable);
  require(replacement.open(QIODevice::WriteOnly), "replacement fixture");
  require(replacement.write("#!/bin/sh\nexit 91\n") > 0, "write replacement");
  replacement.close();
  require(replacement.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), "replacement mode");
#endif
  process.start();
  require(process.waitForStarted(2000), "start registered image");
  require(process.waitForFinished(3000), "registered image did not exit");
  require(
      process.exitStatus() == QProcess::NormalExit && process.exitCode() == 73,
      "executed replacement instead of registered image"
  );
}
static void registrationGates()
{
  auto b = binding();
  auto r = registration(b);
  r.sha256 = QString(64, 'b');
  WorkerProcess p;
  require(!p.launch(r, b) && !p.running(), "unregistered executable launched");
  auto good = registration(b);
  good.profile = "fixture.other";
  WorkerProcess other;
  require(!other.launch(good, b), "wrong pipeline launched");
#ifdef Q_OS_UNIX
  QTemporaryDir temp;
  require(temp.isValid(), "registration test directory");
  auto writable = registration(b);
  const auto path = temp.path() + "/writable-worker";
  require(QFile::copy(writable.executable, path), "copy writable fixture");
  require(
      QFile::setPermissions(
          path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                    QFileDevice::WriteGroup | QFileDevice::ExeGroup
      ),
      "make fixture group writable"
  );
  writable.executable = QFileInfo(path).canonicalFilePath();
  WorkerProcess unsafe;
  require(!unsafe.launch(writable, b), "group-writable registered executable launched");
  require(
      ::chmod(QFile::encodeName(path).constData(), S_IRUSR | S_IWUSR | S_IXUSR | S_ISUID) == 0,
      "set elevated fixture mode"
  );
  require(!validWorkerExecutable(writable), "setuid registered executable accepted");
  require(
      ::chmod(QFile::encodeName(path).constData(), S_IRUSR | S_IWUSR | S_IXUSR | S_ISGID) == 0,
      "set group elevated fixture mode"
  );
  require(!validWorkerExecutable(writable), "setgid registered executable accepted");
#ifdef Q_OS_MACOS
  require(::chmod(QFile::encodeName(path).constData(), S_IRUSR | S_IWUSR | S_IXUSR) == 0, "restore fixture mode");
  require(::chmod(QFile::encodeName(temp.path()).constData(), 0770) == 0, "make parent group writable");
  require(!validWorkerExecutable(writable), "writable executable ancestor accepted");
  require(::chmod(QFile::encodeName(temp.path()).constData(), 0700) == 0, "restore private directory");
  require(validWorkerExecutable(writable), "secure executable under sticky temporary root rejected");
  auto chmodAcl = [&](QStringList arguments) {
    QProcess process;
    process.start("/bin/chmod", arguments);
    require(
        process.waitForStarted(2000) && process.waitForFinished(2000) && process.exitCode() == 0,
        "ACL fixture command failed"
    );
  };
  chmodAcl({"+a", "everyone allow write", path});
  require(!validWorkerExecutable(writable), "ACL-writable executable accepted");
  chmodAcl({"-N", path});
  chmodAcl({"+a", "everyone allow add_file", temp.path()});
  require(!validWorkerExecutable(writable), "ACL-writable ancestor accepted");
  chmodAcl({"-N", temp.path()});
  require(validWorkerExecutable(writable), "restored private executable rejected");
#endif
  const auto link = temp.path() + "/linked-worker";
  require(QFile::link(registration(b).executable, link), "create fixture symlink");
  auto linked = registration(b);
  linked.executable = link;
  WorkerProcess symlink;
  require(!symlink.launch(linked, b), "symlinked executable launched");
#endif
}
static void failures()
{
  for (const auto &profile : {"fixture.crash", "fixture.slow", "fixture.wrongpeer"}) {
    WorkerProcess p;
    const auto b = binding(profile);
    bool exited = false;
    p.onMessage = [&](const WorkerMessage &m) {
      if (std::holds_alternative<WorkerReady>(m))
        p.send(WorkerStart{});
    };
    p.onExited = [&](int, QProcess::ExitStatus) { exited = true; };
    require(launchPrepared(p, b), "fixture launch");
    require(until([&] { return exited; }, 5000), "failed worker survived");
    require(!p.error().isEmpty(), "failed worker reported successful exit");
  }
}
static void stopDeadline()
{
  WorkerProcess p;
  auto b = binding("fixture.ignorestop");
  bool started = false, exited = false;
  p.onMessage = [&](const WorkerMessage &m) {
    if (std::holds_alternative<WorkerReady>(m)) {
      p.send(WorkerStart{});
      started = true;
    }
  };
  p.onExited = [&](int, QProcess::ExitStatus) { exited = true; };
  require(launchPrepared(p, b), "launch ignoring worker");
  require(until([&] { return started; }, 2000), "no ready");
  QElapsedTimer timer;
  timer.start();
  p.stop(StopReason::Revoked);
  require(until([&] { return exited; }, 6000), "ignored stop not terminated");
  require(
      timer.elapsed() >= 4900 && timer.elapsed() < 6000 && p.error() == "WorkerStopDeadline",
      "stop deadline not enforced"
  );
}
static QByteArray boot(const WorkerBinding &b, const QString &endpoint, const QByteArray &secret)
{
  const auto &t = b.token;
  return QJsonDocument(
             QJsonObject{
                 {"protocol", "display-worker-bootstrap/5"},
                 {"endpoint", endpoint},
                 {"parentPid", QString::number(QCoreApplication::applicationPid())},
                 {"secret", QString::fromLatin1(secret.toHex())},
                 {"binding",
                  QJsonObject{
                      {"session", QString::fromStdString(t.session)},
                      {"attempt", "1"},
                      {"generation", "1"},
                      {"epoch", "1"},
                      {"owner", QString::fromStdString(t.owner)},
                      {"worker", QString::fromStdString(t.worker)},
                      {"userSession", b.localUserSession},
                      {"build", b.buildCompatibility},
                      {"backend", b.backend},
                      {"profile", b.profile},
                      {"planHash", b.planHash},
                      {"localIdentity", b.localIdentity},
                      {"peerIdentity", b.peerIdentity},
                      {"controlBinding", b.controlBinding},
                      {"direction", int(b.direction)}
                  }}
             }
  ).toJson(QJsonDocument::Compact);
}
static void legacyBootstrap()
{
  const auto b = binding();
  QProcess process;
  require(prepareWorkerProcess(process, registration(b), {"--display-worker"}), "prepare legacy rejection fixture");
  process.start();
  require(process.waitForStarted(2000), "start legacy fixture");
  auto bytes = boot(b, "/unreachable-display-fixture", QByteArray(32, 'x'));
  bytes.replace("display-worker-bootstrap/5", "display-worker-bootstrap/4");
  require(process.write(bytes) == bytes.size(), "legacy bootstrap write");
  process.closeWriteChannel();
  require(process.waitForFinished(2000), "legacy bootstrap did not fail closed");
  require(
      process.exitStatus() == QProcess::NormalExit && process.exitCode() == 42, "legacy bootstrap accepted by v5 worker"
  );
}
static void parentEof()
{
  QTemporaryDir dir;
  require(dir.isValid(), "temporary directory");
  QLocalServer server;
  server.setSocketOptions(QLocalServer::UserAccessOption);
#ifdef Q_OS_WIN
  const auto endpoint = "bmd-test-" + QString::number(QCoreApplication::applicationPid());
#else
  const auto endpoint = dir.path() + "/ipc";
#endif
  require(server.listen(endpoint), "test listener");
  const auto b = binding();
  const QByteArray secret(32, 'x');
  WorkerProtocol protocol(WorkerSide::Agent, b, secret, Clock::now());
  QProcess process;
  process.setProgram(registration(b).executable);
  process.setArguments({"--display-worker"});
  process.start();
  require(process.waitForStarted(2000), "start EOF fixture");
  process.write(boot(b, endpoint, secret));
  process.closeWriteChannel();
  require(until([&] { return server.hasPendingConnections(); }, 2000), "no worker connection");
  std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
  socket->write(protocol.hello());
  require(
      until(
          [&] {
            if (socket->bytesAvailable())
              protocol.feed(socket->read(8196), true, Clock::now());
            return protocol.established();
          },
          2000
      ),
      "EOF handshake"
  );
  socket->abort();
  require(until([&] { return process.state() == QProcess::NotRunning; }, 2000), "worker ignored parent EOF");
  require(process.exitCode() == 43, "worker did not run self-stop callback");
}
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  if (argc == 2 && QString(argv[1]) == "--display-worker")
    return fixture(app);
  if (argc == 2 && QString(argv[1]) == "--execution-marker")
    return 73;
  try {
    require(argc == 2, "case required");
    const std::string c = argv[1];
    if (c == "WorkerSourceLeaseLifecycle")
      lifecycle(false, true);
    else if (c == "WorkerProcessLifecycle")
      lifecycle();
    else if (c == "WorkerFinalDrain")
      lifecycle(true);
    else if (c == "WorkerExecutableBinding")
      executableBinding();
    else if (c == "WorkerRegistration")
      registrationGates();
    else if (c == "WorkerProcessFailures")
      failures();
    else if (c == "WorkerStopDeadline")
      stopDeadline();
    else if (c == "WorkerLegacyBootstrap")
      legacyBootstrap();
    else if (c == "WorkerParentEof")
      parentEof();
    else
      throw std::runtime_error("unknown case");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
