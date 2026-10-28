// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include <QString>
#include <optional>
namespace deskflow::display::infra {
// Read-only evidence, never authority to terminate a PID. Persist with boot and
// user-session scope; termination still requires a live owned process/OS handle.
struct NativeProcessIdentity
{
  QString platform;
  quint64 pid = 0, birth = 0, fraction = 0;
  bool operator==(const NativeProcessIdentity &) const = default;
  QString encode() const;
  static std::optional<NativeProcessIdentity> decode(const QString &);
};
enum class NativeProcessPresence
{
  Exact,
  OriginalAbsent,
  Unknown
};
std::optional<NativeProcessIdentity> nativeProcessIdentity(quint64 pid);
NativeProcessPresence nativeProcessPresence(const NativeProcessIdentity &);
} // namespace deskflow::display::infra
