/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Chris Rizzitello <sithord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2024 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/Constants.h"
#include "common/ExitCodes.h"
#include "common/I18N.h"
#include "common/PlatformInfo.h"
#include "common/UrlConstants.h"
#include "common/VersionInfo.h"
#include "gui/Diagnostic.h"
#include "gui/LoginStartup.h"
#include "gui/MainWindow.h"
#include "gui/Messages.h"
#include "gui/StyleUtils.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QLocalSocket>
#include <QMessageBox>
#include <QSharedMemory>
#include <QStorageInfo>

#if defined(Q_OS_MACOS)
#include <Carbon/Carbon.h>
#include <cstdlib>
#endif

#if defined(Q_OS_UNIX) && defined(QT_DEBUG)
#include <QLoggingCategory>
#endif

#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
#include "platform/XDGPortalRegistry.h"
#endif

using namespace deskflow::gui;

const static auto kHeader = QStringLiteral("%1: %2\n").arg(kAppName, kDisplayVersion);

int main(int argc, char *argv[])
{
#if defined(Q_OS_UNIX) && defined(QT_DEBUG)
  // Fixes Fedora bug where qDebug() messages aren't printed.
  QLoggingCategory::setFilterRules(QStringLiteral("*.debug=true\nqt.*=false"));
#endif

#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
  if (QT_VERSION < QT_VERSION_CHECK(6, 10, 0))
    deskflow::platform::setAppId();
#endif

  QCoreApplication::setApplicationName(kAppName);
  QCoreApplication::setOrganizationName(kAppName);
  QCoreApplication::setApplicationVersion(kVersion);
  QCoreApplication::setOrganizationDomain(kOrgDomain); // used in prefix, can't be a url
  QGuiApplication::setDesktopFileName(kRevFqdnName);

  QApplication app(argc, argv);

  // Ensure the I18N object is made before strings
  QTextStream(stdout) << "initial language: " << I18N::currentLanguage() << '\n';

  // Add Command Line Options
  auto helpOption = QCommandLineOption({"h", "help"}, "Display Help on the command line");
  auto versionOption = QCommandLineOption({"v", "version"}, "Display version information");
  auto resetOption = QCommandLineOption("reset", "Reset all settings");

  QCommandLineParser parser;
  parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
  parser.addOption(helpOption);
  parser.addOption(versionOption);
  parser.addOption(resetOption);
  parser.parse(QCoreApplication::arguments());

  if (!parser.errorText().isEmpty()) {
    qCritical().noquote() << parser.errorText() << "\nUse --help for more information.";
    return s_exitArgs;
  }

  if (parser.isSet(helpOption)) {
    QTextStream(stdout) << kHeader << QStringLiteral("  %1\n\n").arg(kAppDescription)
                        << parser.helpText().replace(QApplication::applicationFilePath(), kAppId);
    return s_exitSuccess;
  }

  if (parser.isSet(versionOption)) {
    QTextStream(stdout) << kHeader << kCopyright << Qt::endl;
    return s_exitSuccess;
  }

  const auto shmId = QStringLiteral("%1-gui").arg(kAppId);
  // Create a shared memory segment with a unique key
  // This is to prevent a new instance from running if one is already running
  QSharedMemory sharedMemory(shmId);

  // Attempt to attach first and detach in order to clean up stale shm chunks
  // This can happen if the previous instance was killed or crashed
  if (sharedMemory.attach())
    sharedMemory.detach();

  // If we can create 1 byte of SHM we are the only instance
  if (!sharedMemory.create(1)) {
    // Ping the running instance to have it show itself
    QLocalSocket socket;
    socket.connectToServer(shmId, QLocalSocket::ReadOnly);
    if (!socket.waitForConnected()) {
      // If we can't connect to the other instance tell the user its running.
      // This should never happen but just incase we should show something
      QMessageBox::information(nullptr, kAppName, QObject::tr("%1 is already running").arg(kAppName));
    }
    socket.disconnectFromServer();
    return s_exitDuplicate;
  }

  if (!deskflow::platform::isMac() && qEnvironmentVariable("XDG_CURRENT_DESKTOP") != QLatin1String("KDE")) {
    QApplication::setStyle("fusion");
  }

  // Sets the fallback icon path and fallback theme
  updateIconTheme();

  qInstallMessageHandler(deskflow::gui::messages::messageHandler);
  qInfo("%s v%s", kAppName, kDisplayVersion);
  QString startupMigrationError;
  if (!LoginStartup().migrateLegacyRegistration(startupMigrationError))
    qWarning().noquote() << "could not migrate login startup:" << startupMigrationError;

#if defined(Q_OS_MACOS)

  // Finder can launch us behind other windows. Startup alerts need a foreground app,
  // before MainWindow::showAndActivate() has had a chance to run.
  forceAppActive();
  const auto showStartupNotice = [](const QString &text) {
    QMessageBox notice(QMessageBox::Information, kAppName, text, QMessageBox::Ok);
    // A parentless native NSAlert before the main window exists can remain hidden
    // while blocking startup. Use an explicit Qt window and bring it to the front.
    notice.setOption(QMessageBox::Option::DontUseNativeDialog);
    notice.show();
    forceAppActive();
    notice.raise();
    notice.activateWindow();
    notice.exec();
  };

  // External writable disks also live under /Volumes. Only require installation
  // when the application really is on read-only media, such as an installer image.
  const QStorageInfo applicationVolume(app.applicationDirPath());
  if (applicationVolume.isValid() && applicationVolume.isReadOnly()) {
    const auto msgBody = QObject::tr(
        "Please drag %1 to the Applications folder, "
        "and open it from there."
    );
    showStartupNotice(msgBody.arg(kAppName));
    return 1;
  }

#endif

  // --no-reset
  if (parser.isSet(resetOption)) {
    diagnostic::clearSettings(false);
  }

  MainWindow mainWindow;
  mainWindow.listenForOtherInstances();
  mainWindow.open();

  return QApplication::exec();
}
