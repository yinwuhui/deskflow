// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QSettings>
#include <QString>
namespace deskflow {
// Adding positions may be applied live only when all other runtime settings
// and all existing device options remain identical.
bool permitsLayoutReload(QSettings &active, QSettings &candidate);
} // namespace deskflow
