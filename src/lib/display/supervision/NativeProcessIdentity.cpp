// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "NativeProcessIdentity.h"
#include <QFile>
#include <QStringList>
#include <cerrno>
#include <climits>
#ifdef Q_OS_MACOS
#include <libproc.h>
#elif defined(Q_OS_LINUX)
#include <sys/stat.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#endif
namespace deskflow::display::infra {
namespace {
struct Observation
{
  std::optional<NativeProcessIdentity> identity;
  bool absent = false;
};
QString currentPlatform()
{
#ifdef Q_OS_MACOS
  return "mac";
#elif defined(Q_OS_LINUX)
  return "linux";
#elif defined(Q_OS_WIN)
  return "win";
#else
  return {};
#endif
}
Observation observe(quint64 pid)
{
  if (!pid || pid > INT_MAX)
    return {};
#ifdef Q_OS_MACOS
  proc_bsdinfo info{};
  errno = 0;
  const auto n = proc_pidinfo(int(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info));
  if (n != sizeof(info))
    return {{}, n == 0 && errno == ESRCH};
  if (info.pbi_pid != pid || !info.pbi_start_tvsec || info.pbi_start_tvusec >= 1000000)
    return {};
  return {NativeProcessIdentity{"mac", pid, info.pbi_start_tvsec, info.pbi_start_tvusec}, false};
#elif defined(Q_OS_LINUX)
  const auto path = QByteArray("/proc/") + QByteArray::number(pid) + "/stat";
  QFile f(QString::fromLatin1(path));
  if (!f.open(QIODevice::ReadOnly)) {
    struct stat st{};
    errno = 0;
    const auto status = ::stat(path.constData(), &st);
    const bool missing = status != 0 && errno == ENOENT;
    // ENOENT also occurs when procfs is unavailable. Prove that this process
    // can still inspect its own proc entry before inferring another is absent.
    QFile self(QStringLiteral("/proc/self/stat"));
    return {{}, missing && self.open(QIODevice::ReadOnly)};
  }
  const auto bytes = f.read(4097);
  const auto close = bytes.lastIndexOf(')');
  if (bytes.size() > 4096 || close < 0 || !bytes.startsWith(QByteArray::number(pid) + " ("))
    return {};
  // comm can contain spaces and parentheses. Field 3 follows the LAST ')';
  // starttime is field 22, i.e. index 19 in this suffix.
  const auto fields = bytes.mid(close + 1).simplified().split(' ');
  bool ok = false;
  const auto birth = fields.size() > 19 ? fields[19].toULongLong(&ok) : 0;
  if (!ok || !birth)
    return {};
  return {NativeProcessIdentity{"linux", pid, birth, 0}, false};
#elif defined(Q_OS_WIN)
  const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, DWORD(pid));
  if (!handle)
    return {{}, GetLastError() == ERROR_INVALID_PARAMETER};
  FILETIME creation{}, exit{}, kernel{}, user{};
  const bool success = GetProcessTimes(handle, &creation, &exit, &kernel, &user);
  const auto state = WaitForSingleObject(handle, 0);
  CloseHandle(handle);
  if (state == WAIT_OBJECT_0)
    return {{}, true};
  const auto birth = (quint64(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
  if (!success || state != WAIT_TIMEOUT || !birth)
    return {};
  return {NativeProcessIdentity{"win", pid, birth, 0}, false};
#else
  return {};
#endif
}
} // namespace
QString NativeProcessIdentity::encode() const
{
  if (platform != currentPlatform() || !pid || pid > INT_MAX || !birth ||
      (platform == "mac" ? fraction >= 1000000 : fraction != 0))
    return {};
  return QString("process1:%1:%2:%3:%4").arg(platform).arg(pid).arg(birth).arg(fraction);
}
std::optional<NativeProcessIdentity> NativeProcessIdentity::decode(const QString &s)
{
  if (s.size() > 128)
    return {};
  const auto parts = s.split(':');
  if (parts.size() != 5 || parts[0] != "process1")
    return {};
  bool p = false, b = false, f = false;
  NativeProcessIdentity id{parts[1], parts[2].toULongLong(&p), parts[3].toULongLong(&b), parts[4].toULongLong(&f)};
  if (!p || !b || !f || id.encode() != s)
    return {};
  return id;
}
std::optional<NativeProcessIdentity> nativeProcessIdentity(quint64 pid)
{
  return observe(pid).identity;
}
NativeProcessPresence nativeProcessPresence(const NativeProcessIdentity &id)
{
  if (id.encode().isEmpty())
    return NativeProcessPresence::Unknown;
  const auto seen = observe(id.pid);
  if (seen.absent)
    return NativeProcessPresence::OriginalAbsent;
  if (!seen.identity)
    return NativeProcessPresence::Unknown;
  return *seen.identity == id ? NativeProcessPresence::Exact : NativeProcessPresence::OriginalAbsent;
}
} // namespace deskflow::display::infra
