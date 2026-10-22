// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <string>

// Shared between the Qt connection thread and the core input thread. Identity is verified TLS SHA256.
class PeerPolicy
{
public:
  static void setLimit(const std::string &peer, int kib)
  {
    std::lock_guard lock(m_mutex);
    if (kib > 0)
      m_limits[peer] = size_t(kib) * 1024;
    else
      m_limits.erase(peer);
  }
  static size_t limit(const std::string &peer, size_t configured = std::numeric_limits<size_t>::max())
  {
    std::lock_guard lock(m_mutex);
    const auto it = m_limits.find(peer);
    return it == m_limits.end() ? configured : std::min(configured, it->second);
  }

private:
  inline static std::mutex m_mutex;
  inline static std::map<std::string, size_t> m_limits;
};
