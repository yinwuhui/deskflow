// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "DisplayService.h"
#include "PeerUsageService.h"
namespace deskflow::display {
// Joins the usage transaction to the display subservice. Input effects remain
// the responsibility of an authenticated per-target input driver.
class UsageDisplayBridge : public QObject {
  Q_OBJECT
public:
  UsageDisplayBridge(PeerUsageService* usage,DisplayService* display,QObject* parent=nullptr);
  Outcome adoptIncoming();
  Outcome adoptCleanup();
Q_SIGNALS:
  void receiverInputReady();
private:
  void effect(const QJsonObject& command);
  void displayChanged();
  PeerUsageService* m_usage;
  DisplayService* m_display;
  QString m_peer;
  UsageKey m_usageKey;
  SessionId m_request{};
  bool m_active=false;
  bool m_incoming=false;
};
}
