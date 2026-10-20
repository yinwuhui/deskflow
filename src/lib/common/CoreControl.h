// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QJsonObject>
#include <QString>
#include <optional>
namespace deskflow {
// Shared wire validation for the core's control dispatcher; no input or networking.
std::optional<QJsonObject> decodeCoreControlRequest(const QString &encoded);
} // namespace deskflow
