/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 *
 * Common definitions for mDNS based service discovery.
 */

#pragma once

#include <QString>

namespace deskflow::mdns {

// DNS-SD service type advertised by Deskflow servers and browsed by clients.
// Full qualified name as used on the wire (trailing dot included).
inline constexpr auto kServiceType = "_deskflow._tcp.local.";

// TXT record keys.
inline constexpr auto kTxtFingerprint = "fingerprint";
inline constexpr auto kTxtComputerName = "name";

// A server discovered on the local network via mDNS.
struct DiscoveredServer
{
  // Human readable instance name (the ComputerName of the server).
  QString name;
  // Host name to connect to, e.g. "server.local" (resolved by the OS mDNS resolver).
  QString hostName;
  // TCP port of the Deskflow server (usually 24800).
  int port = 0;
  // Best known IPv4 address (may be empty; the OS resolves hostName anyway).
  QString ipAddress;
  // SHA-256 fingerprint of the server TLS certificate (hex), if advertised.
  QString fingerprint;
};

} // namespace deskflow::mdns
