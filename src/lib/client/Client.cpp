/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016, 2026 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "client/Client.h"
#include "common/UsageInputState.h"
#include "deskflow/UsageControlWire.h"
#include "common/PeerConnectionRegistry.h"
#include <QJsonArray>
#include <QUrl>
#include <QUuid>
#include "common/PeerPolicy.h"

#include "arch/Arch.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "client/ServerProxy.h"
#include "client/ServerProxy1_7.h"
#include "client/ServerProxy1_8.h"
#include "common/NetworkProtocol.h"
#include "common/Settings.h"
#include "deskflow/Clipboard.h"
#include "deskflow/Computer.h"
#include "deskflow/DeskflowException.h"
#include "deskflow/IPlatformComputer.h"
#include "deskflow/PacketStreamFilter.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "deskflow/StreamChunker.h"
#include "deskflow/ipc/CoreIpc.h"
#include "net/IDataSocket.h"
#include "net/ISocketFactory.h"
#include "net/SecureSocket.h"
#include "net/TCPSocket.h"

#include <QMetaEnum>

#include <cstdlib>
#include <cstring>

//
// Client
//

Client::DisconnectRequest::DisconnectRequest(Kind kind, const char *message)
    : m_kind(kind),
      m_message(message != nullptr ? message : "")
{
}

Client::DisconnectRequest::DisconnectRequest(deskflow::core::ConnectionRefusal reason, const char *message)
    : m_kind(Kind::Refuse),
      m_refusalReason(reason),
      m_message(message != nullptr ? message : "")
{
}

Client::Client(
    IEventQueue *events, const std::string &name, const NetworkAddress &address, ISocketFactory *socketFactory,
    deskflow::Computer *computer
)
    : m_name(name),
      m_serverAddress(address),
      m_socketFactory(socketFactory),
      m_computer(computer),
      m_events(events),
      m_useSecureNetwork(Settings::value(Settings::Security::TlsEnabled).toBool())
{
  assert(m_socketFactory != nullptr);
  assert(m_computer != nullptr);

  // register suspend/resume event handlers
  m_events->addHandler(EventTypes::ComputerSuspend, getEventTarget(), [this](const auto &) { handleSuspend(); });
  m_events->addHandler(EventTypes::ComputerResume, getEventTarget(), [this](const auto &) { handleResume(); });
}

Client::~Client()
{
  m_events->removeHandler(EventTypes::ComputerSuspend, getEventTarget());
  m_events->removeHandler(EventTypes::ComputerResume, getEventTarget());

  cleanupTimer();
  cleanupComputer();
  cleanupConnecting();
  cleanupConnection();
  delete m_socketFactory;
}

void Client::setServerAddress(const NetworkAddress &address)
{
  m_serverAddress = address;
}

void Client::connect(size_t addressIndex)
{
  if (m_stream != nullptr) {
    return;
  }
  if (m_suspended) {
    m_connectOnResume = true;
    return;
  }

  auto securityLevel = m_useSecureNetwork ? SecurityLevel::PeerAuth : SecurityLevel::PlainText;

  try {
    // resolve the server hostname.  do this every time we connect
    // in case we couldn't resolve the address earlier or the address
    // has changed (which can happen frequently if this is a laptop
    // being shuttled between various networks).  patch by Brent
    // Priddy.
    m_resolvedAddressesCount = m_serverAddress.resolve(addressIndex);

    // m_serverAddress will be null if the hostname address is not reolved
    if (m_serverAddress.getAddress() != nullptr) {
      // to help users troubleshoot, show server host name (issue: 60)
      LOG_DEBUG(
          "connecting to '%s': %s:%i", m_serverAddress.getHostname().c_str(),
          ARCH->addrToString(m_serverAddress.getAddress()).c_str(), m_serverAddress.getPort()
      );
      ipcSendConnectionState(deskflow::core::ConnectionState::Connecting);
    }

    // create the socket
    IDataSocket *socket = m_socketFactory->create(ARCH->getAddrFamily(m_serverAddress.getAddress()), securityLevel);
    bindNetworkInterface(socket);

    // filter socket messages, including a packetizing filter
    m_stream = new PacketStreamFilter(m_events, socket, true);

    // connect
    LOG_VERBOSE("connecting to server");
    setupConnecting();
    setupTimer();
    socket->connect(m_serverAddress);
  } catch (BaseException &e) {
    cleanupTimer();
    cleanupConnecting();
    cleanupStream();
    LOG_VERBOSE("connection failed");
    sendConnectionFailedEvent(e.what());
    return;
  }
}

