/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "common/Constants.h"
#include "common/Hotkey.h"
#include "gui/config/ComputerConfig.h"
#include "gui/config/ComputerList.h"

#include <QList>

class QTextStream;
class QSettings;
class QString;
class QFile;
class ServerConfigDialog;

namespace deskflow::gui::workspace {
class DeviceWorkspace;
}

class ServerConfig : public ComputerConfig
{
  friend class ServerConfigDialog;
  friend class deskflow::gui::workspace::DeviceWorkspace;
  friend QTextStream &operator<<(QTextStream &outStream, const ServerConfig &config);

public:
  explicit ServerConfig(int columns = kServerGridWidth, int rows = kServerGridHeight);
  ~ServerConfig() = default;

  bool operator==(const ServerConfig &sc) const;

  const ComputerList &computers() const
  {
    return m_computers;
  }

  //
  // New methods
  //
  const HotkeyList &hotkeys() const
  {
    return m_Hotkeys;
  }

  bool save(const QString &fileName) const;
  bool computerExists(const QString &computerName) const;
  void save(QFile &file) const;
  bool isFull() const;
  void commit();
  int numComputers() const;
  QStringList unreachableComputers() const;
  QString getServerName() const;
  void updateServerName();
  void reloadSaved()
  {
    recall();
  }
  int gridWidth() const
  {
    return m_columns;
  }
  int gridHeight() const
  {
    return m_rows;
  }
  QString loadError() const
  {
    return m_loadError;
  }
  QString configFile() const;
  bool useExternalConfig() const;
  void addClient(const QString &clientName);

private:
  void recall();
  void setupComputers();
  QSettingsProxy &settings();
  ComputerList &computers()
  {
    return m_computers;
  }
  void setComputers(const ComputerList &computers)
  {
    m_computers = computers;
  }
  void addComputer(const Computer &computer)
  {
    m_computers.append(computer);
  }
  void setConfigFile(const QString &configFile) const;
  void setUseExternalConfig(bool useExternalConfig) const;
  HotkeyList &hotkeys()
  {
    return m_Hotkeys;
  }
  int adjacentComputerIndex(int idx, int deltaColumn, int deltaRow) const;
  bool findComputerName(const QString &name, int &index);
  bool fixNoServer(const QString &name, int &index);

private:
  QString m_loadError;
  HotkeyList m_Hotkeys;

  ComputerList m_computers;
  int m_columns;
  int m_rows;
};

QTextStream &operator<<(QTextStream &outStream, const ServerConfig &config);
