// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "display/supervision/PlatformWorkerContracts.h"
#include <QRectF>
#include <memory>
namespace deskflow::display::infra {
struct MacDisplayRecord
{
  uint32_t id = 0, vendor = 0, product = 0, serial = 0, pixelWidth = 0, pixelHeight = 0;
  QString identity;
  QRectF bounds;
  bool active = false, mirrored = false;
};
struct MacDisplayInventory
{
  bool complete = false;
  QString digest;
  std::vector<MacDisplayRecord> displays;
};
struct MacDesktopState
{
  bool onConsole = false, lockKnown = false, locked = true;
};
MacDisplayInventory macDisplayInventory();
MacDesktopState macDesktopState();
QString macUserSessionReference();
QString macBootInstanceReference();
uint32_t macResourceSerial(const QString &owner, const QString &resource);
bool macResourceHintPresent(const MacDisplayInventory &, const QString &owner, const QString &resource);
// Main-thread, single native-object owner. Never deletes by display ID/name.
class MacVirtualOutput
{
public:
  MacVirtualOutput();
  ~MacVirtualOutput();
  bool supported() const;
  bool create(const WorkerPrepareOutput &, const QString &owner, QString &error);
  std::optional<WorkerOutputObserved> observation();
  bool configureLayout();
  bool healthy();
  void destroy();
  bool released() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
} // namespace deskflow::display::infra
