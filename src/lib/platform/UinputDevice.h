/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <linux/input.h>

namespace deskflow {

// Owns only the three virtual devices created by the privileged helper.
// Neither the core nor GUI needs access to physical input devices or root.
class UinputDevice
{
public:
  UinputDevice();
  explicit UinputDevice(std::array<int, 3> fds); // Takes ownership; also permits pipe-based tests.
  ~UinputDevice();
  UinputDevice(const UinputDevice &) = delete;
  UinputDevice &operator=(const UinputDevice &) = delete;

  void key(std::uint32_t code, bool down);
  void button(std::uint32_t code, bool down);
  void absolute(int x, int y, int originX, int originY, int width, int height);
  void relative(int x, int y);
  void wheel(int x, int y);
  void releaseAll() noexcept;

private:
  void writeEvent(int device, std::uint16_t type, std::uint16_t code, int value);
  void sync(int device);
  std::array<int, 3> m_fds;
  std::bitset<KEY_CNT> m_keys;
  std::bitset<KEY_CNT> m_buttons;
  int m_wheelX = 0;
  int m_wheelY = 0;
  bool m_absolutePositionKnown = false;
};
} // namespace deskflow
