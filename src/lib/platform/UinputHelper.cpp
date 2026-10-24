/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
// Privileged device factory. No network, settings, input data, or caller-supplied
// paths are read. Pass only the newly created devices to the caller, then exit.
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
void configure(int fd, unsigned long request, int value)
{
  if (ioctl(fd, request, value) < 0)
    throw std::runtime_error("uinput device configuration failed");
}

int createDevice(int kind)
{
  const int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    throw std::runtime_error("cannot open /dev/uinput (load the uinput module)");
  try {
    configure(fd, UI_SET_EVBIT, EV_KEY);
    if (kind == 0) {
      for (int code = KEY_ESC; code <= KEY_MAX; ++code)
        if (code < BTN_MISC || code >= KEY_OK)
          configure(fd, UI_SET_KEYBIT, code);
      configure(fd, UI_SET_EVBIT, EV_REP);
    } else {
      for (int code = BTN_LEFT; code <= BTN_TASK; ++code)
        configure(fd, UI_SET_KEYBIT, code);
      configure(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
      configure(fd, UI_SET_EVBIT, EV_REL);
      if (kind == 1) {
        for (const auto axis : {REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES})
          configure(fd, UI_SET_RELBIT, axis);
        configure(fd, UI_SET_EVBIT, EV_ABS);
        for (const auto axis : {ABS_X, ABS_Y}) {
          configure(fd, UI_SET_ABSBIT, axis);
          uinput_abs_setup abs{};
          abs.code = axis;
          abs.absinfo.maximum = 65535;
          abs.absinfo.resolution = 100;
          if (ioctl(fd, UI_ABS_SETUP, &abs) < 0)
            throw std::runtime_error("uinput absolute axis configuration failed");
        }
      } else {
        configure(fd, UI_SET_RELBIT, REL_X);
        configure(fd, UI_SET_RELBIT, REL_Y);
      }
    }
    uinput_setup setup{};
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x1209;
    setup.id.product = 0xb001 + kind;
    const char *names[] = {"BMouse Virtual Keyboard", "BMouse Virtual Absolute Mouse", "BMouse Virtual Relative Mouse"};
    std::snprintf(setup.name, sizeof(setup.name), "%s", names[kind]);
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0)
      throw std::runtime_error("uinput device creation failed");
    return fd;
  } catch (...) {
    close(fd);
    throw;
  }
}
} // namespace

int main(int argc, char **argv)
{
  if (argc != 2)
    return 2;
  std::array<int, 3> fds{-1, -1, -1};
  int connection = -1;
  int status = 0;
  try {
    const auto originalUser = std::getenv("PKEXEC_UID");
    const auto uid = originalUser ? std::strtoul(originalUser, nullptr, 10) : getuid();
    const std::string name = argv[1];
    const auto prefix = "busymouse-uinput-" + std::to_string(uid) + "-";
    sockaddr_un address{};
    if (!name.starts_with(prefix) || name.size() >= sizeof(address.sun_path) - 1)
      throw std::runtime_error("invalid uinput helper socket name");
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path + 1, name.data(), name.size());
    connection = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (connection < 0 ||
        connect(connection, reinterpret_cast<sockaddr *>(&address), offsetof(sockaddr_un, sun_path) + 1 + name.size()) <
            0)
      throw std::runtime_error("cannot connect to uinput receiver");
    ucred peer{};
    socklen_t size = sizeof(peer);
    if (getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &peer, &size) < 0 || peer.uid != uid)
      throw std::runtime_error("uinput receiver identity does not match authorization");
    for (int kind = 0; kind < 3; ++kind)
      fds[kind] = createDevice(kind);
    char result = 'U';
    iovec io{&result, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 3)]{};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    const auto header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int) * 3);
    std::memcpy(CMSG_DATA(header), fds.data(), sizeof(int) * 3);
    if (sendmsg(connection, &message, MSG_NOSIGNAL) != 1)
      throw std::runtime_error("uinput: helper must be launched by BMouse with a device socket");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    status = 1;
  }
  for (const auto fd : fds)
    if (fd >= 0)
      close(fd);
  if (connection >= 0)
    close(connection);
  return status;
}