void Client::disconnect(const char *msg)
{
  cleanup();

  if (msg) {
    sendConnectionFailedEvent(msg);
  } else {
    sendEvent(EventTypes::ClientDisconnected);
  }
}

void Client::refuseConnection(deskflow::core::ConnectionRefusal reason, const char *msg)
{
  const auto metaEnum = QMetaEnum::fromType<deskflow::core::ConnectionRefusal>();
  ipcSendToClient("connectionRefused", metaEnum.valueToKey(static_cast<int>(reason)));

  cleanup();

  if (msg) {
    auto info = new FailInfo(msg);
    info->m_retry = true;
    Event event(EventTypes::ClientConnectionRefused, getEventTarget(), info, Event::EventFlags::DontFreeData);
    m_events->addEvent(std::move(event));
  }
}

void Client::handshakeComplete()
{
  m_ready = true;
  m_inputSuspended = false;
  m_inputManaged = false;
  m_computer->enable();
  if(UsageInputState::displayOwned())suspendRemoteInput();
  if (m_relativeMouseMoves && !m_hasRelativeRestorePosition) {
    saveRelativeRestorePosition();
  }
  if(m_stream&&m_stream->peerIdentity().size()==64){
    m_usageConnectionId=QUuid::createUuid().toString(QUuid::Id128).toStdString();
    if(m_server&&m_server->supportsInputControl())m_server->requestReceiverInputControl(2);
    publishUsageConnection();
  }
  sendEvent(EventTypes::ClientConnected);
}

bool Client::isConnected() const
{
  return (m_server != nullptr);
}

bool Client::isConnecting() const
{
  return (m_timer != nullptr);
}

NetworkAddress Client::getServerAddress() const
{
  return m_serverAddress;
}

size_t Client::getMaximumClipboardReceiveSizeBytes() const
{
  return PeerPolicy::limit(m_stream ? m_stream->peerIdentity() : std::string(), m_maximumClipboardSize * 1024);
}

void *Client::getEventTarget() const
{
  return m_computer->getEventTarget();
}

bool Client::getClipboard(ClipboardID id, IClipboard *clipboard) const
{
  return m_computer->getClipboard(id, clipboard);
}

void Client::getShape(int32_t &x, int32_t &y, int32_t &w, int32_t &h) const
{
  m_computer->getShape(x, y, w, h);
}

void Client::getCursorPos(int32_t &x, int32_t &y) const
{
  m_computer->getCursorPos(x, y);
}

void Client::enter(int32_t xAbs, int32_t yAbs, uint32_t, KeyModifierMask mask, bool)
{
  if (m_inputSuspended)
    return;
  m_active = true;
  if (m_relativeMouseMoves && m_hasRelativeRestorePosition) {
    xAbs = m_relativeRestoreX;
    yAbs = m_relativeRestoreY;
    LOG_VERBOSE("using relative restore position: %d,%d", xAbs, yAbs);
  }
  m_computer->mouseMove(xAbs, yAbs);
  m_computer->enter(mask);
  publishUsageConnection();
}

bool Client::inputReady() const
{
  return m_ready && m_computer->isEnabled();
}

bool Client::suspendRemoteInput()
{
  m_inputManaged = true;
  m_inputSuspended = true; // Fail closed if the platform cannot release input.
  m_active = false;
  const bool released=m_computer->suspendRemoteInput();
  publishUsageConnection();
  return released;
}

bool Client::resumeRemoteInput()
{
  if (!inputReady()||UsageInputState::displayOwned())
    return false;
  if(m_stream&&m_stream->peerIdentity().size()==64&&!UsageInputState::setPaused(QString::fromStdString(m_stream->peerIdentity()),false))return false;
  m_inputManaged = true;
  m_inputSuspended = false;
  m_active = false; // Resume edge routing, never enter the remote desktop here.
  return true;
}

bool Client::leave()
{
  if (m_relativeMouseMoves) {
    saveRelativeRestorePosition();
  }
  m_active = false;

  m_computer->leave();
  publishUsageConnection();

  if (m_enableClipboard) {
    // send clipboards that we own and that have changed
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
      if (m_ownClipboard[id]) {
        sendClipboard(id);
      }
    }
  }

  return true;
}

