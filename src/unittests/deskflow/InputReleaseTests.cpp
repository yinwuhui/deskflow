// SPDX-License-Identifier: MIT
#include "MockEventQueue.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "common/Settings.h"
#include "deskflow/Computer.h"
#include "deskflow/PlatformComputer.h"
#include <QTemporaryDir>
#include <QtTest>
class RecordingPlatform : public PlatformComputer
{
public:
  RecordingPlatform(IEventQueue *events, bool primary = false) : PlatformComputer(events), primary(primary)
  {
  }
  bool primary, allowLeave = true;
  QList<QPair<int, bool>> buttons;
  int releases = 0;
  bool isPrimary() const override
  {
    return primary;
  }
  bool canLeave() override
  {
    return allowLeave;
  }
  void fakeMouseButton(ButtonID id, bool press) override
  {
    buttons.append({int(id), press});
  }
  void fakeAllKeysUp() override
  {
    ++releases;
  }
  void updateKeyMap() override
  {
  }
  void updateKeyState() override
  {
  }
  void setHalfDuplexMask(KeyModifierMask) override
  {
  }
  bool fakeMediaKey(KeyID) override
  {
    return false;
  }
  std::string getSecureInputApp() const override
  {
    return {};
  }
  void *getEventTarget() const override
  {
    return nullptr;
  }
  bool getClipboard(ClipboardID id, IClipboard *) const override
  {
    return {};
  }
  void getShape(int32_t &x, int32_t &y, int32_t &width, int32_t &height) const override
  {
  }
  void getCursorPos(int32_t &x, int32_t &y) const override
  {
  }
  void reconfigure(uint32_t activeSides) override
  {
  }
  uint32_t activeSides() override
  {
    return {};
  }
  void warpCursor(int32_t x, int32_t y) override
  {
  }
  uint32_t registerHotKey(KeyID key, KeyModifierMask mask) override
  {
    return {};
  }
  void unregisterHotKey(uint32_t id) override
  {
  }
  void fakeInputBegin() override
  {
  }
  void fakeInputEnd() override
  {
  }
  int32_t getJumpZoneSize() const override
  {
    return {};
  }
  bool isAnyMouseButtonDown(uint32_t &buttonID) const override
  {
    return {};
  }
  void getCursorCenter(int32_t &x, int32_t &y) const override
  {
  }
  void fakeMouseMove(int32_t x, int32_t y) override
  {
  }
  void fakeMouseRelativeMove(int32_t dx, int32_t dy) const override
  {
  }
  void fakeMouseWheel(ScrollDelta delta) const override
  {
  }
  void enable() override
  {
  }
  void disable() override
  {
  }
  void enter() override
  {
  }
  void leave() override
  {
  }
  bool setClipboard(ClipboardID, const IClipboard *) override
  {
    return {};
  }
  void checkClipboards() override
  {
  }
  void openScreensaver(bool notify) override
  {
  }
  void closeScreensaver() override
  {
  }
  void screensaver(bool activate) override
  {
  }
  void resetOptions() override
  {
  }
  void setOptions(const OptionsList &options) override
  {
  }
  void setSequenceNumber(uint32_t) override
  {
  }
  void updateButtons() override
  {
  }
  IKeyState *getKeyState() const override
  {
    return nullptr;
  }
  void handleSystemEvent(const Event &event) override
  {
  }
};
class InputReleaseTests : public QObject
{
  Q_OBJECT
  Log log;
  MockEventQueue events;
  QTemporaryDir settings;
private Q_SLOTS:
  void initTestCase()
  {
    Settings::setSettingsFile(settings.filePath("test.conf"));
  }
  void disconnectReleasesOnlyHeldButtonsEvenWhenLeaveIsBlocked()
  {
    auto &platform = *new RecordingPlatform(&events); // Computer owns the platform.
    deskflow::Computer computer(&platform, &events);
    computer.enable();
    computer.enter(0);
    computer.mouseDown(kButtonLeft);
    computer.mouseDown(kButtonRight);
    computer.mouseUp(kButtonRight);
    platform.allowLeave = false;
    computer.disable();
    QCOMPARE(platform.buttons, (QList<QPair<int, bool>>{{1, true}, {3, true}, {3, false}, {1, false}}));
    QCOMPARE(platform.releases, 1);
    platform.allowLeave = true;
  }
  void normalLeaveReleasesDragAndDoesNotReleaseAgain()
  {
    auto &platform = *new RecordingPlatform(&events); // Computer owns the platform.
    deskflow::Computer computer(&platform, &events);
    computer.enable();
    computer.enter(0);
    computer.mouseDown(kButtonLeft);
    QVERIFY(computer.leave());
    QCOMPARE(platform.buttons.last(), qMakePair(1, false));
    const auto count = platform.buttons.size();
    computer.disable();
    QCOMPARE(platform.buttons.size(), count);
  }
  void primaryPhysicalInputIsNotReleased()
  {
    auto &platform = *new RecordingPlatform(&events, true);
    deskflow::Computer computer(&platform, &events);
    computer.enable();
    computer.disable();
    QCOMPARE(platform.releases, 0);
    QVERIFY(platform.buttons.isEmpty());
  }
};
QTEST_GUILESS_MAIN(InputReleaseTests)
#include "InputReleaseTests.moc"
