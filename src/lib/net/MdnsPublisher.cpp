/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 *
 * MdnsPublisher implementation. Advertises _deskflow._tcp.local on the LAN.
 */

#include "MdnsPublisher.h"
#include "MdnsCommon.h"

#include <QByteArray>
#include <QString>
#include <atomic>
#include <string>
#include <vector>

#include "mdns/mdns.h"

#if defined(Q_OS_WIN)
#include <iphlpapi.h>
#include <ws2def.h>
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

// State shared between the run loop and the query callback.
struct ServiceState
{
  std::string service;          // "_deskflow._tcp.local."
  std::string hostname;        // "<name>"
  std::string serviceInstance; // "<name>._deskflow._tcp.local."
  std::string hostnameQualified; // "<name>.local."

  struct sockaddr_in addressIpv4{};
  bool hasIpv4 = false;
  int port = 0;

  mdns_record_t recordPtr{};
  mdns_record_t recordSrv{};
  mdns_record_t recordA{};
  mdns_record_t txtRecords[4]{};
  size_t txtCount = 0;

  // Backing storage for TXT key/value strings (must outlive the loop).
  std::string txtComputerName;
  std::string txtFingerprint;
};

// Replace characters that are invalid in a DNS label.
std::string sanitizeLabel(const QString &name)
{
  std::string out;
  out.reserve(name.size());
  for (QChar c : name) {
    char ch = c.toLatin1();
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-')
      out.push_back(ch);
    else
      out.push_back('-');
  }
  if (out.empty())
      out = "deskflow";
  return out;
}

// Enumerate non-loopback, non-tunnel IPv4 interfaces. One socket is opened per
// interface so that the mDNS multicast announce is sent out the *correct*
// interface instead of whatever the kernel picks as the default multicast egress
// (which on hosts with a VPN/proxy virtual interface is usually the tunnel).
std::vector<struct sockaddr_in> getLocalIpv4Interfaces()
{
  std::vector<struct sockaddr_in> out;
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
    return out;
  for (auto *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data()); adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp)
      continue;
    if (adapter->IfType == IF_TYPE_TUNNEL || adapter->IfType == IF_TYPE_PPP)
      continue;
    for (auto *ua = adapter->FirstUnicastAddress; ua; ua = ua->Next) {
      auto *sa = reinterpret_cast<struct sockaddr_in *>(ua->Address.lpSockaddr);
      if (sa->sin_family != AF_INET)
        continue;
      if ((sa->sin_addr.S_un.S_un_b.s_b1 == 127) && (sa->sin_addr.S_un.S_un_b.s_b2 == 0) &&
          (sa->sin_addr.S_un.S_un_b.s_b3 == 0) && (sa->sin_addr.S_un.S_un_b.s_b4 == 1))
        continue;
      out.push_back(*sa);
    }
  }
#else
  struct ifaddrs *ifaddr = nullptr;
  if (getifaddrs(&ifaddr) < 0)
    return out;
  for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
      continue;
    if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_MULTICAST))
      continue;
    if (ifa->ifa_flags & IFF_LOOPBACK)
      continue;
    if (ifa->ifa_flags & IFF_POINTOPOINT) // skip VPN / tunnel virtual ifaces
      continue;
    auto *s = reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr);
    if (s->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
      continue;
    out.push_back(*s);
  }
  freeifaddrs(ifaddr);
#endif
  return out;
}

} // namespace