void Client::setClipboard(ClipboardID id, const IClipboard *clipboard)
{
  m_computer->setClipboard(id, clipboard);
  m_ownClipboard[id] = false;
  m_sentClipboard[id] = false;
}

void Client::grabClipboard(ClipboardID id)
{
  m_computer->grabClipboard(id);
  m_ownClipboard[id] = false;
  m_sentClipboard[id] = false;
}

void Client::setClipboardDirty(ClipboardID, bool)
{
  assert(0 && "shouldn't be called");
}

void Client::keyDown(KeyID id, KeyModifierMask mask, KeyButton button, const std::string &lang)
{
  if (m_inputSuspended || (m_inputManaged && !m_active))
    return;
  m_computer->keyDown(id, mask, button, lang);
}

void Client::keyRepeat(KeyID id, KeyModifierMask mask, int32_t count, KeyButton button, const std::string &lang)
{
  if (m_inputSuspended || (m_inputManaged && !m_active))
    return;
  m_computer->keyRepeat(id, mask, count, button, lang);
}

void Client::keyUp(KeyID id, KeyModifierMask mask, KeyButton button)
{
  if (m_inputSuspended || (m_inputManaged && !m_active))
    return;
  m_computer->keyUp(id, mask, button);
}

void Client::mouseDown(ButtonID id)
{
  if (m_inputSuspended || (m_inputManaged && !m_active))
    return;
  m_computer->mouseDown(id);
}

void Client::mouseUp(ButtonID id)
{
  if (m_inputSuspended || (m_inputManaged && !m_active))
    return;
  m_computer->mouseUp(id);
}

void Client::mouseMove(int32_t x, int32_t y)
{
  if (m_inputSuspended || (m_inputManaged && !m_active))
    return;
  m_computer->mouseMove(x, y);
}

void Client::mouseRelativeMove(int32_t dx, int32_t dy)
{
  if (m_inputSuspended || (m_inputManaged && !m_active))
    return;
  m_computer->mouseRelativeMove(dx, dy);
}

void Client::mouseWheel(int32_t xDelta, int32_t yDelta)
{
  if (m_inputSuspended || (m_inputManaged && !m_active))
    return;
  m_computer->mouseWheel(xDelta, yDelta);
}

void Client::screensaver(bool activate)
{
  m_computer->screensaver(activate);
}

void Client::resetOptions()
{
  m_relativeMouseMoves = false;
  m_hasRelativeRestorePosition = false;
  m_computer->resetOptions();
}

void Client::setOptions(const OptionsList &options)
{
  if (options.size() % 2 != 0) {
    LOG_ERR("options are the incorrect size, can not process them");
    return;
  }

  for (auto index = options.begin(); index != options.end(); ++index) {
    const OptionID id = *index;
    if (id == kOptionClipboardSharing) {
      index++;
      if (index != options.end()) {
        if (!*index) {
          LOG_INFO("clipboard sharing disabled by server");
        }
        m_enableClipboard = *index;
      }
    } else if (id == kOptionClipboardSharingSize) {
      index++;
      if (index != options.end()) {
        m_maximumClipboardSize = *index;
      }
    } else if (id == kOptionRelativeMouseMoves) {
      index++;
      if (index != options.end()) {
        m_relativeMouseMoves = (*index != 0);
        if (m_relativeMouseMoves && m_ready && !m_hasRelativeRestorePosition) {
          saveRelativeRestorePosition();
        }
      }
    }
  }

  if (m_enableClipboard && !m_maximumClipboardSize) {
    m_enableClipboard = false;
    LOG_INFO("clipboard sharing is disabled because the server set the maximum clipboard size to 0");
  }

  m_computer->setOptions(options);
}

void Client::saveRelativeRestorePosition()
{
  m_computer->getCursorPos(m_relativeRestoreX, m_relativeRestoreY);
  m_hasRelativeRestorePosition = true;
  LOG_VERBOSE("saved relative restore position: %d,%d", m_relativeRestoreX, m_relativeRestoreY);
}

std::string Client::getName() const
{
  return m_name;
}

