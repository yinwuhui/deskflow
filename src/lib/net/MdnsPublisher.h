/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 *
 * MdnsPublisher: advertises a Deskflow server on the local network using
 * multicast DNS (RFC 6762 / DNS-SD RFC 6763) so that clients can discover it
 * without knowing its IP address or requiring a system mDNS resolver.
 */

#pragma once

#include <QThread>
#include <QString>

namespace deskflow::mdns {

class MdnsPublisherPrivate;

// Runs a dedicated thread that owns the mDNS sockets, announces the service on
// start, answers incoming queries while running, and sends a goodbye on stop.
class MdnsPublisher : public QThread
{
  Q_OBJECT
public:
  // name  : instance/service name (ComputerName). Must not contain dots.
  // port  : TCP port the Deskflow server listens on.
  // fingerprint : SHA-256 hex of the server TLS certificate (may be empty).
  MdnsPublisher(const QString &name, int port, const QString &fingerprint, QObject *parent = nullptr);
  ~MdnsPublisher() override;

  void stop();

Q_SIGNALS:
  void errorOccurred(const QString &message);

protected:
  void run() override;

private:
  MdnsPublisherPrivate *d;
};

} // namespace deskflow::mdns