// mDNS query callback: answers incoming DNS-SD/mDNS questions.
// Must have C language linkage (mdns.h declares the callback type inside an
// extern "C" block).
extern "C" int deskflowMdnsPublisherCallback(int sock, const struct sockaddr *from, size_t addrlen,
                                             mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
                                             uint16_t rclass, uint32_t ttl, const void *data, size_t size,
                                             size_t name_offset, size_t name_length, size_t record_offset,
                                             size_t record_length, void *user_data)
{
  (void)addrlen;
  (void)from;
  (void)query_id;
  (void)ttl;
  (void)name_length;
  if (entry != MDNS_ENTRYTYPE_QUESTION)
    return 0;

  auto *st = static_cast<ServiceState *>(user_data);

  char namebuffer[256] = {0};
  size_t offset = name_offset;
  mdns_string_t name = mdns_string_extract(data, size, &offset, namebuffer, sizeof(namebuffer));

  // Aligned buffer for outgoing answers.
  alignas(32) char sendbuffer[1024];

  const auto send = [&](mdns_record_t answer, const mdns_record_t *additional, size_t additional_count) {
    uint16_t unicast = (rclass & MDNS_UNICAST_RESPONSE);
    if (unicast) {
      mdns_query_answer_unicast(sock, from, addrlen, sendbuffer, sizeof(sendbuffer), query_id,
                                static_cast<mdns_record_type_t>(rtype), name.str, name.length, answer, 0,
                                0, additional, additional_count);
    } else {
      mdns_query_answer_multicast(sock, sendbuffer, sizeof(sendbuffer), answer, 0, 0, additional,
                                  additional_count);
    }
  };

  // DNS-SD meta-query: list our service type.
  const char dnsSd[] = "_services._dns-sd._udp.local.";
  if (name.length == (sizeof(dnsSd) - 1) && strncmp(name.str, dnsSd, sizeof(dnsSd) - 1) == 0) {
    if (rtype == MDNS_RECORDTYPE_PTR || rtype == MDNS_RECORDTYPE_ANY) {
      mdns_record_t answer{};
      answer.name = name;
      answer.type = MDNS_RECORDTYPE_PTR;
      answer.data.ptr.name = {st->service.c_str(), st->service.size()};
      send(answer, nullptr, 0);
    }
    return 0;
  }

  // PTR query for our service type -> answer with our service instance.
  if (name.length == st->service.size() && strncmp(name.str, st->service.c_str(), name.length) == 0) {
    if (rtype == MDNS_RECORDTYPE_PTR || rtype == MDNS_RECORDTYPE_ANY) {
      mdns_record_t additional[6]{};
      size_t n = 0;
      additional[n++] = st->recordSrv;
      if (st->hasIpv4)
        additional[n++] = st->recordA;
      for (size_t i = 0; i < st->txtCount; ++i)
        additional[n++] = st->txtRecords[i];
      send(st->recordPtr, additional, n);
    }
    return 0;
  }

  // SRV query for our service instance.
  if (name.length == st->serviceInstance.size() &&
      strncmp(name.str, st->serviceInstance.c_str(), name.length) == 0) {
    if (rtype == MDNS_RECORDTYPE_SRV || rtype == MDNS_RECORDTYPE_ANY) {
      mdns_record_t additional[6]{};
      size_t n = 0;
      if (st->hasIpv4)
        additional[n++] = st->recordA;
      for (size_t i = 0; i < st->txtCount; ++i)
        additional[n++] = st->txtRecords[i];
      send(st->recordSrv, additional, n);
    }
    return 0;
  }

  // A query for our qualified hostname.
  if (st->hasIpv4 && name.length == st->hostnameQualified.size() &&
      strncmp(name.str, st->hostnameQualified.c_str(), name.length) == 0) {
    if (rtype == MDNS_RECORDTYPE_A || rtype == MDNS_RECORDTYPE_ANY) {
      mdns_record_t additional[6]{};
      size_t n = 0;
      for (size_t i = 0; i < st->txtCount; ++i)
        additional[n++] = st->txtRecords[i];
      send(st->recordA, additional, n);
    }
    return 0;
  }

  return 0;
}

struct MdnsPublisherPrivate
{
  QString name;
  int port = 0;
  QString fingerprint;
  std::atomic<bool> running{false};
  ServiceState state;
  std::vector<int> socks; // one per LAN interface
  alignas(32) char buffer[2048];
};

MdnsPublisher::MdnsPublisher(const QString &name, int port, const QString &fingerprint, QObject *parent)
    : QThread(parent), d(new MdnsPublisherPrivate)
{
  d->name = name;
  d->port = port;
  d->fingerprint = fingerprint;
}

MdnsPublisher::~MdnsPublisher()
{
  stop();
  wait();
  delete d;
}

void MdnsPublisher::stop()
{
  d->running = false;
}

