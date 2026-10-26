/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClientProxyTests.h"

#include "../deskflow/MockEventQueue.h"
#include "deskflow/AppUtil.h"
#include "deskflow/OptionTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "deskflow/UsageControlWire.h"
#include "io/IStream.h"
#include "server/ClientProxy1_0.h"
#include "server/ClientProxy1_1.h"
#include "server/ClientProxy1_6.h"
#include "server/ClientProxy1_7.h"
#include "server/ClientProxy1_8.h"

#include <memory>
#include <cstring>
#include <string>
#include <vector>

#include <QByteArray>
#include <QTest>

namespace {

class TestAppUtil : public AppUtil
{
public:
  int run() override
  {
    return 0;
  }

  std::vector<std::string> getKeyboardLayoutList() override
  {
    return {"en"};
  }

  std::string getCurrentLanguageCode() override
  {
    return "en";
  }
};

class CapturingStream : public deskflow::IStream
{
public:
  QByteArray take()
  {
    auto bytes = m_buffer;
    m_buffer.clear();
    return bytes;
  }

  void write(const void *buffer, uint32_t n) override
  {
    m_buffer.append(static_cast<const char *>(buffer), n);
  }

  void close() override
  {
  }

  QByteArray input;
  std::string identity;
  std::string peerIdentity() const override { return identity; }
  uint32_t read(void* output, uint32_t count) override
  {
    auto size = std::min<size_t>(count, input.size());
    if (output) std::memcpy(output, input.data(), size);
    input.remove(0, size);
    return size;
  }

  void flush() override
  {
  }

  void shutdownInput() override
  {
  }

  void shutdownOutput() override
  {
  }

  void *getEventTarget() const override
  {
    return const_cast<CapturingStream *>(this);
  }

  bool isReady() const override
  {
    return false;
  }

  uint32_t getSize() const override
  {
    return 0;
  }

private:
  QByteArray m_buffer;
};

std::unique_ptr<ClientProxy> makeProxy(int minor, deskflow::IStream *stream, IEventQueue *events)
{
  // the 1.4 and later constructors assert the server pointer is non-null but only store it
  auto *server = reinterpret_cast<Server *>(0x1);

  std::unique_ptr<ClientProxy> proxy;
  switch (minor) {
  case 0:
    proxy = std::make_unique<ClientProxy1_0>("client", stream, events);
    break;
  case 1:
    proxy = std::make_unique<ClientProxy1_1>("client", stream, events);
    break;
  case 6:
    proxy = std::make_unique<ClientProxy1_6>("client", stream, server, events);
    break;
  case 7:
    proxy = std::make_unique<ClientProxy1_7>("client", stream, server, events);
    break;
  case 8:
    proxy = std::make_unique<ClientProxy1_8>("client", stream, server, events);
    break;
  default:
    break;
  }
  return proxy;
}

struct ProxyUnderTest
{
  MockEventQueue events;
  CapturingStream *stream = new CapturingStream;
  std::unique_ptr<ClientProxy> proxy;

  explicit ProxyUnderTest(int minor) : proxy(makeProxy(minor, stream, &events))
  {
    // drop the query-info and layout-sync messages the constructors send
    stream->take();
  }
};

const KeyID kKey = 0x61;
const KeyModifierMask kMask = 0;
const KeyButton kButton = 0x1e;
const int32_t kCount = 3;
const std::string kLang = "en";

} // namespace

void ClientProxyTests::initTestCase()
{
  // the 1.8 constructor reads the keyboard layouts through AppUtil::instance()
  static TestAppUtil appUtil;
}

// These formats are frozen because shipped third-party clients parse them byte
// for byte: Synergy 1.4 through 1.14.1 negotiate 1.4 through 1.7, Barrier and
// Input Leap negotiate 1.6, and Synergy 1.14.2 onwards and Deskflow negotiate 1.8.
void ClientProxyTests::keyDown_data()
{
  QTest::addColumn<int>("minor");
  QTest::addColumn<QByteArray>("expected");

  QTest::newRow("1.0") << 0 << "DKDN" + QByteArray::fromHex("0061 0000");
  QTest::newRow("1.1") << 1 << "DKDN" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.6") << 6 << "DKDN" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.7") << 7 << "DKDN" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.8") << 8 << "DKDL" + QByteArray::fromHex("0061 0000 001e 00000002") + "en";
}

void ClientProxyTests::keyDown()
{
  QFETCH(int, minor);
  QFETCH(QByteArray, expected);

  ProxyUnderTest test(minor);
  test.proxy->keyDown(kKey, kMask, kButton, kLang);
  QCOMPARE(test.stream->take(), expected);
}

