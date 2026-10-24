/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "platform/UinputDevice.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace {
std::array<int, 3> acquireDevices()
{
  // An abstract Unix socket keeps polkit's standard streams available for its
  // authentication agent. Only the exact privileged child may hand us devices.
  const int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (listener < 0)
    throw std::runtime_error("uinput: cannot create helper socket");
  std::array<unsigned char, 16> nonce{};
  if (getrandom(nonce.data(), nonce.size(), 0) != nonce.size()) {
    close(listener);
    throw std::runtime_error("uinput: cannot create socket nonce");
  }
  std::string name = "busymouse-uinput-" + std::to_string(getuid()) + "-";
  constexpr char hex[] = "0123456789abcdef";
  for (const auto byte : nonce) {
    name += hex[byte >> 4];
    name += hex[byte & 15];
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path + 1, name.data(), name.size());
  const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
  if (bind(listener, reinterpret_cast<sockaddr *>(&address), length) < 0 || listen(listener, 4) < 0) {
    close(listener);
    throw std::runtime_error("uinput: cannot bind helper socket");
  }

  char executable[] = "/usr/bin/pkexec";
  char helper[] = BMOUSE_UINPUT_HELPER_PATH;
  char *args[] = {executable, helper, name.data(), nullptr};
  pid_t pid;
  const int rc = posix_spawn(&pid, executable, nullptr, nullptr, args, environ);
  if (rc != 0) {
    close(listener);
    throw std::runtime_error("uinput: cannot start pkexec; install polkit and the uinput helper");
  }

  int connection = -1;
  bool reaped = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd ready{listener, POLLIN, 0};
    if (poll(&ready, 1, 100) > 0 && (ready.revents & POLLIN)) {
      const int candidate = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (candidate >= 0) {
        ucred peer{};
        socklen_t size = sizeof(peer);
        if (getsockopt(candidate, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 && peer.uid == 0 && peer.pid == pid) {
          connection = candidate;
          break;
        }
        close(candidate);
      }
    }
    if (waitpid(pid, nullptr, WNOHANG) == pid) {
      reaped = true;
      break;
    }
  }
  close(listener);

  std::array<int, 3> fds{-1, -1, -1};
  char result = 0;
  iovec io{&result, 1};
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 3)]{};
  msghdr message{};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  ssize_t count = -1;
  if (connection >= 0) {
    pollfd ready{connection, POLLIN, 0};
    if (poll(&ready, 1, 3000) > 0)
      count = recvmsg(connection, &message, MSG_CMSG_CLOEXEC);
    close(connection);
  }
  const auto header = count > 0 ? CMSG_FIRSTHDR(&message) : nullptr;
  const bool valid = header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS &&
                     header->cmsg_len == CMSG_LEN(sizeof(int) * 3) && !(message.msg_flags & MSG_CTRUNC) &&
                     result == 'U';
  if (header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
    const auto bytes = std::min<std::size_t>(header->cmsg_len - CMSG_LEN(0), sizeof(int) * 3);
    std::memcpy(fds.data(), CMSG_DATA(header), bytes);
  }
  if (!reaped) {
    if (!valid)
      kill(pid, SIGTERM);
    // pkexec may already be privileged and unable to receive our SIGTERM.
    // Never block the input thread waiting for an authentication dialog.
    std::thread([pid] {
      while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
      }
    }).detach();
  }
  if (!valid) {
    for (const auto fd : fds)
      if (fd >= 0)
        close(fd);
    throw std::runtime_error(
        "uinput: helper unavailable or authorization cancelled. Install busymouse-uinput-helper and restart unlocked."
    );
  }
  return fds;
}
} // namespace