void MdnsPublisher::run()
{
  auto &st = d->state;
  st.port = d->port;

  // Sanitize the computer name into a valid DNS label.
  st.hostname = sanitizeLabel(d->name);
  st.service = kServiceType; // already trailing-dot terminated
  st.serviceInstance = st.hostname + "." + st.service;
  st.hostnameQualified = st.hostname + ".local.";

  auto interfaces = getLocalIpv4Interfaces();
  if (interfaces.empty()) {
    Q_EMIT errorOccurred(QStringLiteral("mDNS: no usable IPv4 interface, service not advertised"));
    return;
  }
  st.hasIpv4 = true;
  st.addressIpv4 = interfaces.front();

  // Open one listening socket per LAN interface on the mDNS port. Passing the
  // interface address makes the mdns library set IP_MULTICAST_IF and join the
  // group on that specific interface, so announces leave the right NIC.
  for (const auto &addr : interfaces) {
    struct sockaddr_in saddr = addr;
    saddr.sin_port = htons(MDNS_PORT);
#ifdef __APPLE__
    saddr.sin_len = sizeof(saddr);
#endif
    int s = mdns_socket_open_ipv4(&saddr);
    if (s < 0)
      continue;
    d->socks.push_back(s);
  }
  if (d->socks.empty()) {
    Q_EMIT errorOccurred(QStringLiteral("mDNS: failed to open service socket (port 5353 in use?)"));
    return;
  }

  // Build records.
  mdns_string_t serviceName{st.service.c_str(), st.service.size()};
  mdns_string_t hostnameStr{st.hostname.c_str(), st.hostname.size()};
  mdns_string_t instanceStr{st.serviceInstance.c_str(), st.serviceInstance.size()};
  mdns_string_t qualifiedStr{st.hostnameQualified.c_str(), st.hostnameQualified.size()};

  st.recordPtr.name = serviceName;
  st.recordPtr.type = MDNS_RECORDTYPE_PTR;
  st.recordPtr.data.ptr.name = instanceStr;
  st.recordPtr.rclass = MDNS_CACHE_FLUSH;
  st.recordPtr.ttl = 4500;

  st.recordSrv.name = instanceStr;
  st.recordSrv.type = MDNS_RECORDTYPE_SRV;
  st.recordSrv.data.srv.name = qualifiedStr;
  st.recordSrv.data.srv.port = static_cast<uint16_t>(st.port);
  st.recordSrv.data.srv.priority = 0;
  st.recordSrv.data.srv.weight = 0;
  st.recordSrv.rclass = MDNS_CACHE_FLUSH;
  st.recordSrv.ttl = 4500;

  st.recordA.name = qualifiedStr;
  st.recordA.type = MDNS_RECORDTYPE_A;
  st.recordA.data.a.addr = st.addressIpv4;
  st.recordA.rclass = MDNS_CACHE_FLUSH;
  st.recordA.ttl = 4500;

  st.txtComputerName = d->name.toStdString();
  st.txtFingerprint = d->fingerprint.toStdString();

  st.txtRecords[0].name = instanceStr;
  st.txtRecords[0].type = MDNS_RECORDTYPE_TXT;
  st.txtRecords[0].data.txt.key = {kTxtComputerName, strlen(kTxtComputerName)};
  st.txtRecords[0].data.txt.value = {st.txtComputerName.c_str(), st.txtComputerName.size()};
  st.txtRecords[0].rclass = MDNS_CACHE_FLUSH;
  st.txtRecords[0].ttl = 4500;
  st.txtCount = 1;

  if (!st.txtFingerprint.empty()) {
    st.txtRecords[1].name = instanceStr;
    st.txtRecords[1].type = MDNS_RECORDTYPE_TXT;
    st.txtRecords[1].data.txt.key = {kTxtFingerprint, strlen(kTxtFingerprint)};
    st.txtRecords[1].data.txt.value = {st.txtFingerprint.c_str(), st.txtFingerprint.size()};
    st.txtRecords[1].rclass = MDNS_CACHE_FLUSH;
    st.txtRecords[1].ttl = 4500;
    st.txtCount = 2;
  }

  // Announce on startup, on every interface socket.
  {
    mdns_record_t additional[6]{};
    size_t n = 0;
    additional[n++] = st.recordSrv;
    additional[n++] = st.recordA;
    for (size_t i = 0; i < st.txtCount; ++i)
      additional[n++] = st.txtRecords[i];
    for (int s : d->socks)
      mdns_announce_multicast(s, d->buffer, sizeof(d->buffer), st.recordPtr, 0, 0, additional, n);
  }

  int maxfd = 0;
  for (int s : d->socks)
    maxfd = (s > maxfd) ? s : maxfd;

  d->running = true;
  while (d->running) {
    fd_set readfs;
    FD_ZERO(&readfs);
    for (int s : d->socks)
      FD_SET(s, &readfs);
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;
    if (select(maxfd + 1, &readfs, nullptr, nullptr, &timeout) >= 0) {
      for (int s : d->socks) {
        if (FD_ISSET(s, &readfs))
          mdns_socket_listen(s, d->buffer, sizeof(d->buffer), &deskflowMdnsPublisherCallback, &st);
      }
    } else {
      break;
    }
  }

  // Goodbye on shutdown, on every interface socket.
  {
    mdns_record_t additional[6]{};
    size_t n = 0;
    additional[n++] = st.recordSrv;
    additional[n++] = st.recordA;
    for (size_t i = 0; i < st.txtCount; ++i)
      additional[n++] = st.txtRecords[i];
    for (int s : d->socks)
      mdns_goodbye_multicast(s, d->buffer, sizeof(d->buffer), st.recordPtr, 0, 0, additional, n);
  }

  for (int s : d->socks)
    mdns_socket_close(s);
  d->socks.clear();
}

} // namespace deskflow::mdns