void Client::sendClipboard(ClipboardID id)
{
  // note -- m_mutex must be locked on entry
  assert(m_computer != nullptr);
  assert(m_server != nullptr);

  // get clipboard data.  set the clipboard time to the last
  // clipboard time before getting the data from the computer
  // as the computer may detect an unchanged clipboard and
  // avoid copying the data.
  Clipboard clipboard;
  if (clipboard.open(m_timeClipboard[id])) {
    clipboard.close();
  }
  m_computer->getClipboard(id, &clipboard);

  // check time
  if (m_timeClipboard[id] == 0 || clipboard.getTime() != m_timeClipboard[id]) {
    // marshall the data
    std::string data = clipboard.marshall();
    if (data.size() >= getMaximumClipboardReceiveSizeBytes()) {
      LOG_WARN("not sending clipboard data, exceeds limit: %zu KB", m_maximumClipboardSize);
      return;
    }

    // save new time
    m_timeClipboard[id] = clipboard.getTime();
    // save and send data if different or not yet sent
    if (!m_sentClipboard[id] || data != m_dataClipboard[id]) {
      m_sentClipboard[id] = true;
      m_dataClipboard[id] = data;
      m_server->onClipboardChanged(id, &clipboard);
    }
  }
}

void Client::sendEvent(EventTypes type)
{
  m_events->addEvent(Event(type, getEventTarget()));
}

void Client::sendConnectionFailedEvent(const char *msg)
{
  auto *info = new FailInfo(msg);
  info->m_retry = true;
  Event event(EventTypes::ClientConnectionFailed, getEventTarget(), info, Event::EventFlags::DontFreeData);
  m_events->addEvent(std::move(event));
}

void Client::setupConnecting()
{
  assert(m_stream != nullptr);

  if (Settings::value(Settings::Security::TlsEnabled).toBool()) {
    m_events->addHandler(EventTypes::DataSocketSecureConnected, m_stream->getEventTarget(), [this](const auto &) {
      handleConnected();
    });
  } else {
    m_events->addHandler(EventTypes::DataSocketConnected, m_stream->getEventTarget(), [this](const auto &) {
      handleConnected();
    });
  }
  m_events->addHandler(EventTypes::DataSocketConnectionFailed, m_stream->getEventTarget(), [this](const auto &e) {
    handleConnectionFailed(e);
  });
}

void Client::setupConnection()
{
  assert(m_stream != nullptr);

  m_events->addHandler(EventTypes::ClientDisconnectRequested, m_stream->getEventTarget(), [this](const auto &e) {
    handleDisconnectRequested(e);
  });
  m_events->addHandler(EventTypes::SocketDisconnected, m_stream->getEventTarget(), [this](const auto &) {
    handleDisconnected();
  });
  m_events->addHandler(EventTypes::StreamInputReady, m_stream->getEventTarget(), [this](const auto &) {
    handleHello();
  });
  m_events->addHandler(EventTypes::StreamOutputError, m_stream->getEventTarget(), [this](const auto &) {
    handleOutputError();
  });
  m_events->addHandler(EventTypes::StreamInputShutdown, m_stream->getEventTarget(), [this](const auto &) {
    handleDisconnected();
  });
  m_events->addHandler(EventTypes::StreamOutputShutdown, m_stream->getEventTarget(), [this](const auto &) {
    handleDisconnected();
  });
}

bool Client::setupComputer(int16_t protocolMinor)
{
  assert(m_server == nullptr);

  m_ready = false;

  // only 1.6 and later have a proxy: the clipboard, mouse wheel and key message formats
  // differ below that, and nothing older (synergy 1.4 and earlier) still needs supporting.
  // a version with no case is refused by the hello handler.
  switch (protocolMinor) {
  case 6:
    m_server = new ServerProxy(this, m_stream, m_events);
    break;
  case 7:
    m_server = new ServerProxy1_7(this, m_stream, m_events);
    break;
  case 8:
    m_server = new ServerProxy1_8(this, m_stream, m_events);
    break;
  default:
    break;
  }

  if (m_server != nullptr) {
    m_events->addHandler(EventTypes::ComputerShapeChanged, getEventTarget(), [this](const auto &) {
      handleShapeChanged();
    });
    m_events->addHandler(EventTypes::ClipboardGrabbed, getEventTarget(), [this](const auto &e) {
      handleClipboardGrabbed(e);
    });
  }
  return m_server != nullptr;
}

void Client::setupTimer()
{
  assert(m_timer == nullptr);
  m_timer = m_events->newOneShotTimer(2.0, nullptr);
  m_events->addHandler(EventTypes::Timer, m_timer, [this](const auto &) { handleConnectTimeout(); });
}

