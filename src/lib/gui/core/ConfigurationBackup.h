// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVariant>
namespace deskflow::gui {
struct ConfigurationBackup
{
  QMap<QString, QVariant> settings;
  QByteArray serverConfig;
  QByteArray trustedServers, trustedClients, revokedFingerprints, peerIdentities;
  QByteArray encode() const;
  static bool decode(const QByteArray &bytes, ConfigurationBackup &result, QString &error);
};
} // namespace deskflow::gui
