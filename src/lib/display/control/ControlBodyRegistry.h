// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "InfrastructureControl.h"
namespace deskflow::display::infra {
// Schema validation for the common control plane. Caller supplies the identity
// from its authenticated transport; no field in the body grants authority.
// Plan references, barriers, grants, state and standard backend payloads still
// require the corresponding semantic/adapter checks before any side effect.
bool validateControlBody(const Envelope &, const QString &authenticatedSender, ControlError &);
} // namespace deskflow::display::infra
