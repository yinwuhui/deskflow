/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "LoginStartup.h"
#include "common/Constants.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

LoginStartup::LoginStartup(QString executable, QString directory, QString identifier)
    : m_executable(executable.isEmpty() ? QCoreApplication::applicationFilePath() : executable),
      m_directory(std::move(directory)),
      m_identifier(std::move(identifier))
{
  if (m_identifier.isEmpty()) {
    m_identifier = kRevFqdnName;
#ifdef Q_OS_MACOS
    QFile info(QFileInfo(m_executable).dir().filePath("../Info.plist"));
    if (info.open(QIODevice::ReadOnly)) {
      QXmlStreamReader xml(&info);
      while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QStringLiteral("key") &&
            xml.readElementText() == QStringLiteral("CFBundleIdentifier") && xml.readNextStartElement()) {
          m_identifier = xml.readElementText();
          break;
        }
      }
    }
#endif
  }
  m_identifier += QStringLiteral(".login");
}

QString LoginStartup::registrationPath() const
{
#ifdef Q_OS_WIN
  return m_directory.isEmpty() ? QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run")
                               : QDir(m_directory).filePath("startup.ini");
#elif defined(Q_OS_MACOS)
  return QDir(m_directory.isEmpty() ? QDir::homePath() + "/Library/LaunchAgents" : m_directory)
      .filePath(m_identifier + ".plist");
#else
  return QDir(
             m_directory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart"
                                   : m_directory
  )
      .filePath(m_identifier + ".desktop");
#endif
}

QByteArray LoginStartup::registrationData() const
{
#ifdef Q_OS_MACOS
  QByteArray data;
  QXmlStreamWriter xml(&data);
  xml.setAutoFormatting(true);
  xml.writeStartDocument();
  xml.writeStartElement("plist");
  xml.writeAttribute("version", "1.0");
  xml.writeStartElement("dict");
  xml.writeTextElement("key", "Label");
  xml.writeTextElement("string", m_identifier);
  xml.writeTextElement("key", "ProgramArguments");
  xml.writeStartElement("array");
  xml.writeTextElement("string", m_executable);
  xml.writeEndElement();
  xml.writeTextElement("key", "RunAtLoad");
  xml.writeEmptyElement("true");
  xml.writeTextElement("key", "ProcessType");
  xml.writeTextElement("string", "Interactive");
  xml.writeEndElement();
  xml.writeEndElement();
  xml.writeEndDocument();
  return data;
#else
  QString path = m_executable;
  path.replace("\\", "\\\\").replace("\"", "\\\"").replace("`", "\\`").replace("$", "\\$").replace("%", "%%");
  return QStringLiteral("[Desktop Entry]\nType=Application\nName=Deskflow\nExec=\"%1\"\nTerminal=false\n")
      .arg(path)
      .toUtf8();
#endif
}

bool LoginStartup::isEnabled() const
{
#ifdef Q_OS_WIN
  QSettings registry(registrationPath(), m_directory.isEmpty() ? QSettings::NativeFormat : QSettings::IniFormat);
  return registry.value(m_identifier).toString() == '"' + QDir::toNativeSeparators(m_executable) + '"';
#else
  QFile file(registrationPath());
  return file.open(QIODevice::ReadOnly) && file.readAll() == registrationData();
#endif
}

bool LoginStartup::setEnabled(bool enabled, QString &error) const
{
  error.clear();
#ifdef Q_OS_WIN
  QSettings registry(registrationPath(), m_directory.isEmpty() ? QSettings::NativeFormat : QSettings::IniFormat);
  if (enabled)
    registry.setValue(m_identifier, '"' + QDir::toNativeSeparators(m_executable) + '"');
  else
    registry.remove(m_identifier);
  registry.sync();
  if (registry.status() != QSettings::NoError) {
    error = QCoreApplication::translate("SettingsPage", "Could not update login startup registration.");
    return false;
  }
#else
  const auto path = registrationPath();
  if (!enabled) {
    if (QFile::exists(path) && !QFile::remove(path)) {
      error = QCoreApplication::translate("SettingsPage", "Could not remove login startup registration.");
      return false;
    }
    return true;
  }
  if (!QFileInfo(m_executable).isExecutable()) {
    error = QCoreApplication::translate("SettingsPage", "The application executable is not available.");
    return false;
  }
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    error = QCoreApplication::translate("SettingsPage", "Could not create the login startup directory.");
    return false;
  }
  QSaveFile file(path);
  const auto data = registrationData();
  if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
    error = file.errorString();
    return false;
  }
#endif
  return true;
}