void ClientProxyTests::keyRepeat_data()
{
  QTest::addColumn<int>("minor");
  QTest::addColumn<QByteArray>("expected");

  QTest::newRow("1.0") << 0 << "DKRP" + QByteArray::fromHex("0061 0000 0003");
  QTest::newRow("1.1") << 1 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e");
  QTest::newRow("1.6") << 6 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e");
  QTest::newRow("1.7") << 7 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e");
  QTest::newRow("1.8") << 8 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e 00000002") + "en";
}

void ClientProxyTests::keyRepeat()
{
  QFETCH(int, minor);
  QFETCH(QByteArray, expected);

  ProxyUnderTest test(minor);
  test.proxy->keyRepeat(kKey, kMask, kCount, kButton, kLang);
  QCOMPARE(test.stream->take(), expected);
}

void ClientProxyTests::keyUp_data()
{
  QTest::addColumn<int>("minor");
  QTest::addColumn<QByteArray>("expected");

  QTest::newRow("1.0") << 0 << "DKUP" + QByteArray::fromHex("0061 0000");
  QTest::newRow("1.1") << 1 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.6") << 6 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.7") << 7 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.8") << 8 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
}

void ClientProxyTests::keyUp()
{
  QFETCH(int, minor);
  QFETCH(QByteArray, expected);

  ProxyUnderTest test(minor);
  test.proxy->keyUp(kKey, kMask, kButton);
  QCOMPARE(test.stream->take(), expected);
}

QTEST_MAIN(ClientProxyTests)

namespace {
class InputControlProxy : public ClientProxy1_0
{
public:
  using ClientProxy1_0::ClientProxy1_0;
  using ClientProxy1_0::parseMessage;
};
}
void ClientProxyTests::inputControlNegotiationAndExactAcknowledgement()
{
  MockEventQueue events;
  auto* stream = new CapturingStream;
  stream->identity = std::string(64, 'a');
  InputControlProxy proxy("client", stream, &events);
  stream->take();
  QVERIFY(!proxy.supportsInputControl());
  QVERIFY(!proxy.requestInputControl(1, false));
  QVERIFY(stream->take().isEmpty()); // No new packet before explicit opt-in.
  proxy.setOptions({kOptionInputControl, 1});
  stream->take();
  stream->input = QByteArray::fromHex("00000001");
  QVERIFY(proxy.parseMessage(reinterpret_cast<const uint8_t*>(kMsgDInputControlCapability)));
  QVERIFY(proxy.supportsInputControl());
  QVERIFY(proxy.requestInputControl(7, false));
  QCOMPARE(stream->take(), QByteArray::fromHex("4349434d0000000700"));
  stream->input = QByteArray::fromHex("0000000601");
  QVERIFY(proxy.parseMessage(reinterpret_cast<const uint8_t*>(kMsgDInputControlAck)));
  QVERIFY(!proxy.inputControlAcknowledged(7, false));
  stream->input = QByteArray::fromHex("0000000702");
  QVERIFY(proxy.parseMessage(reinterpret_cast<const uint8_t*>(kMsgDInputControlAck)));
  QVERIFY(!proxy.inputControlAcknowledged(7, false));
  stream->input = QByteArray::fromHex("0000000701");
  QVERIFY(proxy.parseMessage(reinterpret_cast<const uint8_t*>(kMsgDInputControlAck)));
  QVERIFY(proxy.inputControlAcknowledged(7, false));
  QVERIFY(!proxy.requestInputControl(7, true));
  QVERIFY(!proxy.requestInputControl(6, true));
  QVERIFY(proxy.requestInputControl(8, true));
  QCOMPARE(stream->take(), QByteArray::fromHex("4349434d0000000801"));
  QVERIFY(!proxy.inputControlAcknowledged(7, false));
  stream->input = QByteArray::fromHex("0000000802");
  QVERIFY(proxy.parseMessage(reinterpret_cast<const uint8_t*>(kMsgDInputControlAck)));
  QVERIFY(proxy.inputControlAcknowledged(8, true));
  QVERIFY(proxy.requestInputReadiness(9));
  QCOMPARE(stream->take(),QByteArray::fromHex("4349434d0000000902"));
  QVERIFY(!proxy.inputReadinessAcknowledged(9));
  stream->input=QByteArray::fromHex("0000000903");
  QVERIFY(proxy.parseMessage(reinterpret_cast<const uint8_t*>(kMsgDInputControlAck)));
  QVERIFY(proxy.inputReadinessAcknowledged(9));
  proxy.publishInputRouteState(7);QCOMPARE(stream->take(),QByteArray::fromHex("4349435307"));
  proxy.publishInputRouteState(7);QVERIFY(stream->take().isEmpty());
  proxy.publishInputRouteState(3);QCOMPARE(stream->take(),QByteArray::fromHex("4349435303"));
  proxy.publishInputRouteState(9);QVERIFY(stream->take().isEmpty());
  QVERIFY(!proxy.inputControlAcknowledged(9,true));
}
void ClientProxyTests::inputControlRejectsUnauthenticatedAndUnsolicitedCapability()
{
  MockEventQueue events;
  auto* stream = new CapturingStream;
  InputControlProxy proxy("client", stream, &events);
  stream->take();
  proxy.setOptions({kOptionInputControl, 1});
  stream->input = QByteArray::fromHex("00000001");
  QVERIFY(!proxy.parseMessage(reinterpret_cast<const uint8_t*>(kMsgDInputControlCapability)));
  QVERIFY(!proxy.supportsInputControl());
  stream->identity = std::string(64, 'a');
  stream->input = QByteArray::fromHex("00000001");
  QVERIFY(!proxy.parseMessage(reinterpret_cast<const uint8_t*>(kMsgDInputControlCapability)));
  QVERIFY(!proxy.requestInputControl(1, false));
}