void Client::cleanup()
{
  m_connectOnResume = false;
  cleanupTimer();
  cleanupComputer();
  cleanupConnecting();
  cleanupConnection();
}

void Client::cleanupConnecting()
{
  if (m_stream != nullptr) {
    m_events->removeHandler(EventTypes::DataSocketConnected, m_stream->getEventTarget());
    m_events->removeHandler(EventTypes::DataSocketSecureConnected, m_stream->getEventTarget());
    m_events->removeHandler(EventTypes::DataSocketConnectionFailed, m_stream->getEventTarget());
  }
}

void Client::cleanupConnection()
{
  if (m_stream != nullptr) {
    using enum EventTypes;
    m_events->removeHandler(StreamInputReady, m_stream->getEventTarget());
    m_events->removeHandler(StreamOutputError, m_stream->getEventTarget());
    m_events->removeHandler(StreamInputShutdown, m_stream->getEventTarget());
    m_events->removeHandler(StreamOutputShutdown, m_stream->getEventTarget());
    m_events->removeHandler(SocketDisconnected, m_stream->getEventTarget());
    m_events->removeHandler(ClientDisconnectRequested, m_stream->getEventTarget());
    cleanupStream();
  }
}

void Client::cleanupComputer()
{
  const auto retiredConnection=m_usageConnectionId;
  const bool releaseVerified=!retiredConnection.empty()&&m_ready&&suspendRemoteInput();
  if (m_server != nullptr) {
    if (m_ready) {
      m_computer->disable();
      m_ready = false;
    }
    m_events->removeHandler(EventTypes::ComputerShapeChanged, getEventTarget());
    m_events->removeHandler(EventTypes::ClipboardGrabbed, getEventTarget());
    delete m_server;
    m_server = nullptr;
  }
  if(!retiredConnection.empty())PeerConnectionRegistry::remove(QString::fromStdString(retiredConnection),releaseVerified);
  m_usageConnectionId.clear();
}

void Client::cleanupTimer()
{
  if (m_timer != nullptr) {
    m_events->removeHandler(EventTypes::Timer, m_timer);
    m_events->deleteTimer(m_timer);
    m_timer = nullptr;
  }
}

void Client::cleanupStream()
{
  delete m_stream;
  m_stream = nullptr;
}

void Client::handleConnected()
{
  LOG_VERBOSE("connected, waiting for hello");
  cleanupConnecting();
  setupConnection();

  // reset clipboard state
  for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
    m_ownClipboard[id] = false;
    m_sentClipboard[id] = false;
    m_timeClipboard[id] = 0;
  }
}

void Client::handleConnectionFailed(const Event &event)
{
  auto *info = static_cast<IDataSocket::ConnectionFailedInfo *>(event.getData());

  cleanupTimer();
  cleanupConnecting();
  cleanupStream();
  LOG_VERBOSE("connection failed");
  sendConnectionFailedEvent(info->m_what.c_str());
  delete info;
}

void Client::handleConnectTimeout()
{
  cleanupTimer();
  cleanupConnecting();
  cleanupConnection();
  cleanupStream();
  LOG_VERBOSE("connection timed out");
  sendConnectionFailedEvent("Timed out");
}

void Client::handleOutputError()
{
  cleanupTimer();
  cleanupComputer();
  cleanupConnection();
  LOG_WARN("error sending to server");
  sendEvent(EventTypes::ClientDisconnected);
}

void Client::handleDisconnected()
{
  cleanupTimer();
  cleanupComputer();
  cleanupConnection();
  LOG_VERBOSE("disconnected");
  sendEvent(EventTypes::ClientDisconnected);
}

void Client::handleDisconnectRequested(const Event &event)
{
  const auto *request = static_cast<const DisconnectRequest *>(event.getDataObject());
  if (request == nullptr) {
    disconnect(nullptr);
    return;
  }

  if (request->kind() == DisconnectRequest::Kind::Refuse) {
    refuseConnection(request->refusalReason(), request->message());
  } else {
    disconnect(request->message());
  }
}

void Client::handleShapeChanged()
{
  LOG_DEBUG("resolution changed");
  m_server->onInfoChanged();
}

