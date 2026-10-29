// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "InfrastructureControl.h"
#include "display/supervision/MediaWorkerContracts.h"
namespace deskflow::display::infra {
// Schema validation for the common control plane. Caller supplies the identity
// from its authenticated transport; no field in the body grants authority.
// Plan references, barriers, grants, state and standard backend payloads still
// require the corresponding semantic/adapter checks before any side effect.
struct ControlMediaSignal
{
  uint64_t sequence = 0;
  WorkerMediaSignal signal;
};
QJsonObject encodeWebRtcSignalBody(const ControlMediaSignal &, ControlError &);
std::optional<ControlMediaSignal> decodeWebRtcSignalBody(const QJsonObject &, ControlError &);
QJsonObject encodeFrameCorrelationBody(const WebRtcFrameCorrelation &, ControlError &);
std::optional<WebRtcFrameCorrelation> decodeFrameCorrelationBody(const QJsonObject &, ControlError &);
bool validateControlBody(const Envelope &, const QString &authenticatedSender, ControlError &);
} // namespace deskflow::display::infra
