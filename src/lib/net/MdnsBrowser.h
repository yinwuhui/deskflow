/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 *
 * MdnsBrowser: continuously browses the local network for Deskflow servers
 * advertising _deskflow._tcp.local and emits the list of discovered servers.
 */

#pragma once

#include "MdnsCommon.h"

#include <QHash>
#include <QObject>
#include <QThread>
#include <QVector>

namespace deskflow::mdns {

class MdnsBrowserPrivate;

// Runs on a dedicated thread. Periodically sends an mDNS PTR query for the
// Deskflow service type and aggregates the answers (PTR/SRV/A/TXT) into
// DiscoveredServer records. Emits serversChanged on the GUI thread whenever
// the set of discovered servers changes.
class MdnsBrowser : public QThread
{
  Q_OBJECT
public:
  explicit MdnsBrowser(QObject *parent = nullptr);
  ~MdnsBrowser() override;

  void stop();

  // Snapshot of currently known servers (thread-safe enough for UI reads
  // after serversChanged is received).
  QVector<DiscoveredServer> servers() const;

Q_SIGNALS:
  // Emitted (queued to the receiver's thread) when the discovered set changes.
  void serversChanged();

protected:
  void run() override;

private:
  MdnsBrowserPrivate *d;
};

} // namespace deskflow::mdns

Q_DECLARE_METATYPE(deskflow::mdns::DiscoveredServer)
