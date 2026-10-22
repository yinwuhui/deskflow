/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerConfig.h"

#include "common/Hotkey.h"
#include "common/Settings.h"

#include <QAbstractButton>
#include <QPushButton>

using enum ComputerConfig::SwitchCorner;
using enum ComputerConfig::Fix;

static const struct
{
  int x;
  int y;
  const char *name;
} neighbourDirs[] = {
    {1, 0, "right"},
    {-1, 0, "left"},
    {0, -1, "up"},
    {0, 1, "down"},

};

const int serverDefaultIndex = 7;

ServerConfig::ServerConfig(int columns, int rows) : m_computers(columns), m_columns(columns), m_rows(rows)
{
  recall();
}

bool ServerConfig::save(const QString &fileName) const
{
  QFile file(fileName);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    return false;

  save(file);
  file.close();

  return true;
}

bool ServerConfig::operator==(const ServerConfig &sc) const
{
  return m_computers == sc.m_computers && //
         m_Hotkeys == sc.m_Hotkeys && m_freeLayout == sc.m_freeLayout && m_layoutRectangles == sc.m_layoutRectangles;
}

void ServerConfig::save(QFile &file) const
{
  QTextStream outStream(&file);
  outStream << *this;
}

void ServerConfig::setupComputers()
{
  computers().clear();
  hotkeys().clear();

  // There must always be computer objects for each cell in the computers QList.
  // Unused computers are identified by having an empty name.
  for (int i = 0; i < m_columns * m_rows; i++)
    addComputer(Computer());
}

void ServerConfig::commit()
{
  qDebug("committing server config");
  Settings::setValue(Settings::Server::FreeLayout, m_freeLayout);
  QVariantMap rectangles;
  for (auto it = m_layoutRectangles.cbegin(); it != m_layoutRectangles.cend(); ++it) {
    if (!computerExists(it.key()))
      continue;
    const auto r = it.value();
    rectangles.insert(it.key(), QVariantList{r.x(), r.y(), r.width(), r.height()});
  }
  Settings::setValue(Settings::Server::LayoutRectangles, rectangles);

  settings().beginGroup("internalConfig");
  settings().remove("");

  settings().beginWriteArray("screens");
  for (int i = 0; i < computers().size(); i++) {
    settings().setArrayIndex(i);
    const auto &computer = computers()[i];
    computer.saveSettings(settings());
    auto computerName = Settings::value(Settings::Core::ComputerName).toString();
    if (computer.isServer() && computerName != computer.name()) {
      Settings::setValue(Settings::Core::ComputerName, computer.name());
    }
  }
  settings().endArray();

  settings().beginWriteArray("hotkeys");
  for (int i = 0; i < hotkeys().size(); i++) {
    settings().setArrayIndex(i);
    hotkeys()[i].saveSettings(settings().get());
  }
  settings().endArray();

  settings().endGroup();
}

void ServerConfig::recall()
{
  qDebug("recalling server config");
  m_freeLayout = Settings::value(Settings::Server::FreeLayout).toBool();
  const auto rectangles = Settings::value(Settings::Server::LayoutRectangles).toMap();
  for (auto it = rectangles.cbegin(); it != rectangles.cend(); ++it) {
    const auto values = it.value().toList();
    if (values.size() == 4)
      m_layoutRectangles.insert(
          it.key(), QRectF(values[0].toDouble(), values[1].toDouble(), values[2].toDouble(), values[3].toDouble())
      );
  }

  settings().beginGroup("internalConfig");

  m_columns = Settings::value(Settings::Server::GridWidth).toInt();
  m_rows = Settings::value(Settings::Server::GridHeight).toInt();

  // we need to know the number of columns and rows before we can set up
  // ourselves
  setupComputers();

  int numComputers = settings().beginReadArray("screens");
  Q_ASSERT(numComputers <= computers().size());
  for (int i = 0; i < numComputers; i++) {
    settings().setArrayIndex(i);
    computers()[i].loadSettings(settings());
    if (getServerName() == computers()[i].name()) {
      computers()[i].markAsServer();
    }
  }
  settings().endArray();

  int numHotkeys = settings().beginReadArray("hotkeys");
  for (int i = 0; i < numHotkeys; i++) {
    settings().setArrayIndex(i);
    Hotkey h;
    h.loadSettings(settings().get());
    hotkeys().append(h);
  }
  settings().endArray();

  settings().endGroup();
}

int ServerConfig::adjacentComputerIndex(int idx, int deltaColumn, int deltaRow) const
{
  if (computers()[idx].isNull())
    return -1;

  // if we're at the left or right end of the table, don't find results going
  // further left or right
  if ((deltaColumn > 0 && (idx + 1) % m_columns == 0) || (deltaColumn < 0 && idx % m_columns == 0))
    return -1;

  int arrayPos = idx + deltaColumn + deltaRow * m_columns;

  if (arrayPos >= computers().size() || arrayPos < 0)
    return -1;

  return arrayPos;
}

