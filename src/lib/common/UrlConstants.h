/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QString>

// important: this is used for settings paths on some platforms,
// and must not be a url. qt automatically converts this to reverse domain
// notation (rdn), e.g. org.busymouse
// Stable local identity, independent of the upstream documentation host.
const auto kOrgDomain = QStringLiteral("busymouse.org");

const auto kUrlSourceQuery = QStringLiteral("source=gui");
const auto kUrlApp = QStringLiteral("https://deskflow.org");
const auto kUrlHelp = QStringLiteral("%1/help?%2").arg(kUrlApp, kUrlSourceQuery);
const auto kUrlDownload = QStringLiteral("%1/download?%2").arg(kUrlApp, kUrlSourceQuery);
const auto kUrlWiki = QStringLiteral("%1/wiki?%2").arg(kUrlApp, kUrlSourceQuery);
// No busymouse update service has been configured.
const auto kUrlUpdateCheck = QString();

#if defined(Q_OS_LINUX)
const auto kUrlGnomeTrayFix = QStringLiteral("https://extensions.gnome.org/extension/615/appindicator-support/");
#endif
