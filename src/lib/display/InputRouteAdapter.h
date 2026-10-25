// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "DisplayService.h"
#include "PeerUsageService.h"
#include <QElapsedTimer>
#include <QTimer>
#include <map>
namespace deskflow::display {
// Converts transaction effects into generation-tagged core IPC operations.
// Completion requires an exact peer/operation/sequence response from the core.
class InputRouteAdapter : public QObject {
  Q_OBJECT
public:
  InputRouteAdapter(PeerUsageService* usage,DisplayService* display,QObject* parent=nullptr);
  void coreResult(const QJsonObject& result);
  void coreUnavailable();
Q_SIGNALS:
  void coreCommand(const QJsonObject& command);
private:
  struct Pending {
    UsageKey key;
    QString effect;
    QString requestId;
    QString kind;
    uint32_t sequence=0;
    bool enabled=false;
    QElapsedTimer elapsed;
  };
  void effect(const QJsonObject& command);
  void send(const QString& peer,Pending& pending,const QString& kind);
  void fail(const QString& peer,Reason reason);
  void tick();
  PeerUsageService* m_usage;
  DisplayService* m_display;
  std::map<QString,Pending> m_pending;
  std::map<QString,uint32_t> m_sequence;
  std::map<QString,std::pair<UsageKey,uint32_t>> m_released;
  QTimer m_timer;
};
}
