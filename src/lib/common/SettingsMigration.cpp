// SPDX-FileCopyrightText: (C) 2026 busymouse contributors
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "SettingsMigration.h"

#include "Settings.h"

#include <QDebug>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

bool migrateDeskflowProfile(
    const QString &oldDirectory, const QString &newDirectory, const QString &oldState, const QString &newState
)
{
  const QDir source(oldDirectory);
  if (!source.exists() || QFileInfo::exists(newDirectory))
    return false;
  if (!source.exists("Deskflow.conf") && !source.exists("tls/deskflow.pem"))
    return false;

  const auto parent = QFileInfo(newDirectory).absolutePath();
  if (!QDir().mkpath(parent))
    return false;
  QTemporaryDir staging(parent + "/.busymouse-migration-XXXXXX");
  if (!staging.isValid())
    return false;

  QDirIterator files(
      oldDirectory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories
  );
  while (files.hasNext()) {
    files.next();
    const auto info = files.fileInfo();
    // Never traverse or reproduce links to files outside the legacy profile.
    if (info.isSymLink()) {
      qWarning() << "legacy settings migration skipped: profile contains a symbolic link";
      return false;
    }
    const auto target = staging.filePath(source.relativeFilePath(info.filePath()));
    if (info.isDir()) {
      if (!QDir().mkpath(target))
        return false;
    } else if (!QFile::copy(info.filePath(), target)) {
      qWarning() << "could not copy legacy profile file:" << info.fileName();
      return false;
    }
  }

  const auto stagedSettings = staging.filePath("busymouse.conf");
  if (QFileInfo::exists(staging.filePath("Deskflow.conf")) &&
      !QFile::rename(staging.filePath("Deskflow.conf"), stagedSettings))
    return false;
  {
    QSettings settings(stagedSettings, QSettings::IniFormat);
    const QStringList pathKeys{
        Settings::Security::Certificate, Settings::Server::ExternalConfigFile, Settings::Daemon::ConfigFile,
        Settings::Daemon::LogFile, Settings::Log::File
    };
    const auto oldPrefix = source.absolutePath() + '/';
    const auto newPrefix = QDir(newDirectory).absolutePath() + '/';
    for (const auto &key : pathKeys) {
      const auto path = QDir::fromNativeSeparators(settings.value(key).toString());
      if (path.startsWith(oldPrefix)) {
        const auto relative = path.mid(oldPrefix.size());
        settings.setValue(key, newPrefix + (relative == "Deskflow.conf" ? "busymouse.conf" : relative));
      }
    }
    // Keep the original key pair (and therefore the peer's trusted fingerprint).
    if (!settings.contains(Settings::Security::Certificate) && source.exists("tls/deskflow.pem"))
      settings.setValue(Settings::Security::Certificate, newPrefix + "tls/deskflow.pem");
    if (!settings.contains(Settings::Server::ExternalConfigFile) && source.exists("deskflow-server.conf"))
      settings.setValue(Settings::Server::ExternalConfigFile, newPrefix + "deskflow-server.conf");

    // An upstream release must never be offered as an update for this fork.
    settings.setValue(Settings::Gui::AutoUpdateCheck, false);
    settings.setValue(Settings::Gui::UpdateCheckUrl, QString());
    settings.sync();
    if (settings.status() != QSettings::NoError)
      return false;
  }

  // QFile::copy preserves file permissions; preserve directory permissions too.
  QDirIterator directories(
      oldDirectory, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories
  );
  while (directories.hasNext()) {
    directories.next();
    if (!QFile::setPermissions(
            staging.filePath(source.relativeFilePath(directories.filePath())), directories.fileInfo().permissions()
        ))
      return false;
  }
  if (!QFile::setPermissions(staging.path(), QFileInfo(oldDirectory).permissions()))
    return false;
  if (!QDir().rename(staging.path(), newDirectory))
    return false;
  staging.setAutoRemove(false);
  if (!oldState.isEmpty() && !newState.isEmpty() && QFileInfo::exists(oldState) && !QFileInfo::exists(newState)) {
    if (!QDir().mkpath(QFileInfo(newState).absolutePath()) || !QFile::copy(oldState, newState))
      qWarning() << "legacy profile copied, but its window state could not be copied";
  }
  qInfo() << "copied legacy settings to:" << newDirectory;
  return true;
}
