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

#if defined(Q_OS_WIN)
#include "arch/win32/ArchMiscWindows.h"
#endif

#ifdef DESKFLOW_BLUETOOTH
#include "BluetoothDiscovery.h"
#include "ConnectionManager.h"
#include "RfcommBridge.h"
#include "common/PeerPolicy.h"
#include "net/FingerprintDatabase.h"
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QSettings>
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

  QApplication::setApplicationName(QStringLiteral("%1 Core").arg(kAppName));
  QApplication app(argc, argv);

  Arch arch;
  arch.init();

  Log log;
  qInstallMessageHandler(qtMessageHandler);

  CoreArgParser parser(QCoreApplication::arguments());

  // Print any parser errors
  if (!parser.errorText().isEmpty()) {
    QTextStream(stdout) << parser.errorText() << "\n";
    return s_exitArgs;
  }

  if (parser.help()) {
    showHelp(parser);
    return s_exitSuccess;
  }

  if (parser.version()) {
    QTextStream(stdout) << parser.versionText();
    return s_exitSuccess;
  }

  const bool bluetoothAction = parser.bluetoothScan() || !parser.bluetoothServices().isEmpty();
  if (bluetoothAction || parser.bluetoothRequested() || parser.autoFailover()) {
#ifndef DESKFLOW_BLUETOOTH
    LOG_CRIT("Bluetooth support is not built; configure with -DBUILD_BLUETOOTH=ON");
    return s_exitConfig;
#else
    const int choices =
        int(parser.bluetoothScan()) + int(!parser.bluetoothServices().isEmpty()) + int(parser.bluetoothRequested());
    const auto addressText = parser.bluetoothRequested() ? parser.bluetoothPeer() : parser.bluetoothServices();
    if ((choices != 1 && !(parser.autoFailover() && choices == 0)) || (bluetoothAction && parser.autoFailover()) ||
        (parser.bluetoothRequested() && QBluetoothAddress(addressText).isNull()) ||
        (!parser.bluetoothServices().isEmpty() && QBluetoothAddress(addressText).isNull())) {
      LOG_CRIT("Specify exactly one Bluetooth action and a valid peer address");
      return s_exitArgs;
    }
    if (bluetoothAction) {
      BluetoothDiscovery discovery;
      bool discoveryFailed = false;
      QObject::connect(&discovery, &BluetoothDiscovery::deviceFound, &app, [](const auto &address, const auto &name) {
        qInfo().noquote() << "bluetooth_device=" << address << "name=" << name;
      });
      QObject::connect(
          &discovery, &BluetoothDiscovery::serviceFound, &app,
          [](const auto &address, const auto &name, const auto &uuid, int channel) {
            qInfo().noquote() << "bluetooth_service=" << address << name << uuid << "channel=" << channel;
          }
      );
      QObject::connect(&discovery, &BluetoothDiscovery::error, &app, [&](const auto &error) {
        discoveryFailed = true;
        qCritical().noquote() << error;
      });
      QObject::connect(&discovery, &BluetoothDiscovery::finished, &app, [&] {
        app.exit(discoveryFailed ? s_exitFailed : s_exitSuccess);
      });
      requestBluetoothPermission(&app, false, [&](bool granted) {
        if (!granted) {
          LOG_CRIT("Bluetooth permission denied");
          app.exit(s_exitFailed);
        } else if (parser.bluetoothScan()) {
          discovery.scan();
        } else {
          discovery.discoverServices(QBluetoothAddress(parser.bluetoothServices()));
        }
      });
      return app.exec();
    }
#endif
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
    LOG_WARN("an instance of busymouse core is already running");
    return s_exitDuplicate;
  }

  parser.parse();
  bool automatic = parser.autoFailover() || Settings::value(Settings::Connection::AutoBackup).toBool();
  const bool authenticated = Settings::value(Settings::Security::TlsEnabled).toBool() &&
                             Settings::value(Settings::Security::CheckPeers).toBool();
  const bool sharingBluetooth =
#ifdef DESKFLOW_BLUETOOTH
      parser.serverMode() && Settings::value(Settings::Connection::ShareBluetooth).toBool();
#else
      false;
#endif
  if ((parser.autoFailover() || parser.bluetoothRequested() || sharingBluetooth) && !authenticated) {
    LOG_CRIT("Bluetooth transport requires TLS and peer fingerprint verification");
    return s_exitConfig;
  }
#ifndef DESKFLOW_BLUETOOTH
  if (parser.autoFailover() || parser.bluetoothRequested()) {
    LOG_CRIT("Bluetooth support is not built");
    return s_exitConfig;
  }
