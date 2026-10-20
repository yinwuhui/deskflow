// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "LayoutReload.h"
#include "Settings.h"
#include <QSet>
namespace deskflow {
bool permitsLayoutReload(QSettings &active, QSettings &candidate)
{
  if (active.status() != QSettings::NoError || candidate.status() != QSettings::NoError)
    return false;
  if (active.value(Settings::Core::CoreMode).value<Settings::CoreMode>() != Settings::CoreMode::Server ||
      candidate.value(Settings::Core::CoreMode).value<Settings::CoreMode>() != Settings::CoreMode::Server)
    return false;
  QSet<QString> deviceGroups;
  auto keys = active.allKeys();
  for (const auto &key : keys)
    if (key.startsWith("computer_"))
      deviceGroups.insert(key.section('/', 0, 0));
  keys.append(candidate.allKeys());
  keys.removeDuplicates();
  for (const auto &key : keys) {
    if (key.startsWith("gui/") || key.startsWith("internalConfig/screens/") || key == Settings::Core::LastVersion ||
        key == Settings::Core::SharingPaused || key == Settings::Server::GridWidth ||
        key == Settings::Server::GridHeight || key == Settings::Server::ExternalConfig ||
        key == Settings::Server::ExternalConfigFile)
      continue;
    if (key.startsWith("computer_") && !deviceGroups.contains(key.section('/', 0, 0)))
      continue;
    if (active.value(key) != candidate.value(key))
      return false;
  }
  return true;
}
} // namespace deskflow
