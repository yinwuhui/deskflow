/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#include "UinputDeviceTests.h"
#include "platform/UinputDevice.h"

#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace {
struct Devices
{
  std::array<int, 3> readers;
  std::array<int, 3> writers;
  Devices()
  {
    for (int i = 0; i < 3; ++i) {
      int pair[2];
      if (pipe2(pair, O_NONBLOCK | O_CLOEXEC) != 0)
        qFatal("pipe failed");
      readers[i] = pair[0];
      writers[i] = pair[1];
    }
  }
  ~Devices()
  {
    for (auto fd : readers)
      close(fd);
  }
  std::vector<input_event> read(int device)
  {
    std::vector<input_event> events;
    input_event event;
    while (::read(readers[device], &event, sizeof(event)) == sizeof(event))
      events.push_back(event);
    return events;
  }
};
} // namespace

void UinputDeviceTests::releasesHeldKeysAndButtons()
{
  Devices pipes;
  {
    deskflow::UinputDevice device(pipes.writers);
    device.key(KEY_LEFTSHIFT, true);
    device.key(KEY_LEFTSHIFT, true); // Duplicate downs must not leave a stuck modifier.
    device.key(KEY_A, true);
    device.button(BTN_LEFT, true);
    device.key(KEY_MAX + 1, true);
    device.key(BTN_LEFT, true);
    auto keys = pipes.read(0);
    QCOMPARE(keys.size(), size_t(4));
    QCOMPARE(keys[0].code, KEY_LEFTSHIFT);
    QCOMPARE(keys[0].value, 1);
    pipes.read(1);
    // Destructor must release both modifiers and buttons, even without key-up packets.
  }
  auto keys = pipes.read(0);
  QCOMPARE(keys.size(), size_t(3));
  QCOMPARE(keys[0].code, KEY_A);
  QCOMPARE(keys[0].value, 0);
  QCOMPARE(keys[1].code, KEY_LEFTSHIFT);
  QCOMPARE(keys[1].value, 0);
  auto buttons = pipes.read(1);
  QCOMPARE(buttons.size(), size_t(2));
  QCOMPARE(buttons[0].code, BTN_LEFT);
  QCOMPARE(buttons[0].value, 0);
}

void UinputDeviceTests::mapsAbsoluteCoordinatesAndClamps()
{
  Devices pipes;
  deskflow::UinputDevice device(pipes.writers);
  device.absolute(100, 200, 100, 200, 1920, 1080);
  device.absolute(9999, -9999, 100, 200, 1920, 1080);
  auto events = pipes.read(1);
  QCOMPARE(events.size(), size_t(8));
  QCOMPARE(events[2].type, EV_ABS);
  QCOMPARE(events[2].value, 0);
  QCOMPARE(events[3].value, 0);
  QCOMPARE(events[5].value, 65535);
  QCOMPARE(events[6].value, 0);
}

void UinputDeviceTests::preservesFractionalWheelMotion()
{
  Devices pipes;
  deskflow::UinputDevice device(pipes.writers);
  device.wheel(60, -60);
  device.wheel(60, -60);
  auto events = pipes.read(1);
  int horizontal = 0, vertical = 0;
  for (const auto &event : events) {
    if (event.type == EV_REL && event.code == REL_HWHEEL)
      horizontal += event.value;
    if (event.type == EV_REL && event.code == REL_WHEEL)
      vertical += event.value;
  }
  QCOMPARE(horizontal, 1);
  QCOMPARE(vertical, -1);
  device.wheel(60, -60);
  pipes.read(1);
  device.releaseAll();
  pipes.read(1);
  device.wheel(60, -60);
  for (const auto &event : pipes.read(1)) {
    QVERIFY(event.code != REL_WHEEL || event.type != EV_REL);
    QVERIFY(event.code != REL_HWHEEL || event.type != EV_REL);
  }
}

void UinputDeviceTests::reentryRefreshesCoordinates()
{
  Devices pipes;
  deskflow::UinputDevice device(pipes.writers);
  device.absolute(123, 10, 0, 0, 1000, 1000);
  const auto first = pipes.read(1);
  device.releaseAll();
  pipes.read(1);
  device.absolute(123, 10, 0, 0, 1000, 1000);
  const auto second = pipes.read(1);
  QCOMPARE(second.size(), size_t(5));
  QCOMPARE(second[0].value, second[2].value + 1);
  QCOMPARE(second[2].value, first[2].value);
  QCOMPARE(second[3].value, first[3].value);
}

void UinputDeviceTests::relativeMotionUsesSeparateDevice()
{
  Devices pipes;
  deskflow::UinputDevice device(pipes.writers);
  device.relative(3, -4);
  QCOMPARE(pipes.read(1).size(), size_t(0));
  const auto events = pipes.read(2);
  QCOMPARE(events.size(), size_t(3));
  QCOMPARE(events[0].code, REL_X);
  QCOMPARE(events[0].value, 3);
  QCOMPARE(events[1].code, REL_Y);
  QCOMPARE(events[1].value, -4);
}

QTEST_MAIN(UinputDeviceTests)
