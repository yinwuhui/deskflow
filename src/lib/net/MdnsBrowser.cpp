/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 *
 * MdnsBrowser implementation.
 */

#include "MdnsBrowser.h"
#include "MdnsCommon.h"

#include <QDateTime>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QVector>
#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "mdns/mdns.h"

#if defined(Q_OS_WIN)
#include <iphlpapi.h>
#include <ws2def.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace deskflow::mdns {

namespace {

// Trailing-dot stripped form of a DNS name, usable as a connectable host.
QString stripTrailingDot(const char *s, size_t n)
{
  QString str = QString::fromUtf8(s, int(n));
  while (str.endsWith('.'))
    str.chop(1);
  return str;
}

struct Entry
{
  DiscoveredServer info;
  qint64 lastSeenMsec = 0;
};

// Aggregated state for the current receive batch. Owned by the browser thread.
struct BrowserState
{
  std::map<std::string, Entry> entries;       // keyed by service instance name
  std::map<std::string, QString> ipByHost;   // qualified hostname -> IPv4 string
  bool changed = false;
};

} // namespace

// mDNS response callback: accumulates PTR/SRV/A/TXT records into BrowserState.
// Must have C language linkage (mdns.h declares the callback type inside an
// extern "C" block).
extern "C" int deskflowMdnsBrowserCallback(int sock, const struct sockaddr *from, size_t addrlen,
                                           mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
                                           uint16_t rclass, uint32_t ttl, const void *data, size_t size,
                                           size_t name_offset, size_t name_length, size_t record_offset,
                                           size_t record_length, void *user_data)
{
  (void)sock;
  (void)from;
  (void)addrlen;
  (void)query_id;
  (void)rclass;
  (void)name_length;

  if (entry != MDNS_ENTRYTYPE_ANSWER && entry != MDNS_ENTRYTYPE_ADDITIONAL)
    return 0;

  auto *st = static_cast<BrowserState *>(user_data);

  char namebuffer[256] = {0};
  size_t offset = name_offset;
  mdns_string_t recname = mdns_string_extract(data, size, &offset, namebuffer, sizeof(namebuffer));
  const std::string recName(recname.str, recname.length);

  const qint64 now = QDateTime::currentMSecsSinceEpoch();

  if (ttl == 0) {
    auto it = st->entries.find(recName);
    if (it != st->entries.end()) {
      st->entries.erase(it);
      st->changed = true;
    }
    return 0;
  }

  auto touch = [&](const std::string &key) -> Entry & {
    auto &e = st->entries[key];
    if (e.lastSeenMsec == 0)
      st->changed = true;
    e.lastSeenMsec = now;
    return e;
  };

  if (rtype == MDNS_RECORDTYPE_PTR) {
    char buf[256] = {0};
    mdns_string_t ptr = mdns_record_parse_ptr(data, size, record_offset, record_length, buf, sizeof(buf));
    std::string instance(ptr.str, ptr.length);
    Entry &e = touch(instance);
    e.info.hostName = stripTrailingDot(ptr.str, ptr.length);
    if (e.info.name.isEmpty())
      e.info.name = e.info.hostName;
  } else if (rtype == MDNS_RECORDTYPE_SRV) {
    char buf[256] = {0};
    mdns_record_srv_t srv = mdns_record_parse_srv(data, size, record_offset, record_length, buf, sizeof(buf));
    Entry &e = touch(recName);
    e.info.hostName = stripTrailingDot(srv.name.str, srv.name.length);
    e.info.port = int(srv.port);
    if (e.info.name.isEmpty())
      e.info.name = e.info.hostName;
  } else if (rtype == MDNS_RECORDTYPE_A) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    mdns_record_parse_a(data, size, record_offset, record_length, &addr);
    char ipbuf[INET_ADDRSTRLEN] = {0};
#if defined(Q_OS_WIN)
    inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
#else
    inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
#endif
    st->ipByHost[recName] = QString::fromUtf8(ipbuf);
    st->changed = true;
  } else if (rtype == MDNS_RECORDTYPE_TXT) {
    mdns_record_txt_t txt[16];
    size_t parsed = mdns_record_parse_txt(data, size, record_offset, record_length, txt,
                                          sizeof(txt) / sizeof(mdns_record_txt_t));
    Entry &e = touch(recName);
    for (size_t i = 0; i < parsed; ++i) {
      QString key = QString::fromUtf8(txt[i].key.str, int(txt[i].key.length));
      QString value = QString::fromUtf8(txt[i].value.str, int(txt[i].value.length));
      if (key == QLatin1String(kTxtFingerprint))
        e.info.fingerprint = value;
      else if (key == QLatin1String(kTxtComputerName) && !value.isEmpty())
        e.info.name = value;
    }
  }

  return 0;
}

