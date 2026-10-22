// SPDX-License-Identifier: MIT
#include "BluetoothDiscovery.h"
#include "BluetoothPairing.h"
#include "RfcommBridge.h"
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothLocalDevice>
#include <QBluetoothPermission>
#include <QBluetoothServer>
#include <QBluetoothServiceInfo>
#include <QBluetoothSocket>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QPointer>
#include <QSocketNotifier>
#include <QTcpServer>
#include <QTextStream>
#include <algorithm>
#include <memory>
#ifdef Q_OS_MACOS
#include <QGuiApplication>
#endif

int main(int argc, char **argv)
{
#ifdef Q_OS_MACOS
  // Apple permission dialogs require a GUI application, even without windows.
  QGuiApplication app(argc, argv);
#else
  QCoreApplication app(argc, argv);
#endif
  app.setApplicationName("busymouse-rfcomm");
  app.setApplicationVersion("0.1");
  QCommandLineParser parser;
  parser.setApplicationDescription("Experimental macOS server / Linux client RFCOMM bridge and RTT probe.");
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addOptions({
      {"scan", "Discover nearby classic Bluetooth devices; pair using your system settings."},
      {"pair", "Pair with a device; type yes/no or a PIN when explicitly requested.", "address"},
      {"services", "Discover busymouse services on a peer.", "address"},
      {"server", "Listen for RFCOMM on macOS; connect to the local busymouse server."},
      {"peer", "Required: peer Bluetooth address (server: allowed Linux peer; client: Mac).", "address"},
      {"probe", "Client: measure RTT without running busymouse; reconnect automatically."},
      {"port", "Loopback TCP port (server: busymouse server port; client: bridge listener).", "port", "24800"},
      {"drop-after", "Simulate one disconnect N seconds after first readiness (client only).", "seconds", "0"},
      {"duration", "Exit after N seconds (0: run until interrupted).", "seconds", "0"},
  });
  parser.process(app);
  bool portOk, dropOk, durationOk;
  const auto port = parser.value("port").toUInt(&portOk);
  const auto drop = parser.value("drop-after").toInt(&dropOk);
  const auto duration = parser.value("duration").toInt(&durationOk);
  const QBluetoothAddress peer(parser.value("peer"));
  if (!portOk || !port || port > 65535 || !dropOk || drop < 0 || drop > 86400 || !durationOk || duration < 0 ||
      duration > 86400 ||
      (!parser.isSet("scan") && !parser.isSet("pair") && !parser.isSet("services") && peer.isNull()) ||
      (parser.isSet("server") && (parser.isSet("probe") || drop))) {
    qCritical() << "Invalid arguments; use --help. --peer is required except with --scan.";
    return 2;
  }
  const int actions = int(parser.isSet("scan")) + int(parser.isSet("pair")) + int(parser.isSet("services"));
  if (actions > 1 || (actions && (parser.isSet("server") || parser.isSet("probe") || parser.isSet("peer"))) ||
      (parser.isSet("pair") && QBluetoothAddress(parser.value("pair")).isNull()) ||
      (parser.isSet("services") && QBluetoothAddress(parser.value("services")).isNull())) {
    qCritical() << "Conflicting actions or invalid Bluetooth address";
    return 2;
  }
  // Keep permission requests and Bluetooth objects on the Qt main thread.
  auto run = [&] {
    if (duration)
      QTimer::singleShot(duration * 1000, &app, &QCoreApplication::quit);
    if (parser.isSet("pair")) {
      auto *pairing = new BluetoothPairing(&app);
      auto pending = std::make_shared<int>(0);
      QObject::connect(pairing, &BluetoothPairing::confirmationRequired, &app, [pending](const auto &code) {
        *pending = 1;
        qInfo().noquote() << "Compare code on BOTH devices:" << code << "Type yes to confirm, no to reject:";
      });
      QObject::connect(pairing, &BluetoothPairing::pinRequired, &app, [pending] {
        *pending = 2;
        qInfo() << "Enter the PIN displayed/required by the other device:";
      });
      QObject::connect(pairing, &BluetoothPairing::passkeyDisplayed, &app, [](const auto &code) {
        qInfo().noquote() << "Enter this passkey on the other device:" << code;
      });
      auto *input = new QSocketNotifier(fileno(stdin), QSocketNotifier::Read, pairing);
      QObject::connect(input, &QSocketNotifier::activated, pairing, [pairing, pending] {
        const auto line = QTextStream(stdin).readLine();
        if (line.isNull()) {
          pairing->cancel();
          QCoreApplication::exit(1);
        } else if (*pending == 1) {
          pairing->confirm(line.trimmed().compare("yes", Qt::CaseInsensitive) == 0);
          *pending = 0;
        } else if (*pending == 2) {
          pairing->providePin(line.trimmed());
          *pending = 0;
        }
      });
      QObject::connect(pairing, &BluetoothPairing::finished, &app, [](bool success, const auto &error) {
        qInfo().noquote() << "pairing_success=" << success << error;
        QCoreApplication::exit(success ? 0 : 1);
      });
      QObject::connect(&app, &QCoreApplication::aboutToQuit, pairing, &BluetoothPairing::cancel);
      pairing->pair(QBluetoothAddress(parser.value("pair")));
    } else if (parser.isSet("scan") || parser.isSet("services")) {
      auto *scanner = new BluetoothDiscovery(&app);
      auto failed = std::make_shared<bool>(false);
      QObject::connect(scanner, &BluetoothDiscovery::deviceFound, &app, [](const auto &address, const auto &name) {
        qInfo().noquote() << address << name;
      });
      QObject::connect(
          scanner, &BluetoothDiscovery::serviceFound, &app,
          [](const auto &address, const auto &name, const auto &uuid, int channel) {
            qInfo().noquote() << address << name << uuid << "channel=" << channel;
          }
      );
      QObject::connect(scanner, &BluetoothDiscovery::error, &app, [failed](const auto &error) {
        *failed = true;
        qCritical().noquote() << error;
      });
      QObject::connect(scanner, &BluetoothDiscovery::finished, &app, [failed] {
        QCoreApplication::exit(*failed ? 1 : 0);
      });
      if (parser.isSet("scan"))
        scanner->scan();
      else
        scanner->discoverServices(QBluetoothAddress(parser.value("services")));
    } else {
      auto *prototype = new RfcommBridge(parser.isSet("server"), parser.isSet("probe"), peer, quint16(port), drop);
      prototype->setParent(&app);
      QObject::connect(prototype, &RfcommBridge::fatalError, &app, [](const QString &error) {
        qCritical().noquote() << error;
        QCoreApplication::exit(1);
      });
      QObject::connect(&app, &QCoreApplication::aboutToQuit, prototype, &RfcommBridge::stop);
      prototype->start();
    }
  };
  requestBluetoothPermission(&app, parser.isSet("server"), [&](bool granted) {
    if (granted)
      run();
    else {
      qCritical() << "Bluetooth permission denied. Enable it in system privacy settings.";
      app.exit(1);
    }
  });
  return app.exec();
}
