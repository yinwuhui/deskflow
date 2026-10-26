// SPDX-License-Identifier: MIT
#include "MockEventQueue.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "common/Settings.h"
#include "common/UsageInputState.h"
#include "deskflow/Computer.h"
#include "client/Client.h"
#include "client/ServerProxy.h"
#include "server/ClientProxy1_0.h"
#include "server/Server.h"
#include "server/PrimaryClient.h"
#include "server/Config.h"
#include "deskflow/ipc/CoreIpcServer.h"
#include "deskflow/OptionTypes.h"
#include "io/IStream.h"
#include "net/ISocketFactory.h"
#include <cstring>
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
  int keyPresses = 0;
  void fakeKeyDown(KeyID, KeyModifierMask, KeyButton, const std::string&) override { ++keyPresses; }
  KeyModifierMask getActiveModifiers() const override { return 0; }
  KeyModifierMask pollActiveModifiers() const override { return 0; }
  bool fakeKeyUp(KeyButton) override { return true; }
  bool fakeKeyRepeat(KeyID,KeyModifierMask,int32_t,KeyButton,const std::string&) override { return true; }
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
  { x=0;y=0;width=1920;height=1080; }
  void getCursorPos(int32_t &x, int32_t &y) const override
  { x=960;y=540; }
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
  { x=960;y=540; }
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
namespace {
class ControlStream : public deskflow::IStream
{
public:
  QByteArray input,output;
  std::string identity=std::string(64,'a');
  void write(const void* bytes,uint32_t count) override { output.append(static_cast<const char*>(bytes),count); }
  uint32_t read(void* bytes,uint32_t count) override {
    auto size=std::min<size_t>(count,input.size());if(bytes)std::memcpy(bytes,input.constData(),size);input.remove(0,size);return size;
  }
  QByteArray take() { auto result=output;output.clear();return result; }
  void close() override {}
  void flush() override {}
  void shutdownInput() override {}
  void shutdownOutput() override {}
  void* getEventTarget() const override { return const_cast<ControlStream*>(this); }
  bool isReady() const override { return !input.isEmpty(); }
  uint32_t getSize() const override { return input.size(); }
  std::string peerIdentity() const override { return identity; }
};
class NoSocketFactory : public ISocketFactory
{
public:
  IDataSocket* create(IArchNetwork::AddressFamily,SecurityLevel) const override { return nullptr; }
  IListenSocket* createListen(IArchNetwork::AddressFamily,SecurityLevel) const override { return nullptr; }
};
class ReceivingProxy : public ServerProxy
{
public:
  using ServerProxy::ServerProxy;
  bool message(const QByteArray& code) { return parseMessage(reinterpret_cast<const uint8_t*>(code.constData()))==ConnectionResult::Okay; }
};
class SendingProxy : public ClientProxy1_0
{
public:
  using ClientProxy1_0::ClientProxy1_0;
  bool message(const QByteArray& code) { return parseMessage(reinterpret_cast<const uint8_t*>(code.constData())); }
};
}
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
    new deskflow::core::ipc::CoreIpcServer(this);
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
  void suspendReleasesHeldInputEvenWhenLeaveIsBlocked()
  {
    auto &platform = *new RecordingPlatform(&events);
    deskflow::Computer computer(&platform, &events);
    computer.enable();
    computer.enter(0);
    computer.mouseDown(kButtonLeft);
    platform.allowLeave = false;
    QVERIFY(computer.suspendRemoteInput());
    QCOMPARE(platform.buttons, (QList<QPair<int, bool>>{{1, true}, {1, false}}));
    QCOMPARE(platform.releases, 1);
    // Portal/platform enablement stays available for the next explicitly
    // authorized use; suspension does not inject into the primary computer.
    platform.allowLeave = true;
    computer.disable();
  }
  void inputControlWireReleasesAndKeepsKeyboardWithMouseFocus()
  {
    auto& platform=*new RecordingPlatform(&events);
    deskflow::Computer computer(&platform,&events);
    Client client(&events,"receiver",NetworkAddress(),new NoSocketFactory,&computer);
    client.handshakeComplete();
    ControlStream receivingStream;
    ReceivingProxy receiver(&client,&receivingStream,&events);
    auto* sendingStream=new ControlStream;
    SendingProxy sender("receiver",sendingStream,&events);sendingStream->take();
    auto receive=[&] {
      auto bytes=sendingStream->take();receivingStream.input=bytes.mid(4);return receiver.message(bytes.left(4));
    };
    auto acknowledge=[&] {
      auto bytes=receivingStream.take();sendingStream->input=bytes.mid(4);return sender.message(bytes.left(4));
    };
    sender.setOptions({kOptionInputControl,1});QVERIFY(receive());QVERIFY(acknowledge());
    QVERIFY(sender.supportsInputControl());
    sender.publishInputRouteState(7);QVERIFY(receive());QVERIFY(receiver.sourceRouteActive());
    client.enter(0,0,1,0,false);client.keyDown(0x61,0,30,"");client.mouseDown(kButtonLeft);
    QCOMPARE(platform.keyPresses,1);QCOMPARE(platform.buttons.size(),1);
    platform.allowLeave=false;
    QVERIFY(sender.requestInputControl(1,false));QVERIFY(receive());QVERIFY(acknowledge());
    QVERIFY(sender.inputControlAcknowledged(1,false));QCOMPARE(platform.releases,1);
    QCOMPARE(platform.buttons.last(),qMakePair(1,false));
    client.enter(0,0,2,0,false);client.keyDown(0x61,0,30,"");client.mouseDown(kButtonLeft);
    QCOMPARE(platform.keyPresses,1);QCOMPARE(platform.buttons.size(),2);
    // An old/duplicate sequence cannot resume the receiver.
    QVERIFY(!sender.requestInputControl(1,true));
    QVERIFY(sender.requestInputControl(2,true));QVERIFY(receive());QVERIFY(acknowledge());
    QVERIFY(sender.inputControlAcknowledged(2,true));
    client.keyDown(0x61,0,30,"");client.mouseDown(kButtonLeft);
    QCOMPARE(platform.keyPresses,1);QCOMPARE(platform.buttons.size(),2);
    // Only a subsequent unified focus entry can deliver keyboard and mouse.
    platform.allowLeave=true;client.enter(0,0,3,0,false);
    client.keyDown(0x61,0,30,"");client.mouseDown(kButtonLeft);
    QCOMPARE(platform.keyPresses,2);QCOMPARE(platform.buttons.last(),qMakePair(1,true));
    QVERIFY(sender.requestInputReadiness(3));QVERIFY(receive());QVERIFY(acknowledge());
    QVERIFY(sender.inputReadinessAcknowledged(3));
    QVERIFY(client.leave());
    computer.disable();
  }
  void serverSuspendsOnlyAuthenticatedTargetAndRequiresResumeCommit()
  {
    deskflow::server::Config config(&events);
    QVERIFY(config.addComputer("source"));QVERIFY(config.addComputer("target"));QVERIFY(config.addComputer("other"));
    auto& platform=*new RecordingPlatform(&events,true);
    deskflow::Computer computer(&platform,&events);PrimaryClient primary("source",&computer);
    Server server(config,&primary,&computer,&events);
    auto* firstStream=new ControlStream;auto* secondStream=new ControlStream;secondStream->identity=std::string(64,'b');
    auto* target=new SendingProxy("target",firstStream,&events);
    auto* other=new SendingProxy("other",secondStream,&events);
    server.adoptClient(target);server.adoptClient(other);firstStream->take();secondStream->take();
    firstStream->input=QByteArray::fromHex("00000001");
    QVERIFY(target->message(QByteArray(kMsgDInputControlCapability,4)));
    QVERIFY(server.requestInputRoute(std::string(64,'a'),1,false));
    auto status=server.inputRouteStatus(std::string(64,'a'),1,false);
    QVERIFY(status.suspended);QVERIFY(!status.acknowledged);QVERIFY(status.localFocus);
    QVERIFY(secondStream->take().isEmpty());QCOMPARE(server.getNumClients(),3U);
    QVERIFY(!server.inputRouteStatus(std::string(64,'b'),1,false).suspended);
    firstStream->input=QByteArray::fromHex("0000000101");QVERIFY(target->message(QByteArray(kMsgDInputControlAck,4)));
    QVERIFY(server.inputRouteStatus(std::string(64,'a'),1,false).acknowledged);
    QVERIFY(!server.inputRouteStatus(std::string(64,'a'),1,false).otherRoutesIsolated);
    QVERIFY(server.requestInputRoute(std::string(64,'a'),2,true));
    QVERIFY(!server.commitInputResume(std::string(64,'a'),2));
    firstStream->input=QByteArray::fromHex("0000000202");QVERIFY(target->message(QByteArray(kMsgDInputControlAck,4)));
    QVERIFY(server.commitInputResume(std::string(64,'a'),2));
    QVERIFY(!server.inputRouteStatus(std::string(64,'a'),2,true).suspended);
    QVERIFY(secondStream->take().isEmpty());
  }
  void pausedRouteSurvivesCoreRecreation()
  {
    auto run=[&](bool pause){
      deskflow::server::Config config(&events);QVERIFY(config.addComputer("source"));QVERIFY(config.addComputer("target"));
      auto& platform=*new RecordingPlatform(&events,true);deskflow::Computer computer(&platform,&events);PrimaryClient primary("source",&computer);
      Server server(config,&primary,&computer,&events);auto* stream=new ControlStream;auto* target=new SendingProxy("target",stream,&events);server.adoptClient(target);
      if(pause)QCOMPARE(server.remoteInputControl(target,0),uint8_t(1));
      else{
        QVERIFY(server.inputRouteStatus(std::string(64,'a'),0,false).suspended);
        QCOMPARE(server.remoteInputControl(target,3),uint8_t(0)); // No inherited resume preparation.
        QVERIFY(UsageInputState::setDisplayOwned(QString(64,'a'),true));
        QCOMPARE(server.remoteInputControl(target,1),uint8_t(0));
        QVERIFY(UsageInputState::setDisplayOwned(QString(64,'a'),false));
        QCOMPARE(server.remoteInputControl(target,1),uint8_t(2));
        QCOMPARE(server.remoteInputControl(target,3),uint8_t(4));
        QVERIFY(!server.inputRouteStatus(std::string(64,'a'),0,false).suspended);
      }
    };run(true);run(false);
  }
  void primaryPhysicalInputIsNotReleased()
  {
    auto &platform = *new RecordingPlatform(&events, true);
    deskflow::Computer computer(&platform, &events);
    computer.enable();
    QVERIFY(!computer.suspendRemoteInput());
    computer.disable();
    QCOMPARE(platform.releases, 0);
    QVERIFY(platform.buttons.isEmpty());
  }
};
QTEST_GUILESS_MAIN(InputReleaseTests)
#include "InputReleaseTests.moc"
