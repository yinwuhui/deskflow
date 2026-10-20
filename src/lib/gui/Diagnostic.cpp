/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "Diagnostic.h"

#include "common/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>

namespace deskflow::gui::diagnostic {

void restart()
{
  QString program = QCoreApplication::applicationFilePath();
  QStringList arguments = QCoreApplication::arguments();

  // look for and remove --reset option if found
  if (int resetIndex = arguments.indexOf("--reset"); resetIndex != -1)
    arguments.remove(resetIndex);

  qInfo("launching new process: %s", qPrintable(program));
  QProcess::startDetached(program, arguments);

  qDebug("exiting current process");
  QCoreApplication::exit();
}

QStringList managedProfileFiles()
{
  const auto root = QDir(Settings::settingsPath()).canonicalPath();
  if (root.isEmpty())
    return {};
  QStringList candidates{
      QDir(root).filePath(".workspace-state.json"),
      Settings::settingsFile(),
      Settings::settingsFile() + ".before-workspace",
      Settings::defaultValue(Settings::Server::ExternalConfigFile).toString(),
      Settings::tlsTrustedClientsDb(),
      Settings::tlsTrustedServersDb(),
      Settings::tlsRevokedDb(),
      Settings::peerIdentityDb(),
      Settings::value(Settings::Security::Certificate).toString(),
      Settings::value(Settings::Log::File).toString()
  };
  const auto snapshots = QDir(root).entryList({".runtime-*.conf"}, QDir::Files | QDir::Hidden | QDir::NoSymLinks);
  for (const auto &name : snapshots)
    candidates.append(QDir(root).filePath(name));
  QStringList files;
  const auto external =
      Settings::value(Settings::Server::ExternalConfig).toBool()
          ? QFileInfo(Settings::value(Settings::Server::ExternalConfigFile).toString()).canonicalFilePath()
          : QString();
  for (const auto &path : candidates) {
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    if (info.isFile() && !info.isSymLink() && canonical.startsWith(root + "/") && canonical != external)
      files.append(canonical);
  }
  files.removeDuplicates();
  files.sort();
  return files;
}

bool clearSettings(bool enableRestart, QStringList *failedFiles)
{
  const auto files = managedProfileFiles();
  Settings::proxy().clear();
  Settings::clearState();
  Settings::save(false);
  QStringList failed;
  for (const auto &path : files)
    if (QFile::exists(path) && !QFile::remove(path))
      failed.append(path);
  if (failedFiles)
    *failedFiles = failed;
  if (!failed.isEmpty())
    return false;
#ifdef Q_OS_WIN
  if (Settings::isPortableMode()) {
    QFile marker(Settings::settingsFile());
    if (!marker.open(QIODevice::WriteOnly) || marker.write(" ") != 1)
      return false;
  }
#endif
  if (enableRestart)
    restart();
  return true;
}

} // namespace deskflow::gui::diagnostic
