// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "WorkerProcess.h"
#include "display/control/InfrastructureControl.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QtEndian>
#ifdef Q_OS_UNIX
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef Q_OS_MACOS
#include <sys/un.h>
#endif
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#endif
namespace deskflow::display::infra {
namespace {
void erase(QByteArray &b)
{
  if (!b.isEmpty()) {
    b.detach();
    volatile char *p = b.data();
    for (qsizetype i = 0; i < b.size(); ++i)
      p[i] = 0;
  }
  b.clear();
}
QByteArray randomSecret()
{
  QByteArray b(32, Qt::Uninitialized);
  for (qsizetype i = 0; i < 32; i += 4)
    qToBigEndian(QRandomGenerator::system()->generate(), b.data() + i);
  return b;
}
QJsonObject bindingObject(const WorkerBinding &b)
{
  const auto &t = b.token;
  return {
      {"session", QString::fromStdString(t.session)},
      {"attempt", QString::number(t.attempt)},
      {"generation", QString::number(t.generation)},
      {"epoch", QString::number(t.epoch)},
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
  };
}
std::optional<WorkerBinding> decodeBinding(const QJsonObject &o, WorkerPurpose purpose)
{
  auto num = [&](const char *k) {
    return o.value(k).isString() ? parseCounter(o.value(k).toString().toStdString()) : std::optional<uint64_t>{};
  };
  const auto a = num("attempt"), g = num("generation"), e = num("epoch");
  if (!a || !g || !e || *e > UINT32_MAX)
    return {};
  WorkerBinding b{
      {o.value("session").toString().toStdString(), *a, *g, uint32_t(*e), o.value("owner").toString().toStdString(),
       o.value("worker").toString().toStdString()},
      o.value("userSession").toString(),
      o.value("build").toString(),
      o.value("backend").toString(),
      o.value("profile").toString(),
      o.value("planHash").toString(),
      o.value("localIdentity").toString(),
      o.value("peerIdentity").toString(),
      o.value("controlBinding").toString(),
      Direction(o.value("direction").toInt(-1))
  };
  b.purpose = purpose;
  if (bindingObject(b) != o)
    return {};
  WorkerProtocol check(WorkerSide::Worker, b, QByteArray(32, 'x'), Clock::now());
  if (check.failed())
    return {};
  return b;
}
QByteArray bootstrapBytes(const WorkerBinding &b, const QString &endpoint, const QByteArray &secret)
{
  return QJsonDocument(
             QJsonObject{
                 {"protocol",
                  b.purpose == WorkerPurpose::Platform ? "display-platform-bootstrap/1" : "display-worker-bootstrap/2"},
                 {"binding", bindingObject(b)},
                 {"endpoint", endpoint},
                 {"parentPid", QString::number(QCoreApplication::applicationPid())},
                 {"secret", QString::fromLatin1(secret.toHex())}
             }
  ).toJson(QJsonDocument::Compact);
}
bool peerIdentity(QLocalSocket &socket, uint64_t expectedPid, bool agentSide)
{
#ifdef Q_OS_UNIX
  const auto fd = int(socket.socketDescriptor());
#ifdef Q_OS_MACOS
  uid_t uid;
  gid_t gid;
  pid_t pid = 0;
  socklen_t size = sizeof(pid);
  return getpeereid(fd, &uid, &gid) == 0 && uid == geteuid() &&
         getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &size) == 0 && pid > 0 && uint64_t(pid) == expectedPid;
#else
  struct ucred cred{};
  socklen_t size = sizeof(cred);
  return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) == 0 && cred.uid == geteuid() && cred.pid > 0 &&
         uint64_t(cred.pid) == expectedPid;
#endif
#elif defined(Q_OS_WIN)
  ULONG pid = 0;
  const auto handle = reinterpret_cast<HANDLE>(socket.socketDescriptor());
  const bool ok = agentSide ? GetNamedPipeClientProcessId(handle, &pid) : GetNamedPipeServerProcessId(handle, &pid);
  return ok && uint64_t(pid) == expectedPid; // Pipe DACL is current-user-only (Qt UserAccessOption).
#else
  Q_UNUSED(socket);
  Q_UNUSED(expectedPid);
  Q_UNUSED(agentSide);
  return false;
#endif
  Q_UNUSED(agentSide);
}
bool writeBounded(QLocalSocket *socket, const QByteArray &bytes)
{
  return socket && socket->state() == QLocalSocket::ConnectedState && !bytes.isEmpty() &&
         socket->bytesToWrite() + bytes.size() <= 4 * (WorkerProtocol::MaxMessageBytes + 4) &&
         socket->write(bytes) == bytes.size();
}
bool validExecutable(const WorkerRegistration &r)
{
  QFileInfo f(r.executable);
  if (!f.isAbsolute() || !f.isFile() || f.isSymLink() || f.canonicalFilePath() != r.executable || !f.isExecutable() ||
      f.size() <= 0 || f.size() > 128 * 1024 * 1024 || !validId(r.sha256.toStdString(), 32))
    return false;
#ifdef Q_OS_UNIX
  struct stat st{};
  if (stat(QFile::encodeName(r.executable).constData(), &st) != 0 || (st.st_uid != geteuid() && st.st_uid != 0) ||
      (st.st_mode & (S_IWGRP | S_IWOTH)))
    return false;
#endif
  QFile file(r.executable);
  if (!file.open(QIODevice::ReadOnly))
    return false;
  QCryptographicHash hash(QCryptographicHash::Sha256);
  return hash.addData(&file) && QString::fromLatin1(hash.result().toHex()) == r.sha256;
}
} // namespace
std::optional<WorkerBootstrap> readWorkerBootstrap(QString &error)
{
  QFile input;
  if (!input.open(stdin, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) {
    error = "BootstrapInput";
    return {};
  }
  auto bytes = input.read(WorkerProtocol::MaxBootstrapBytes + 1);
  ControlError e;
  auto o = parseBoundedObject(bytes, WorkerProtocol::MaxBootstrapBytes, e);
  if (!o || o->size() != 5 ||
      (o->value("protocol") != "display-worker-bootstrap/2" && o->value("protocol") != "display-platform-bootstrap/1"
      ) ||
      !o->value("binding").isObject() || !o->value("endpoint").isString() || !o->value("parentPid").isString() ||
      !o->value("secret").isString()) {
    erase(bytes);
    error = "BootstrapSchema";
    return {};
  }
  const auto b = decodeBinding(
      o->value("binding").toObject(),
      o->value("protocol") == "display-platform-bootstrap/1" ? WorkerPurpose::Platform : WorkerPurpose::Media
  );
  const auto pid = parseCounter(o->value("parentPid").toString().toStdString());
  const auto secretText = o->value("secret").toString().toLatin1();
  const auto endpoint = o->value("endpoint").toString();
  if (!b || !pid || *pid > UINT32_MAX || !validId(secretText.toStdString(), 32) || endpoint.isEmpty() ||
      endpoint.size() > 256 || endpoint.contains(QChar(0))) {
    erase(bytes);
    error = "BootstrapBinding";
    return {};
  }
#ifdef Q_OS_UNIX
  if (uint64_t(getppid()) != *pid) {
    erase(bytes);
    error = "BootstrapParent";
    return {};
  }
#endif
  WorkerBootstrap result{*b, endpoint, *pid, QByteArray::fromHex(secretText)};
  erase(bytes);
  return result;
}
WorkerProcess::WorkerProcess(QObject *parent) : QObject(parent), m_directory(QDir::tempPath() + "/bmd-worker-XXXXXX")
{
  m_server.setMaxPendingConnections(1);
  m_server.setSocketOptions(QLocalServer::UserAccessOption);
  connect(&m_server, &QLocalServer::newConnection, this, [this] { accept(); });
  connect(&m_process, &QProcess::started, this, [this] {
#ifdef Q_OS_WIN
    const auto handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(m_process.processId()));
    const bool assigned = handle && AssignProcessToJobObject(static_cast<HANDLE>(m_job), handle);
    if (handle)
      CloseHandle(handle);
    if (!assigned) {
      fail("WorkerJobAssignment");
      return;
    }
#endif
    if (m_process.write(m_bootstrap) != m_bootstrap.size()) {
      fail("WorkerBootstrapWrite");
      return;
    }
    erase(m_bootstrap);
    m_process.closeWriteChannel();
  });
  connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
    if (e == QProcess::FailedToStart) {
      fail("WorkerLaunchFailed");
      if (!m_exitNotified) {
        m_exitNotified = true;
        if (onExited)
          onExited(-1, QProcess::CrashExit);
      }
    }
  });
  connect(
      &m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
      [this](int code, QProcess::ExitStatus status) {
        m_timer.stop();
        m_server.close();
        // Process exit may be delivered before the local socket's readyRead.
        // Consume the bounded receive buffer before aborting it, including the
        // final authenticated Quiesced observation.
        if (m_socket && m_socket->state() == QLocalSocket::ConnectedState)
          m_socket->waitForReadyRead(0); // Poll kernel bytes when its notifier is still pending.
        for (int chunk = 0; chunk < 4 && m_socket && m_socket->bytesAvailable() > 0 && m_error.isEmpty(); ++chunk)
          read();
        if (m_socket)
          m_socket->abort();
        erase(m_bootstrap);
        if (m_error.isEmpty() && (status == QProcess::CrashExit || code != 0))
          m_error = "WorkerAbnormalExit";
        else if (!m_expectedExit && m_error.isEmpty())
          m_error = "WorkerUnexpectedExit";
        if (!m_exitNotified) {
          m_exitNotified = true;
          if (onExited)
            onExited(code, status);
        }
      }
  );
  m_process.setStandardErrorFile(QProcess::nullDevice());
  m_process.setStandardOutputFile(QProcess::nullDevice());
  m_timer.setInterval(250);
  connect(&m_timer, &QTimer::timeout, this, [this] { poll(); });
