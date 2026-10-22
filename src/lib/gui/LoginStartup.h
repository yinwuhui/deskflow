/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QString>

// Registration only: never launches a process or changes input-control permissions.
class LoginStartup
{
public:
  explicit LoginStartup(QString executable = {}, QString directory = {}, QString identifier = {});
  bool isEnabled() const;
  bool setEnabled(bool enabled, QString &error) const;
  bool migrateLegacyRegistration(QString &error) const;
  QString registrationPath() const;

private:
  QByteArray registrationData() const;
  QString m_executable;
  QString m_directory;
  QString m_identifier;
};
