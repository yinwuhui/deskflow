/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreIpcClient.h"

#include "common/Constants.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace deskflow::gui::ipc {

CoreIpcClient::CoreIpcClient(QObject *parent) : IpcClient(parent, kCoreIpcName, QStringLiteral("core"))
{
  // do nothing
}

void CoreIpcClient::sendStop()
{
  sendMessage(QStringLiteral("stop"));
}

void CoreIpcClient::sendControl(const QString &id, const QString &command, const QString &argument)
{
  const QJsonObject request{{"id", id}, {"command", command}, {"argument", argument}};
  sendMessage(
      "control=" + QString::fromLatin1(QJsonDocument(request)
                                           .toJson(QJsonDocument::Compact)
                                           .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))
  );
}

void CoreIpcClient::processCommand(const QString &command, const QStringList &parts)
{
  const auto args = parts.size() >= 2 ? parts.at(1) : QString();
  Q_EMIT commandReceived(command, args);
}

} // namespace deskflow::gui::ipc
