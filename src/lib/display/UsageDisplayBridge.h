// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "DisplayService.h"
#include "PeerUsageService.h"
#include "InfrastructureControlChannel.h"
#include "InputRouteAdapter.h"
namespace deskflow::display {
// Local authorization fact, not a peer assertion or a serializable grant. The
// future native owner must additionally commit the plan and bind OS resources.
class NativeDisplayAuthorization {
public:
  bool active() const { return m_live->load(std::memory_order_acquire)&&m_control.active()&&m_isolation.active(); }
  const Key& displayKey() const { return m_key; }
  Role role() const { return m_role; }
  const InfrastructureControlChannel::Lease& control() const { return m_control; }
  const InputIsolationProof& isolation() const { return m_isolation; }
private:
  friend class UsageDisplayBridge;
  NativeDisplayAuthorization(Key key,Role role,InfrastructureControlChannel::Lease control,InputIsolationProof isolation)
    :m_key(key),m_role(role),m_control(std::move(control)),m_isolation(std::move(isolation)),m_live(std::make_shared<std::atomic<bool>>(true)){}
  Key m_key;
  Role m_role;
  InfrastructureControlChannel::Lease m_control;
  InputIsolationProof m_isolation;
  std::shared_ptr<std::atomic<bool>> m_live;
};
// Joins the usage transaction to the display subservice. Input effects remain
// the responsibility of an authenticated per-target input driver.
class UsageDisplayBridge : public QObject {
  Q_OBJECT
public:
  UsageDisplayBridge(PeerUsageService* usage,DisplayService* display,QObject* parent=nullptr);
  ~UsageDisplayBridge() override;
  std::optional<NativeDisplayAuthorization> nativeAuthorization(
    const InfrastructureControlChannel::Lease&,const InputIsolationProof&);
  Outcome adoptIncoming();
  Outcome adoptCleanup();
Q_SIGNALS:
  void receiverInputReady();
private:
  void effect(const QJsonObject& command);
  void displayChanged();
  bool matchesAuthorization(const NativeDisplayAuthorization&) const;
  void revokeAuthorization();
  void checkAuthorization();
  std::optional<NativeDisplayAuthorization> m_authorization;
  PeerUsageService* m_usage;
  DisplayService* m_display;
  QString m_peer;
  UsageKey m_usageKey;
  SessionId m_request{};
  bool m_active=false;
  bool m_incoming=false;
};
}
