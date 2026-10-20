/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once
#include <QStringList>

namespace deskflow::gui::diagnostic {

QStringList managedProfileFiles();
bool clearSettings(bool enableRestart, QStringList *failedFiles = nullptr);

} // namespace deskflow::gui::diagnostic