void Client::handleClipboardGrabbed(const Event &event)
{
  if (!m_enableClipboard || (m_maximumClipboardSize == 0)) {
    return;
  }

  const auto *info = static_cast<const IComputer::ClipboardInfo *>(event.getData());

  // grab ownership
  m_server->onGrabClipboard(info->m_id);

  // we now own the clipboard and it has not been sent to the server
  m_ownClipboard[info->m_id] = true;
  m_sentClipboard[info->m_id] = false;
  m_timeClipboard[info->m_id] = 0;

  // if we're not the active computer then send the clipboard now,
  // otherwise we'll wait until we leave.
  if (!m_active) {
    sendClipboard(info->m_id);
  }
}

void Client::handleHello()
{
  int16_t serverMajor;
  int16_t serverMinor;

  // as luck would have it, both "Synergy" and "Barrier" are 7 chars,
  // so we eat 7 chars and then test for either protocol name.
  // we cannot re-use `readf` to check for various hello messages,
  // as `readf` eats bytes (advances the stream position reference).
  std::string protocolName;
  ProtocolUtil::readf(m_stream, kMsgHello, &protocolName, &serverMajor, &serverMinor);

  if (const auto proto = networkProtocolFromString(QString::fromStdString(protocolName));
      proto == NetworkProtocol::Unknown) {
    LOG_WARN("hello back received with protocol: '%s'", protocolName.c_str());
    sendConnectionFailedEvent("got invalid hello message from server");
    cleanupTimer();
    cleanupConnection();
    return;
  }

  if (serverMajor != kProtocolMajorVersion) {
    LOG_WARN("server protocol version not compatible: %d.%d", serverMajor, serverMinor);
    sendConnectionFailedEvent(IncompatibleClientException(serverMajor, serverMinor).what());
    cleanupTimer();
    cleanupConnection();
    return;
  }

  int16_t helloBackMinor = kProtocolMinorVersion;
  if (serverMinor < kProtocolMinorVersion) {
    helloBackMinor = serverMinor;
    LOG_INFO(
        "downgrading client protocol version from %d.%d to %d.%d", //
        kProtocolMajorVersion, kProtocolMinorVersion, kProtocolMajorVersion, helloBackMinor
    );
  }

  // no proxy speaks the negotiated version, so hang up as incompatible rather than
  // talk a version the client does not implement, the same rule the server applies
  // when it picks a client proxy
  if (!setupComputer(helloBackMinor)) {
    LOG_WARN("server protocol version not supported: %d.%d", serverMajor, serverMinor);
    sendConnectionFailedEvent(IncompatibleClientException(serverMajor, serverMinor).what());
    cleanupTimer();
    cleanupConnection();
    return;
  }

  LOG_DEBUG("saying hello back with version %s %d.%d", protocolName.c_str(), kProtocolMajorVersion, helloBackMinor);

  // dynamically build write format for hello back since `ProtocolUtil::writef`
  // doesn't support formatting fixed length strings yet.
  std::string helloBackMessage = protocolName + kMsgHelloBackArgs;
  ProtocolUtil::writef(m_stream, helloBackMessage.c_str(), kProtocolMajorVersion, helloBackMinor, &m_name);

  // now connected but waiting to complete handshake
  cleanupTimer();

  // make sure we process any remaining messages later.  we won't
  // receive another event for already pending messages so we fake
  // one.
  if (m_stream->isReady()) {
    m_events->addEvent(Event(EventTypes::StreamInputReady, m_stream->getEventTarget()));
  }
}

void Client::handleSuspend()
{
  if (!m_suspended) {
    LOG_INFO("suspend");
    m_suspended = true;
    ipcSendToClient("transportPower", "suspend");
    bool wasConnected = isConnected();
    disconnect(nullptr);
    m_connectOnResume = wasConnected;
  }
}

void Client::handleResume()
{
  if (m_suspended) {
    LOG_INFO("resume");
    m_suspended = false;
    ipcSendToClient("transportPower", "resume");
    if (m_connectOnResume) {
      m_connectOnResume = false;
      connect();
    }
  }
}

void Client::bindNetworkInterface(IDataSocket *socket) const
{
  if (m_localTransport)
    return;
  const auto address = Settings::value(Settings::Core::Interface).toString();
  if (address.isEmpty())
    return;

  LOG_DEBUG("bind to network interface: %s", qPrintable(address));

  NetworkAddress bindAddress(address.toStdString());
  bindAddress.resolve();

  socket->bind(bindAddress);
}