#endif
#ifdef DESKFLOW_BLUETOOTH
  const int bluetoothPort = Settings::value(Settings::Core::Port).toInt();
  if (bluetoothPort < 1 || bluetoothPort > 65535)
    return s_exitConfig;
#endif

  EventQueue events;
  const auto processName = QFileInfo(argv[0]).fileName();

  App *coreApp = createApp(parser, events, processName);

  QThread coreThread;
  bool coreStarted = false;
  const auto ipcServer = new deskflow::core::ipc::CoreIpcServer(&app); // NOSONAR - Qt managed
  QObject::connect(ipcServer, &deskflow::core::ipc::IpcServer::stopProcessRequested, &app, [&] {
    if (coreStarted)
      coreApp->quit();
    else
      app.quit();
  });
  ipcServer->listen();
  QObject::connect(ipcServer, &deskflow::core::ipc::IpcServer::inputControlCommand, &app, [&](const QString& json) {
    if (auto* serverApp=dynamic_cast<ServerApp*>(coreApp))
      serverApp->queueInputControlCommand(json);
    else
      ipcServer->broadcastCommand("inputControlResult", "{\"accepted\":false,\"reasonCode\":\"UnsupportedPlatform\"}");
  });

  QObject::connect(&coreThread, &QThread::finished, &app, &QApplication::quit);
