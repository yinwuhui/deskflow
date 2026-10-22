// SPDX-FileCopyrightText: (C) 2026 busymouse contributors
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once

#include <QString>

// Copy a legacy profile atomically. Existing destinations and the source are never changed.
bool migrateDeskflowProfile(
    const QString &oldDirectory, const QString &newDirectory, const QString &oldState = {}, const QString &newState = {}
);
