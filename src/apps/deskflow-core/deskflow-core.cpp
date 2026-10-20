/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016, 2025 - 2026 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreArgParser.h"

#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "common/Constants.h"
#include "common/ExitCodes.h"
#include "deskflow/ClientApp.h"
#include "deskflow/ServerApp.h"
#include "deskflow/ipc/CoreIpcServer.h"
#include "server/Config.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <sstream>

#if defined(Q_OS_WIN)
#include "arch/win32/ArchMiscWindows.h"
#endif

#include <QApplication>
#include <QFileInfo>
#include <QSharedMemory>
#include <QTextStream>
#include <QThread>

void qtMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
  const auto utf8 = message.toUtf8();
  switch (type) {
  case QtDebugMsg:
    CLOG->print(context.file, context.line, CLOG_TAG_DEBUG "%s", utf8.constData());
    break;
  case QtInfoMsg:
    CLOG->print(context.file, context.line, CLOG_TAG_INFO "%s", utf8.constData());
    break;
  case QtWarningMsg:
    CLOG->print(context.file, context.line, CLOG_TAG_WARN "%s", utf8.constData());
    break;
  case QtCriticalMsg:
    CLOG->print(context.file, context.line, CLOG_TAG_ERR "%s", utf8.constData());
    break;
  case QtFatalMsg:
    CLOG->print(context.file, context.line, CLOG_TAG_CRIT "%s", utf8.constData());
    break;
  }
  if (type == QtFatalMsg) {
    abort();
  }
}

void showHelp(const CoreArgParser &parser)
{
  QTextStream(stdout) << parser.helpText();
}

App *createApp(const CoreArgParser &parser, EventQueue &events, const QString &processName)
{
  if (parser.serverMode()) {
    return new ServerApp(&events, processName);
  } else if (parser.clientMode()) {
    return new ClientApp(&events, processName);
  }
  return nullptr;
}

int main(int argc, char **argv)
{
#if defined(Q_OS_WIN)
  ArchMiscWindows::setInstanceWin32(GetModuleHandle(nullptr));
#endif

  QCoreApplication::setApplicationName(QStringLiteral("%1 Core").arg(kAppName));
  // Validation and CLI information must work without a display server, input
  // permission or platform plugin. Normal sharing still uses QApplication.
  QStringList arguments;
  for (int i = 0; i < argc; ++i)
    arguments.append(QString::fromLocal8Bit(argv[i]));
  const bool commandLineOnly = arguments.contains("--validate-config") || arguments.contains("--version") ||
                               arguments.contains("--help") || arguments.contains("-h") || arguments.contains("-v");
  std::unique_ptr<QCoreApplication> app;
  if (commandLineOnly)
    app = std::make_unique<QCoreApplication>(argc, argv);
  else
    app = std::make_unique<QApplication>(argc, argv);

  Arch arch;
  arch.init();

  Log log;
  qInstallMessageHandler(qtMessageHandler);

  CoreArgParser parser(QCoreApplication::arguments());

  // Print any parser errors
  if (!parser.errorText().isEmpty()) {
    QTextStream(stdout) << parser.errorText() << "\n";
  }

  if (parser.help()) {
    showHelp(parser);
    return s_exitSuccess;
  }

  if (parser.version()) {
    QTextStream(stdout) << parser.versionText();
    return s_exitSuccess;
  }

  if (parser.validateOnly()) {
    parser.parse();
    try {
      EventQueue events;
      if (parser.serverMode()) {
        QFile file(Settings::serverConfigFile());
        if (!file.open(QIODevice::ReadOnly))
          throw std::runtime_error("Cannot read server configuration");
        std::istringstream stream(file.readAll().toStdString());
        deskflow::server::Config config(&events);
        deskflow::server::ConfigReadContext context(stream);
        config.read(context);
        if (!config.isComputer(Settings::value(Settings::Core::ComputerName).toString().toStdString()))
          throw std::runtime_error("Local computer is absent from configuration");
      } else {
        ClientApp candidate(&events);
        candidate.parseArgs();
      }
      QTextStream(stdout) << QJsonDocument(QJsonObject{{"success", true}}).toJson(QJsonDocument::Compact) << "\n";
      return s_exitSuccess;
    } catch (const std::exception &error) {
      QTextStream(stdout) << QJsonDocument(
                                 QJsonObject{{"success", false}, {"reason", QString::fromUtf8(error.what())}}
                             ).toJson(QJsonDocument::Compact)
                          << "\n";
      return s_exitFailed;
    }
  }

  // Before we check any more args we need to check for a duplicate process.
  // Create a shared memory segment with a unique key
  // This is to prevent a new instance from running if one is already running
  QSharedMemory sharedMemory(kCoreBinName);

  // Attempt to attach first and detach in order to clean up stale shm chunks
  // This can happen if the previous instance was killed or crashed
  if (sharedMemory.attach())
    sharedMemory.detach();

  if (!sharedMemory.create(1) && parser.singleInstanceOnly()) {
    LOG_WARN("an instance of deskflow core is already running");
    return s_exitDuplicate;
  }

  parser.parse();

  EventQueue events;
  const auto processName = QFileInfo(argv[0]).fileName();

  App *coreApp = createApp(parser, events, processName);

  const auto ipcServer = new deskflow::core::ipc::CoreIpcServer(app.get()); // NOSONAR - Qt managed
  QObject::connect(
      ipcServer, &deskflow::core::ipc::IpcServer::stopProcessRequested, coreApp, &App::quit, Qt::DirectConnection
  );
  ipcServer->listen();

  QThread coreThread;
  QObject::connect(&coreThread, &QThread::finished, app.get(), &QCoreApplication::quit);
  coreApp->run(coreThread);

  int exitCode = QCoreApplication::exec();
  coreThread.wait();

  if (exitCode == s_exitSuccess) {
    exitCode = coreApp->getExitCode();
  }
  delete coreApp;

  LOG_DEBUG("core exited, code: %d", exitCode);
  return exitCode;
}