// Open one ephemeral IPv4 socket per non-loopback interface for sending queries.
std::vector<int> openClientSockets()
{
  std::vector<int> sockets;

#if defined(Q_OS_WIN)
  ULONG size = 8192;
  std::vector<unsigned char> buf(size);
  DWORD ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr,
                                   reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()), &size);
  if (ret == ERROR_BUFFER_OVERFLOW) {
    buf.resize(size);
    ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr,
                               reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()), &size);
  }
  if (ret != NO_ERROR)
    return sockets;
  for (auto *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()); adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp)
      continue;
    for (auto *ua = adapter->FirstUnicastAddress; ua; ua = ua->Next) {
      auto *sa = reinterpret_cast<struct sockaddr_in *>(ua->Address.lpSockaddr);
      if (sa->sin_family != AF_INET)
        continue;
      if ((sa->sin_addr.S_un.S_un_b.s_b1 == 127) && (sa->sin_addr.S_un.S_un_b.s_b2 == 0) &&
          (sa->sin_addr.S_un.S_un_b.s_b3 == 0) && (sa->sin_addr.S_un.S_un_b.s_b4 == 1))
        continue;
      struct sockaddr_in bind = *sa;
      bind.sin_port = 0;
      int sock = mdns_socket_open_ipv4(&bind);
      if (sock >= 0)
        sockets.push_back(sock);
    }
  }
#else
  struct ifaddrs *ifaddr = nullptr;
  if (getifaddrs(&ifaddr) < 0)
    return sockets;
  for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
      continue;
    if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_MULTICAST))
      continue;
    if (ifa->ifa_flags & IFF_LOOPBACK)
      continue;
    auto *s = reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr);
    if (s->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
      continue;
    struct sockaddr_in bind = *s;
    bind.sin_port = 0;
    int sock = mdns_socket_open_ipv4(&bind);
    if (sock >= 0)
      sockets.push_back(sock);
  }
  freeifaddrs(ifaddr);
#endif

  return sockets;
}

struct MdnsBrowserPrivate
{
  // Never reset in run(): stop() may be called before initialization completes.
  std::atomic<bool> stopRequested{false};
  mutable QMutex mutex;
  QVector<DiscoveredServer> snapshot;
  alignas(32) char buffer[2048];
  BrowserState state;
};

MdnsBrowser::MdnsBrowser(QObject *parent) : QThread(parent), d(new MdnsBrowserPrivate) {}

MdnsBrowser::~MdnsBrowser()
{
  stop();
  wait();
  delete d;
}

void MdnsBrowser::stop()
{
  d->stopRequested = true;
}

QVector<DiscoveredServer> MdnsBrowser::servers() const
{
  QMutexLocker lock(&d->mutex);
  return d->snapshot;
}

void MdnsBrowser::run()
{
  std::vector<int> sockets = openClientSockets();
  if (sockets.empty()) {
    d->stopRequested = true;
    return;
  }

  static const std::string service = kServiceType;

  qint64 nextQueryMsec = 0;
  qint64 nextPruneMsec = 0;

  auto sendQuery = [&]() {
    for (int sock : sockets)
      mdns_query_send(sock, MDNS_RECORDTYPE_PTR, service.c_str(), service.size(), d->buffer,
                      sizeof(d->buffer), 0);
  };

  while (!d->stopRequested) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (now >= nextQueryMsec) {
      sendQuery();
      nextQueryMsec = now + 3000;
    }

    fd_set readfs;
    FD_ZERO(&readfs);
    int nfds = 0;
    for (int sock : sockets) {
      FD_SET(sock, &readfs);
      if (sock >= nfds)
        nfds = sock + 1;
    }
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    if (select(nfds, &readfs, nullptr, nullptr, &timeout) >= 0) {
      for (int sock : sockets) {
        if (FD_ISSET(sock, &readfs))
          mdns_query_recv(sock, d->buffer, sizeof(d->buffer), &deskflowMdnsBrowserCallback, &d->state, 0);
      }
    }

    // Correlate A records to entries by hostname.
    for (auto &kv : d->state.entries) {
      Entry &e = kv.second;
      if (!e.info.hostName.isEmpty()) {
        std::string hostKey = (e.info.hostName + QStringLiteral(".")).toStdString();
        auto it = d->state.ipByHost.find(hostKey);
        if (it != d->state.ipByHost.end() && e.info.ipAddress != it->second) {
          e.info.ipAddress = it->second;
          d->state.changed = true;
        }
      }
    }

    // Prune stale entries (not seen for 15 seconds).
    if (now >= nextPruneMsec) {
      nextPruneMsec = now + 1000;
      for (auto it = d->state.entries.begin(); it != d->state.entries.end();) {
        if (now - it->second.lastSeenMsec > 15000) {
          it = d->state.entries.erase(it);
          d->state.changed = true;
        } else {
          ++it;
        }
      }
    }

    if (d->state.changed) {
      d->state.changed = false;
      QVector<DiscoveredServer> list;
      list.reserve(int(d->state.entries.size()));
      for (auto &kv : d->state.entries)
        list.append(kv.second.info);
      {
        QMutexLocker lock(&d->mutex);
        d->snapshot = list;
      }
      Q_EMIT serversChanged();
    }
  }

  for (int sock : sockets)
    mdns_socket_close(sock);
}

} // namespace deskflow::mdns