#ifdef Q_OS_UNIX
  m_process.setChildProcessModifier([] {
    if (setpgid(0, 0) != 0)
      _exit(126);
  });
#endif
}
WorkerProcess::~WorkerProcess()
{
  onExited = {};
  onMessage = {};
  onEstablished = {};
  m_timer.stop();
  m_expectedExit = true;
  m_server.close();
  if (m_socket)
    m_socket->abort();
  terminateOwnedProcess();
  if (running())
    m_process.waitForFinished(1000);
  erase(m_bootstrap);
#ifdef Q_OS_WIN
  if (m_job)
    CloseHandle(static_cast<HANDLE>(m_job));
#endif
}
bool WorkerProcess::launch(const WorkerRegistration &r, const WorkerBinding &b)
{
  if (m_launched || !m_error.isEmpty())
    return false;
  m_launched = true;
  if (r.purpose != b.purpose || r.backend != b.backend || r.profile != b.profile || !validExecutable(r) ||
      !m_directory.isValid()) {
    fail("WorkerRegistration");
    return false;
  }
#ifdef Q_OS_WIN
  m_job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
  info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!m_job ||
      !SetInformationJobObject(static_cast<HANDLE>(m_job), JobObjectExtendedLimitInformation, &info, sizeof(info))) {
    fail("WorkerJobCreation");
    return false;
  }
  const auto endpoint = "bmd-worker-" + QString::fromLatin1(randomSecret().toHex());
