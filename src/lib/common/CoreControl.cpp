// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "CoreControl.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
namespace deskflow {
std::optional<QJsonObject> decodeCoreControlRequest(const QString &encoded)
{
  if (encoded.size() > 8192)
    return {};
  const auto bytes = QByteArray::fromBase64(
      encoded.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors
  );
  if (bytes.isEmpty() || bytes.size() > 4096)
    return {};
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(bytes, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    return {};
  const auto request = document.object();
  const auto id = request.value("id");
  const auto command = request.value("command");
  const auto argument = request.value("argument");
  if (!id.isString() || id.toString().isEmpty() || id.toString().size() > 128 || !command.isString() ||
      (!argument.isUndefined() && !argument.isString()))
    return {};
  const auto operation = command.toString();
  const auto value = argument.toString();
  if (operation == "ready") {
    if (!value.isEmpty())
      return {};
  } else if (operation == "pause") {
    if (value != "0" && value != "1")
      return {};
  } else if (operation == "revoke") {
    if (!QRegularExpression("^[a-f0-9]{64}$").match(value).hasMatch())
      return {};
  } else if (operation == "reloadLayout") {
    // The core additionally checks the actual file, parser, and allowed change scope.
    if (value.isEmpty() || value.contains(QChar::Null) || !QFileInfo(value).isAbsolute())
      return {};
  } else
    return {};
  return request;
}
} // namespace deskflow