void Client::publishUsageConnection()
{
  if(!m_ready||!m_stream||m_usageConnectionId.empty())return;
  const auto peer=m_stream->peerIdentity();if(peer.size()!=64)return;
  const auto remote=m_usageRemoteAddress;
  QUrl endpoint("tcp://"+remote);QString host=endpoint.host().toLower();int port=endpoint.port(Settings::value(Settings::Core::Port).toInt());
  if(port<=0)port=24800;
  if(host.contains(':'))host="["+host+"]";
  QJsonArray keys{QString::fromStdString("peer:"+peer)};
  if(!host.isEmpty())keys.append("server:"+host+":"+QString::number(port));
  const bool ready=inputReady()&&m_server&&m_server->sourceInputReady();
  PeerConnectionRegistry::update({{"peerId",QString::fromStdString(peer)},{"peerName",remote},
    {"baseConnectionId",QString::fromStdString(m_usageConnectionId)},{"baseConnected",true},{"originalInputRole","receiver"},
    {"inputActive",ready&&m_server->sourceRouteActive()&&!m_inputSuspended},{"inputPermissionsReady",ready},{"inputControlSupported",m_server&&m_server->supportsInputControl()},
    {"inputFocusOwner",m_active?QString::fromStdString(peer):"local"},{"deviceKeys",keys}});
}
InputRouteStatus Client::inputRouteStatus(const std::string& peer,uint32_t sequence,bool enabled) const
{
  InputRouteStatus status;if(!m_stream||peer.size()!=64||m_stream->peerIdentity()!=peer||!m_server)return status;
  status.connected=m_ready;status.supported=m_server->supportsInputControl();status.suspended=m_inputSuspended;
  status.localFocus=!m_active;status.lastSequence=m_receiverCoreSequence;status.otherRoutesIsolated=true;
  const bool matches=sequence!=0&&sequence==m_receiverCoreSequence;
  status.responseReceived=matches&&m_server->receiverControlResponded();
  status.permissionsReady=matches&&status.responseReceived&&m_server->receiverControlMode()==2&&m_server->receiverControlResult()==3&&inputReady();
  const auto mode=m_server->receiverControlMode(),result=m_server->receiverControlResult();
  status.acknowledged=matches&&status.responseReceived&&(enabled?((mode==1&&result==2)||(mode==3&&result==4)):
    mode==0&&result==1&&m_receiverReleaseVerified);
  // Resume preparation is still suspended until commit arrives at both ends.
  if(mode==1)status.suspended=true;
  return status;
}
bool Client::requestInputRoute(const std::string& peer,uint32_t sequence,bool enabled)
{
  const auto status=inputRouteStatus(peer,sequence,enabled);
  if(!status.connected||!status.supported||!sequence||sequence<=m_receiverCoreSequence)return false;
  m_receiverCoreSequence=sequence;
  if(!enabled){m_receiverReleaseVerified=suspendRemoteInput();publishUsageConnection();}
  return m_server->requestReceiverInputControl(enabled?1:0);
}
bool Client::requestInputProbe(const std::string& peer,uint32_t sequence)
{
  const auto status=inputRouteStatus(peer,sequence,false);
  if(!status.connected||!status.supported||!sequence||sequence<=m_receiverCoreSequence)return false;
  m_receiverCoreSequence=sequence;return m_server->requestReceiverInputControl(2);
}
bool Client::commitInputResume(const std::string& peer,uint32_t sequence)
{
  const auto status=inputRouteStatus(peer,sequence,true);
  if(!status.acknowledged||m_server->receiverControlMode()!=1||!resumeRemoteInput())return false;
  return m_server->requestReceiverInputControl(3);
}

bool Client::sendUsageControl(const QString& peer,const QString& connection,const QJsonObject& body)
{
  if(!m_ready||!m_stream||!m_server||!m_server->supportsInputControl()||
    peer.toStdString()!=m_stream->peerIdentity()||connection.toStdString()!=m_usageConnectionId)return false;
  return deskflow::input::writeUsageControl(m_stream,"DUSG%s",body);
}
void Client::receiveUsageControl(const QJsonObject& body)
{
  if(m_ready&&m_stream&&!m_usageConnectionId.empty())
    PeerConnectionRegistry::controlReceived(QString::fromStdString(m_stream->peerIdentity()),QString::fromStdString(m_usageConnectionId),body);
}

bool Client::matchesUsageConnection(const QString& peer,const QString& connection) const
{
  return m_ready&&m_stream&&peer.toStdString()==m_stream->peerIdentity()&&connection.toStdString()==m_usageConnectionId;
}