void ClientProxyTests::receiverControlIsCorrelatedAndReplaySafe()
{
  MockEventQueue events;auto* stream=new CapturingStream;stream->identity=std::string(64,'a');
  InputControlProxy proxy("client",stream,&events);stream->take();proxy.setOptions({kOptionInputControl,1});stream->take();
  stream->input=QByteArray::fromHex("00000001");QVERIFY(proxy.parseMessage(reinterpret_cast<const uint8_t*>(kMsgDInputControlCapability)));
  int calls=0;proxy.remoteInputControl=[&](uint8_t mode){++calls;return static_cast<uint8_t>(mode+1);};
  stream->input=QByteArray::fromHex("0000000100");QVERIFY(proxy.parseMessage(reinterpret_cast<const uint8_t*>("DICR")));
  QCOMPARE(stream->take(),QByteArray::fromHex("434943410000000101"));QCOMPARE(calls,1);
  stream->input=QByteArray::fromHex("0000000100");QVERIFY(proxy.parseMessage(reinterpret_cast<const uint8_t*>("DICR")));
  QCOMPARE(stream->take(),QByteArray::fromHex("434943410000000101"));QCOMPARE(calls,1);
  stream->input=QByteArray::fromHex("0000000101");QVERIFY(proxy.parseMessage(reinterpret_cast<const uint8_t*>("DICR")));
  QCOMPARE(stream->take(),QByteArray::fromHex("434943410000000100"));QCOMPARE(calls,1);
  stream->input=QByteArray::fromHex("0000000203");QVERIFY(proxy.parseMessage(reinterpret_cast<const uint8_t*>("DICR")));
  QCOMPARE(stream->take(),QByteArray::fromHex("434943410000000204"));QCOMPARE(calls,2);
}
void ClientProxyTests::boundedUsageControlOnAuthenticatedStream()
{
  MockEventQueue events;auto* stream=new CapturingStream;stream->identity=std::string(64,'a');
  InputControlProxy proxy("client",stream,&events);stream->take();
  stream->input=QByteArray::fromHex("00004001");QVERIFY(!proxy.parseMessage(reinterpret_cast<const uint8_t*>("DUSG")));
  proxy.setOptions({kOptionInputControl,1});stream->take();stream->input=QByteArray::fromHex("00000001");
  QVERIFY(proxy.parseMessage(reinterpret_cast<const uint8_t*>(kMsgDInputControlCapability)));
  int calls=0;QJsonObject received;proxy.usageControlReceived=[&](const QJsonObject& body){++calls;received=body;return true;};
  const QJsonObject message{{"type","request"},{"protocol","usage-display/1"}};
  QVERIFY(deskflow::input::writeUsageControl(stream,"DUSG%s",message));auto frame=stream->take();
  QCOMPARE(frame.left(4),QByteArray("DUSG"));stream->input=frame.mid(4);
  QVERIFY(proxy.parseMessage(reinterpret_cast<const uint8_t*>("DUSG")));QCOMPARE(calls,1);QCOMPARE(received,message);
  stream->input=QByteArray::fromHex("00004001");QVERIFY(!proxy.parseMessage(reinterpret_cast<const uint8_t*>("DUSG")));QCOMPARE(calls,1);
  QVERIFY(!deskflow::input::writeUsageControl(stream,"DUSG%s",{{"large",QString(16384,'x')}}));
}