#else
  const auto endpoint = m_directory.path() + "/ipc";
  if (!QFile::setPermissions(
          m_directory.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
      )) {
    fail("WorkerPrivateDirectory");
    return false;
  }
#endif
  auto secret = randomSecret();
  m_protocol = std::make_unique<WorkerProtocol>(WorkerSide::Agent, b, secret, Clock::now());
  if (m_protocol->failed()) {
    erase(secret);
    fail("WorkerBinding");
    return false;
  }
  m_bootstrap = bootstrapBytes(b, endpoint, secret);
  erase(secret);
  if (!m_server.listen(endpoint)) {
    fail("WorkerEndpoint");
    return false;
  }
  m_process.setProgram(r.executable);
  m_process.setArguments({"--display-worker"});
  // Never inherit dynamic loader or GStreamer plugin overrides from a caller.
  auto env = QProcessEnvironment::systemEnvironment();
  for (const auto &k : env.keys())
    if (k.startsWith("DYLD_") || k.startsWith("LD_") || k.startsWith("GST_"))
      env.remove(k);
  m_process.setProcessEnvironment(env);
  m_timer.start();
  m_process.start();
  return true;
}
void WorkerProcess::accept()
{
  auto *socket = m_server.nextPendingConnection();
  if (!socket)
    return;
  if (m_socket || !m_protocol || !running() || !peerIdentity(*socket, uint64_t(m_process.processId()), true)) {
    socket->abort();
    socket->deleteLater();
    return;
  }
  m_socket = socket;
  socket->setParent(this);
  socket->setReadBufferSize(2 * (WorkerProtocol::MaxMessageBytes + 4));
  m_server.close();
  connect(socket, &QLocalSocket::readyRead, this, [this] { read(); });
  connect(socket, &QLocalSocket::disconnected, this, [this] {
    for (int chunk = 0; chunk < 4 && m_socket && m_socket->bytesAvailable() > 0 && m_error.isEmpty(); ++chunk)
      read();
    if (!m_expectedExit)
      fail("WorkerIpcEof");
  });
  if (!writeBounded(socket, m_protocol->hello())) {
    fail("WorkerHelloWrite");
    return;
  }
  read();
}
void WorkerProcess::read()
{
  if (!m_protocol || !m_socket)
    return;
  if (!m_protocol->feed(m_socket->read(WorkerProtocol::MaxMessageBytes + 4), true, Clock::now())) {
    fail(m_protocol->error());
    return;
  }
  if (m_protocol->established() && !m_established) {
    m_established = true;
    if (onEstablished)
      onEstablished();
  }
  for (const auto &m : m_protocol->takeMessages()) {
    if (std::holds_alternative<WorkerQuiesced>(m)) {
      m_expectedExit = true;
      if (m_stopDeadline == TimePoint{})
        m_stopDeadline = Clock::now() + std::chrono::seconds(1);
    }
    if (!std::holds_alternative<WorkerHeartbeat>(m) && onMessage)
      onMessage(m);
  }
  if (m_socket->bytesAvailable() > 0)
    QTimer::singleShot(0, this, [this] { read(); });
}
bool WorkerProcess::send(const WorkerMessage &m)
{
  if (!m_protocol || !m_error.isEmpty())
    return false;
  if (!writeBounded(m_socket, m_protocol->encode(m, Clock::now()))) {
    fail("WorkerIpcWrite");
    return false;
  }
  return true;
}
void WorkerProcess::stop(StopReason reason)
{
  if (!running())
    return;
  if (m_stopDeadline == TimePoint{})
    m_stopDeadline = Clock::now() + std::chrono::seconds(5);
  if (m_established)
    send(WorkerStop{reason});
  else
    fail("WorkerStopBeforeHandshake");
}
void WorkerProcess::terminate()
{
  fail("WorkerTerminatedBySupervisor");
}
void WorkerProcess::poll()
{
  if (!m_protocol)
    return;
  m_protocol->tick(Clock::now());
  if (m_protocol->failed()) {
    fail(m_protocol->error());
    return;
  }
  if (m_stopDeadline != TimePoint{} && Clock::now() >= m_stopDeadline) {
    fail("WorkerStopDeadline");
    return;
  }
  if (m_established && !m_expectedExit)
    send(WorkerHeartbeat{});
}
void WorkerProcess::fail(const QString &code)
{
  if (m_error.isEmpty())
    m_error = code;
  m_timer.stop();
  m_server.close();
  erase(m_bootstrap);
  if (m_socket)
    m_socket->abort();
  terminateOwnedProcess();
}
void WorkerProcess::terminateOwnedProcess()
{
  if (!running())
    return;
#ifdef Q_OS_UNIX
  const auto pid = m_process.processId();
  if (pid > 0)
    ::kill(-pid, SIGKILL);
#elif defined(Q_OS_WIN)
  if (m_job)
    TerminateJobObject(static_cast<HANDLE>(m_job), 1);
#endif
  m_process.kill();
}
WorkerConnection::WorkerConnection(WorkerBootstrap bootstrap, QObject *parent)
    : QObject(parent),
      m_protocol(
          std::make_unique<WorkerProtocol>(
              WorkerSide::Worker, std::move(bootstrap.binding), bootstrap.secret, Clock::now()
          )
      ),
      m_endpoint(std::move(bootstrap.endpoint)),
      m_parentPid(bootstrap.parentPid)
{
  erase(bootstrap.secret);
  m_socket.setParent(this);
  m_timer.setParent(this);
  m_socket.setReadBufferSize(2 * (WorkerProtocol::MaxMessageBytes + 4));
  connect(&m_socket, &QLocalSocket::connected, this, [this] {
    if (!peerIdentity(m_socket, m_parentPid, false)) {
      fail("WorkerAgentIdentity");
      return;
    }
    m_connected = true;
    if (!writeBounded(&m_socket, m_protocol->hello())) {
      fail("WorkerHelloWrite");
      return;
    }
    read();
  });
  connect(&m_socket, &QLocalSocket::readyRead, this, [this] { read(); });
  connect(&m_socket, &QLocalSocket::disconnected, this, [this] { fail("WorkerParentEof"); });
  connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
    fail("WorkerSocketError");
  });
  m_timer.setInterval(250);
  connect(&m_timer, &QTimer::timeout, this, [this] { poll(); });
}
WorkerConnection::~WorkerConnection()
{
  onShutdown = {};
  m_timer.stop();
  m_socket.abort();
}
void WorkerConnection::connectToAgent()
{
  if (m_connected || m_shuttingDown)
    return;
  m_timer.start();
  m_socket.connectToServer(m_endpoint);
}
void WorkerConnection::disconnectAfterWrites()
{
  m_socket.disconnectFromServer();
}
void WorkerConnection::read()
{
  if (!m_connected || m_shuttingDown)
    return;
  if (!m_protocol->feed(m_socket.read(WorkerProtocol::MaxMessageBytes + 4), true, Clock::now())) {
    fail(m_protocol->error());
    return;
  }
  if (m_protocol->established() && !m_established) {
    m_established = true;
    if (onEstablished)
      onEstablished();
  }
  for (const auto &m : m_protocol->takeMessages())
    if (!std::holds_alternative<WorkerHeartbeat>(m) && onMessage)
      onMessage(m);
  if (m_socket.bytesAvailable() > 0)
    QTimer::singleShot(0, this, [this] { read(); });
}
bool WorkerConnection::send(const WorkerMessage &m)
{
  if (m_shuttingDown)
    return false;
  if (!writeBounded(&m_socket, m_protocol->encode(m, Clock::now()))) {
    fail("WorkerIpcWrite");
    return false;
  }
  if (std::holds_alternative<WorkerQuiesced>(m))
    m_timer.stop();
  return true;
}
void WorkerConnection::poll()
{
  m_protocol->tick(Clock::now());
  if (m_protocol->failed()) {
    fail(m_protocol->error());
    return;
  }
  if (m_established)
    send(WorkerHeartbeat{});
}
void WorkerConnection::fail(const QString &code)
{
  if (m_shuttingDown)
    return;
  m_shuttingDown = true;
  m_error = code;
  m_timer.stop();
  m_protocol->disconnected();
  m_socket.abort();
  if (onShutdown)
    onShutdown();
}
} // namespace deskflow::display::infra
