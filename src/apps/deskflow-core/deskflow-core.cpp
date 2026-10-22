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
#include "AutoTransport.h"
#include "BluetoothDiscovery.h"
#include "RfcommBridge.h"
#include "net/FingerprintDatabase.h"
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
  const bool automatic = parser.autoFailover() || Settings::value(Settings::Connection::AutoFailover).toBool();
#ifndef DESKFLOW_BLUETOOTH
  if (automatic) {
    LOG_CRIT("Automatic transport requires a build with Bluetooth support");
    return s_exitConfig;
  }
#endif

#ifdef DESKFLOW_BLUETOOTH
  int bluetoothPort = 0;
  if (parser.bluetoothRequested() || automatic) {
    if (!Settings::value(Settings::Security::TlsEnabled).toBool() ||
        !Settings::value(Settings::Security::CheckPeers).toBool()) {
      LOG_CRIT("Bluetooth transport requires TLS and peer fingerprint verification");
      return s_exitConfig;
    }
    bluetoothPort = Settings::value(Settings::Core::Port).toInt();
    if (bluetoothPort < 1 || bluetoothPort > 65535) {
      return s_exitConfig;
    }
  }
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

  QObject::connect(&coreThread, &QThread::finished, &app, &QApplication::quit);
#ifdef DESKFLOW_BLUETOOTH
  std::unique_ptr<RfcommBridge> bluetooth;
  std::unique_ptr<AutoTransport> autoTransport;
  if (automatic) {
    const QBluetoothAddress peer(
        parser.bluetoothRequested() ? parser.bluetoothPeer()
                                    : Settings::value(Settings::Connection::BluetoothPeer).toString()
    );
    FingerprintDatabase trusted;
    trusted.read(parser.serverMode() ? Settings::tlsTrustedClientsDb() : Settings::tlsTrustedServersDb());
    QList<QByteArray> pins;
    for (const auto &fingerprint : trusted.fingerprints())
      if (fingerprint.type == QCryptographicHash::Sha256 && !pins.contains(fingerprint.data))
        pins.append(fingerprint.data);
    auto configuredPin = Settings::value(Settings::Connection::PeerFingerprint).toString().remove(':');
    const auto explicitPin = QByteArray::fromHex(configuredPin.toLatin1());
    if (!configuredPin.isEmpty() && (configuredPin.size() != 64 ||
        explicitPin.toHex() != configuredPin.toLatin1().toLower())) {
      LOG_CRIT("Peer fingerprint must be exactly 64 hexadecimal digits (colons are allowed)");
      delete coreApp;
      return s_exitConfig;
    }
    const QByteArray pin = configuredPin.isEmpty() && pins.size() == 1 ? pins.first() : explicitPin;
    const auto host = Settings::value(Settings::Client::RemoteHost).toString().trimmed();
    if (peer.isNull() || pin.size() != 32 || !pins.contains(pin) ||
        (!parser.serverMode() && (host.isEmpty() || host.contains(',')))) {
      LOG_CRIT(
          "Auto failover needs a valid Bluetooth address, one network host, and one explicitly trusted SHA256 peer "
          "identity"
      );
      delete coreApp;
      return s_exitConfig;
    }
    AutoTransport::Options options;
    options.server = parser.serverMode();
    options.host = host;
    options.bluetoothPeer = peer;
    options.peerSha256 = pin;
    options.pem = Settings::value(Settings::Security::Certificate).toString();
    options.networkPort = quint16(bluetoothPort);
    const auto bindAddress = Settings::value(Settings::Core::Interface).toString();
    if (options.server && !bindAddress.isEmpty() && !options.listenAddress.setAddress(bindAddress)) {
      LOG_CRIT("Automatic transport requires a numeric listen address");
      delete coreApp;
      return s_exitConfig;
    }
    coreApp->setClipboardLimitKiB(64);
    autoTransport = std::make_unique<AutoTransport>(options);
    QObject::connect(autoTransport.get(), &AutoTransport::started, &app, [&](quint16 port) {
      if (!coreStarted) {
        coreStarted = true;
        coreApp->setLocalTransportPort(port);
        coreApp->run(coreThread);
      }
    });
    QObject::connect(autoTransport.get(), &AutoTransport::fatalError, &app, [&](const auto &error) {
      qCritical().noquote() << error;
      app.exit(s_exitConfig);
    });
    QObject::connect(autoTransport.get(), &AutoTransport::warning, &app, [](const auto &error) {
      qWarning().noquote() << error;
    });
    QObject::connect(autoTransport.get(), &AutoTransport::stateChanged, &app, [&](const auto &state) {
      ipcServer->broadcastCommand("transportState", state);
    });
    QObject::connect(
        ipcServer, &deskflow::core::ipc::IpcServer::commandBroadcast, &app,
        [&](const QString &command, const QString &args) {
          if (command == "connectionState")
            autoTransport->coreConnected(args == "Connected");
        }
    );
    QObject::connect(&app, &QCoreApplication::aboutToQuit, autoTransport.get(), &AutoTransport::stop);
    QTimer::singleShot(0, &app, [&] { autoTransport->start(); });
    requestBluetoothPermission(&app, parser.serverMode(), [&](bool granted) {
      if (granted)
        autoTransport->enableBluetooth();
      else
        qWarning("Bluetooth permission denied; network remains available");
    });
  } else if (parser.bluetoothRequested()) {
    bluetooth = std::make_unique<RfcommBridge>(
        parser.serverMode(), false, QBluetoothAddress(parser.bluetoothPeer()),
        parser.serverMode() ? quint16(bluetoothPort) : 0, 0
    );
    QObject::connect(bluetooth.get(), &RfcommBridge::started, &app, [&](quint16 localPort) {
      if (!coreStarted) {
        coreStarted = true;
        coreApp->setLocalTransportPort(localPort);
        coreApp->run(coreThread);
      }
    });
    QObject::connect(bluetooth.get(), &RfcommBridge::fatalError, &app, [&](const QString &error) {
      qCritical().noquote() << error;
      app.exit(s_exitFailed);
    });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, bluetooth.get(), &RfcommBridge::stop);
    requestBluetoothPermission(&app, parser.serverMode(), [&](bool granted) {
      if (granted)
        bluetooth->start();
      else {
        LOG_CRIT("Bluetooth permission denied");
        app.exit(s_exitFailed);
      }
    });
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