QTextStream &operator<<(QTextStream &outStream, const ServerConfig &config)
{
  outStream << "section: links" << Qt::endl;

  if (config.freeLayout()) {
    screenlayout::Rectangles rectangles;
    for (const auto &computer : config.computers())
      if (!computer.isNull() && config.layoutRectangles().contains(computer.name()))
        rectangles.insert(computer.name(), config.layoutRectangles().value(computer.name()));
    const auto links = screenlayout::links(rectangles);
    for (const auto &computer : config.computers()) {
      if (computer.isNull())
        continue;
      outStream << '\t' << computer.name() << ":\n";
      for (const auto &link : links)
        if (link.source == computer.name())
          outStream << "\t\t" << screenlayout::linkText(link) << Qt::endl;
    }
  } else {

    for (int i = 0; const auto &computer : config.computers()) {
      if (!computer.isNull()) {
        outStream << "\t" << computer.name() << ":\n";
        for (const auto &neighbour : std::as_const(neighbourDirs)) {
          int idx = config.adjacentComputerIndex(i, neighbour.x, neighbour.y);
          if (idx != -1 && !config.computers()[idx].isNull())
            outStream << "\t\t" << neighbour.name << " = " << config.computers()[idx].name() << Qt::endl;
        }
      }
      i++;
    }
  }

  outStream << "end" << Qt::endl << Qt::endl;

  outStream << "section: options" << Qt::endl;

  for (const Hotkey &hotkey : config.hotkeys())
    outStream << hotkey;

  outStream << "end" << Qt::endl << Qt::endl;

  return outStream;
}

int ServerConfig::numComputers() const
{
  int rval = 0;

  for (const Computer &s : computers()) {
    if (!s.isNull())
      rval++;
  }

  return rval;
}

QString ServerConfig::getServerName() const
{
  return Settings::value(Settings::Core::ComputerName).toString();
}

void ServerConfig::updateServerName()
{
  for (auto &computer : computers()) {
    if (computer.isServer()) {
      if (m_layoutRectangles.contains(computer.name())) {
        const auto rectangle = m_layoutRectangles.take(computer.name());
        m_layoutRectangles.insert(Settings::value(Settings::Core::ComputerName).toString(), rectangle);
      }
      computer.setName(Settings::value(Settings::Core::ComputerName).toString());
      break;
    }
  }
}

QString ServerConfig::configFile() const
{
  return Settings::value(Settings::Server::ExternalConfigFile).toString();
}

bool ServerConfig::useExternalConfig() const
{
  return Settings::value(Settings::Server::ExternalConfig).toBool();
}

bool ServerConfig::isFull() const
{
  bool isFull = true;

  for (const auto &computer : computers()) {
    if (computer.isNull()) {
      isFull = false;
      break;
    }
  }

  return isFull;
}

bool ServerConfig::computerExists(const QString &computerName) const
{
  bool isExists = false;

  for (const auto &computer : computers()) {
    if (!computer.isNull() && computer.name() == computerName) {
      isExists = true;
      break;
    }
  }

  return isExists;
}

void ServerConfig::addClient(const QString &clientName)
{
  int serverIndex = -1;
  const auto computerName = Settings::value(Settings::Core::ComputerName).toString();

  if (findComputerName(computerName, serverIndex)) {
    m_computers[serverIndex].markAsServer();
  } else {
    fixNoServer(computerName, serverIndex);
  }

  m_computers.addComputerByPriority(Computer(clientName));
  if (m_freeLayout && computerExists(clientName) && !m_layoutRectangles.contains(clientName)) {
    if (m_layoutRectangles.isEmpty()) {
      for (int i = 0; i < m_computers.size(); ++i)
        if (!m_computers[i].isNull())
          m_layoutRectangles.insert(
              m_computers[i].name(), QRectF((i % m_columns) * 320, (i / m_columns) * 200, 320, 200)
          );
    } else {
      auto right = m_layoutRectangles.first();
      for (const auto &rectangle : std::as_const(m_layoutRectangles))
        if (rectangle.right() > right.right())
          right = rectangle;
      m_layoutRectangles.insert(clientName, QRectF(right.right(), right.top(), 320, 200));
    }
  }
}

void ServerConfig::setConfigFile(const QString &configFile) const
{
  Settings::setValue(Settings::Server::ExternalConfigFile, configFile);
}

void ServerConfig::setUseExternalConfig(bool useExternalConfig) const
{
  Settings::setValue(Settings::Server::ExternalConfig, useExternalConfig);
}

bool ServerConfig::findComputerName(const QString &name, int &index)
{
  bool found = false;
  for (int i = 0; i < computers().size(); i++) {
    if (!computers()[i].isNull() && computers()[i].name().compare(name) == 0) {
      index = i;
      found = true;
      break;
    }
  }
  return found;
}

bool ServerConfig::fixNoServer(const QString &name, int &index)
{
  bool fixed = false;
  if (computers()[serverDefaultIndex].isNull()) {
    m_computers[serverDefaultIndex].setName(name);
    m_computers[serverDefaultIndex].markAsServer();
    index = serverDefaultIndex;
    fixed = true;
  }

  return fixed;
}

QSettingsProxy &ServerConfig::settings()
{
  return Settings::proxy();
}