#ifdef DESKFLOW_BLUETOOTH
  std::unique_ptr<ConnectionManager> autoTransport;
  if (authenticated) {
    ConnectionManager::Options options;
    options.server = parser.serverMode();
    options.automatic = automatic;
    if (options.server) {
      options.serverTransports = (Settings::value(Settings::Connection::ShareNetwork).toBool() ? 1 : 0) |
                                 (Settings::value(Settings::Connection::ShareBluetooth).toBool() ? 2 : 0);
      options.advertiseBluetooth = options.serverTransports & 2;
      options.automatic = options.serverTransports == 3;
    }
    options.host = Settings::value(Settings::Client::RemoteHost).toString().trimmed();
    options.initialLane =
        parser.bluetoothRequested() || Settings::value(Settings::Connection::InitialTransport) == "bluetooth" ? 1 : 0;
    if (options.server)
      options.initialLane = options.serverTransports & 1 ? 0 : 1;
    options.bluetoothAddress = parser.bluetoothRequested()
                                   ? parser.bluetoothPeer()
                                   : Settings::value(Settings::Connection::BluetoothPeer).toString();
    options.backupPeer = Settings::value(Settings::Connection::BackupTarget).toString();
    options.pem = Settings::value(Settings::Security::Certificate).toString();
    options.name = Settings::value(Settings::Core::ComputerName).toString();
    options.port = quint16(bluetoothPort);
    const auto bind = Settings::value(Settings::Core::Interface).toString();
    if (options.server && !bind.isEmpty() && !options.bind.setAddress(bind)) {
      delete coreApp;
      return s_exitConfig;
    }
    options.trusted = [server = options.server](const QByteArray &pin) {
      FingerprintDatabase db;
      db.read(server ? Settings::tlsTrustedClientsDb() : Settings::tlsTrustedServersDb());
      return db.isTrusted({QCryptographicHash::Sha256, pin});
    };
    if (!options.server) {
      const auto cached = Settings::value(Settings::Connection::Peers).toMap();
      QStringList matches;
      for (auto i = cached.begin(); i != cached.end(); ++i) {
        const auto record = i.value().toMap();
        if ((options.initialLane == 0 && !options.host.isEmpty() &&
             record.value("networkTarget").toString() == options.host) ||
            (options.initialLane == 1 && !options.bluetoothAddress.isEmpty() &&
             record.value("bluetooth").toString() == options.bluetoothAddress))
          matches.append(i.key());
      }
      if (matches.size() == 1)
        options.initialPeer = matches.first();
    }
    options.loadPeer = [](const QString &id) {
      return QJsonObject::fromVariantMap(Settings::value(Settings::Connection::Peers).toMap().value(id).toMap());
    };
    options.savePeer = [](const QString &id, const QJsonObject &record) {
      auto peers = Settings::value(Settings::Connection::Peers).toMap();
      if (record.value("revoked").toBool()) {
        peers.remove(id);
        Settings::setValue(Settings::Connection::Peers, peers);
        return;
      }
      auto previous = peers.value(id).toMap();
      const auto update = record.toVariantMap();
      for (auto i = update.begin(); i != update.end(); ++i)
        previous.insert(i.key(), i.value());
      peers.insert(id, previous);
      Settings::setValue(Settings::Connection::Peers, peers);
    };
    autoTransport = std::make_unique<ConnectionManager>(options);
    auto *trustWatcher = new QFileSystemWatcher(&app);
    const auto trustPath = options.server ? Settings::tlsTrustedClientsDb() : Settings::tlsTrustedServersDb();
    trustWatcher->addPath(QFileInfo(trustPath).absolutePath());
    QObject::connect(trustWatcher, &QFileSystemWatcher::directoryChanged, &app, [&] { autoTransport->trustChanged(); });
    if (QFileInfo::exists(trustPath))
      trustWatcher->addPath(trustPath);
    QObject::connect(trustWatcher, &QFileSystemWatcher::fileChanged, &app, [&, trustWatcher, trustPath] {
      QTimer::singleShot(50, &app, [&, trustWatcher, trustPath] {
        if (QFileInfo::exists(trustPath) && !trustWatcher->files().contains(trustPath))
          trustWatcher->addPath(trustPath);
        autoTransport->trustChanged();
      });
    });
    QObject::connect(autoTransport.get(), &ConnectionManager::started, &app, [&](quint16 port) {
      if (!coreStarted) {
        coreStarted = true;
        coreApp->setLocalTransportPort(port);
        coreApp->run(coreThread);
      }
    });
    QObject::connect(autoTransport.get(), &ConnectionManager::fatalError, &app, [&](const QString &error) {
      qCritical().noquote() << error;
      app.exit(s_exitConfig);
    });
    QObject::connect(autoTransport.get(), &ConnectionManager::changed, &app, [&](const QJsonObject &snapshot) {
      ipcServer->broadcastCommand(
          "transportSnapshot", QString::fromUtf8(QJsonDocument(snapshot).toJson(QJsonDocument::Compact))
      );
    });
    QObject::connect(autoTransport.get(), &ConnectionManager::operationFinished, &app, [&](const QJsonObject &result) {
      ipcServer->broadcastCommand(
          "transportResult", QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))
      );
    });
    QObject::connect(autoTransport.get(), &ConnectionManager::stateChanged, &app, [&](const QString &state) {
      ipcServer->broadcastCommand("transportState", state);
    });
    QObject::connect(autoTransport.get(), &ConnectionManager::trustRequired, &app, [&](const QByteArray &pin) {
      ipcServer->broadcastCommand("peerFingerprint", QString::fromLatin1(pin.toHex(':')));
    });
    QObject::connect(autoTransport.get(), &ConnectionManager::clipboardLimit, &app, [](const QString &id, int kib) {
      PeerPolicy::setLimit(id.toStdString(), kib);
    });
    QObject::connect(ipcServer, &deskflow::core::ipc::IpcServer::transportCommand, &app, [&](const QString &text) {
      const auto doc = QJsonDocument::fromJson(text.toUtf8());
      if (doc.isObject()) {
        autoTransport->command(doc.object());
        if (doc.object().value("kind") == "defaultPolicy" && doc.object().value("enabled").isBool())
          Settings::setValue(Settings::Connection::AutoBackup, doc.object().value("enabled").toBool());
        if (parser.serverMode() && doc.object().value("select").toBool())
          Settings::setValue(
              Settings::Connection::BackupTarget, autoTransport->snapshot().value("backupPeer").toString()
          );
      }
    });
    QObject::connect(
        ipcServer, &deskflow::core::ipc::IpcServer::commandBroadcast, &app,
        [&](const QString &command, const QString &args) {
          if (command == "transportPower") {
            autoTransport->suspend(args == "suspend");
          } else if (command == "peerCore") {
            const auto parts = args.split(':');
            if (parts.size() == 2)
              autoTransport->coreConnected(parts[0], parts[1] == "connected");
          } else if (command == "connectionState" && !parser.serverMode())
            autoTransport->coreConnected({}, args == "Connected");
        }
    );
    QObject::connect(&app, &QCoreApplication::aboutToQuit, autoTransport.get(), &ConnectionManager::stop);
    QTimer::singleShot(0, &app, [&] { autoTransport->start(); });
  } else
#endif
  {
    coreStarted = true;
    coreApp->run(coreThread);
  }

  int exitCode = QApplication::exec();
  if (coreThread.isRunning())
    coreApp->quit();
  coreThread.wait();

  if (exitCode == s_exitSuccess) {
    exitCode = coreApp->getExitCode();
  }
  delete coreApp;

  LOG_DEBUG("core exited, code: %d", exitCode);
  return exitCode;
}