namespace deskflow {
UinputDevice::UinputDevice() : UinputDevice(acquireDevices())
{
}

UinputDevice::UinputDevice(std::array<int, 3> fds) : m_fds(fds)
{
}

UinputDevice::~UinputDevice()
{
  releaseAll();
  for (const auto fd : m_fds)
    close(fd); // The kernel destroys the virtual device on the last close, including after a crash.
}

void UinputDevice::writeEvent(int device, std::uint16_t type, std::uint16_t code, int value)
{
  input_event event{};
  event.type = type;
  event.code = code;
  event.value = value;
  ssize_t count;
  do {
    count = write(m_fds[device], &event, sizeof(event));
  } while (count < 0 && errno == EINTR);
  if (count != sizeof(event))
    throw std::runtime_error("uinput: failed to write input event");
}

void UinputDevice::sync(int device)
{
  writeEvent(device, EV_SYN, SYN_REPORT, 0);
}

void UinputDevice::key(std::uint32_t code, bool down)
{
  if (code == KEY_RESERVED || code > KEY_MAX || (code >= BTN_MISC && code < KEY_OK))
    return;
  if (m_keys.test(code) == down)
    return; // The kernel handles autorepeat; don't send repeated presses as new keys.
  writeEvent(0, EV_KEY, code, down ? 1 : 0);
  m_keys.set(code, down);
  sync(0);
}

void UinputDevice::button(std::uint32_t code, bool down)
{
  if (code < BTN_LEFT || code > BTN_TASK || m_buttons.test(code) == down)
    return;
  writeEvent(1, EV_KEY, code, down ? 1 : 0);
  m_buttons.set(code, down);
  sync(1);
}

void UinputDevice::absolute(int x, int y, int originX, int originY, int width, int height)
{
  if (width <= 1 || height <= 1)
    return;
  const auto scale = [](int value, int origin, int extent) {
    return static_cast<int>(
        std::clamp<std::int64_t>(std::int64_t(value) - origin, 0, extent - 1) * 65535 / (extent - 1)
    );
  };
  const auto absoluteX = scale(x, originX, width);
  const auto absoluteY = scale(y, originY, height);
  if (!m_absolutePositionKnown) {
    // evdev suppresses unchanged coordinates. On re-entry the local mouse may
    // have moved the cursor, so force a fresh absolute position before landing.
    writeEvent(1, EV_ABS, ABS_X, absoluteX == 65535 ? 65534 : absoluteX + 1);
    sync(1);
    m_absolutePositionKnown = true;
  }
  writeEvent(1, EV_ABS, ABS_X, absoluteX);
  writeEvent(1, EV_ABS, ABS_Y, absoluteY);
  sync(1);
}

void UinputDevice::relative(int x, int y)
{
  writeEvent(2, EV_REL, REL_X, x);
  writeEvent(2, EV_REL, REL_Y, y);
  sync(2);
}

void UinputDevice::wheel(int x, int y)
{
  // Protocol scroll units are 1/120 of a detent. Preserve fractions and also
  // emit the legacy axes for consumers without high-resolution wheel support.
  writeEvent(1, EV_REL, REL_HWHEEL_HI_RES, x);
  writeEvent(1, EV_REL, REL_WHEEL_HI_RES, y);
  m_wheelX += x;
  m_wheelY += y;
  if (m_wheelX / 120)
    writeEvent(1, EV_REL, REL_HWHEEL, m_wheelX / 120);
  if (m_wheelY / 120)
    writeEvent(1, EV_REL, REL_WHEEL, m_wheelY / 120);
  m_wheelX %= 120;
  m_wheelY %= 120;
  sync(1);
}

void UinputDevice::releaseAll() noexcept
{
  try {
    for (unsigned code = 0; code < KEY_CNT; ++code) {
      if (m_keys.test(code))
        writeEvent(0, EV_KEY, code, 0);
      if (m_buttons.test(code))
        writeEvent(1, EV_KEY, code, 0);
    }
    sync(0);
    sync(1);
  } catch (...) {
    // Destruction must still close every descriptor if a device has failed.
  }
  m_keys.reset();
  m_buttons.reset();
  m_wheelX = m_wheelY = 0;
  m_absolutePositionKnown = false;
}
} // namespace deskflow
