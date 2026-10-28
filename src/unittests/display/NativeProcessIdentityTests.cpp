// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "display/supervision/NativeProcessIdentity.h"
#include <QCoreApplication>
#include <QProcess>
#include <QTimer>
#include <iostream>
#include <stdexcept>
using namespace deskflow::display::infra;
static void require(bool b, const char *why)
{
  if (!b)
    throw std::runtime_error(why);
}
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  if (argc == 2 && QString(argv[1]) == "--child") {
    QTimer::singleShot(10000, &app, &QCoreApplication::quit);
    return app.exec();
  }
  try {
    require(argc == 2, "case required");
    if (QString(argv[1]) == "ProcessBirthIdentity") {
      const auto id = nativeProcessIdentity(QCoreApplication::applicationPid());
      require(bool(id), "cannot read current native birth identity");
      require(NativeProcessIdentity::decode(id->encode()) == id, "canonical identity roundtrip");
      require(nativeProcessPresence(*id) == NativeProcessPresence::Exact, "current process not exact");
      auto replaced = *id;
      ++replaced.birth;
      require(
          nativeProcessPresence(replaced) == NativeProcessPresence::OriginalAbsent,
          "same PID with different birth accepted"
      );
      require(!NativeProcessIdentity::decode(id->encode() + ":extra"), "extra fields accepted");
      require(!NativeProcessIdentity::decode("process1:mac:01:1:0"), "noncanonical PID accepted");
      require(nativeProcessPresence({}) == NativeProcessPresence::Unknown, "invalid identity proves absence");
      require(!nativeProcessIdentity(0) && !nativeProcessIdentity(UINT64_MAX), "invalid PID inspected");
    } else if (QString(argv[1]) == "ProcessBirthExit") {
      QProcess child;
      child.start(QCoreApplication::applicationFilePath(), {"--child"});
      require(child.waitForStarted(2000), "child launch");
      const auto id = nativeProcessIdentity(child.processId());
      require(bool(id) && nativeProcessPresence(*id) == NativeProcessPresence::Exact, "child birth unavailable");
      child.kill(); // Live owned QProcess; never kill by the persisted identity.
      require(child.waitForFinished(2000), "child exit");
      require(nativeProcessPresence(*id) == NativeProcessPresence::OriginalAbsent, "reaped child still present");
    } else
      throw std::runtime_error("unknown case");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
