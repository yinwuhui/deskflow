// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

// Same-process projection of an authenticated transport onto its live loopback
// socket. Neither an IP address nor a GUI-supplied fingerprint creates a binding.
class LocalTransportIdentity
{
public:
  static bool bind(uint16_t corePort,uint16_t proxyPort,const std::string& owner,const std::string& peer)
  {
    if(!corePort||!proxyPort||owner.empty()||peer.size()!=64)return false;
    for(auto c:peer)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
    std::lock_guard lock(m_mutex);auto key=std::pair{corePort,proxyPort};
    auto found=m_bindings.find(key);
    if(found!=m_bindings.end())return found->second.owner==owner&&found->second.peer==peer;
    if(m_bindings.size()>=1024)return false;
    m_bindings.emplace(key,Binding{owner,peer});return true;
  }
  static std::string lookup(uint16_t corePort,uint16_t proxyPort)
  {
    std::lock_guard lock(m_mutex);auto it=m_bindings.find({corePort,proxyPort});
    return it==m_bindings.end()?std::string{}:it->second.peer;
  }
  static void release(const std::string& owner)
  {
    std::lock_guard lock(m_mutex);
    for(auto it=m_bindings.begin();it!=m_bindings.end();)if(it->second.owner==owner)it=m_bindings.erase(it);else ++it;
  }
private:
  struct Binding {std::string owner,peer;};
  inline static std::mutex m_mutex;
  inline static std::map<std::pair<uint16_t,uint16_t>,Binding> m_bindings;
};
