// SPDX-License-Identifier: MIT
#include "BluetoothPairing.h"
#import <IOBluetooth/IOBluetooth.h>
#include <cstring>

@interface DFPairDelegate : NSObject <IOBluetoothDevicePairDelegate>
@property(nonatomic, assign) BluetoothPairing *owner;
@end
@implementation DFPairDelegate
- (void)devicePairingUserConfirmationRequest:(id)sender numericValue:(BluetoothNumericValue)value
{
  Q_UNUSED(sender);
  if (self.owner)
    Q_EMIT self.owner->confirmationRequired(QString::number(value).rightJustified(6, '0'));
}
- (void)devicePairingPINCodeRequest:(id)sender
{
  Q_UNUSED(sender);
  if (self.owner)
    Q_EMIT self.owner->pinRequired();
}
- (void)devicePairingUserPasskeyNotification:(id)sender passkey:(BluetoothPasskey)value
{
  Q_UNUSED(sender);
  if (self.owner)
    Q_EMIT self.owner->passkeyDisplayed(QString::number(value).rightJustified(6, '0'));
}
- (void)devicePairingFinished:(id)sender error:(IOReturn)error
{
  Q_UNUSED(sender);
  if (self.owner)
    self.owner->complete(error == kIOReturnSuccess, QStringLiteral("Native pairing error %1").arg(error));
}
@end

struct NativePairState
{
  __strong IOBluetoothDevicePair *pair;
  __strong DFPairDelegate *delegate;
};

void BluetoothPairing::pair(const QBluetoothAddress &peer)
{
  cancel();
  if (peer.isNull()) {
    Q_EMIT finished(false, "Invalid Bluetooth address");
    return;
  }
  auto *device = [IOBluetoothDevice deviceWithAddressString:peer.toString().toNSString()];
  if (!device) {
    Q_EMIT finished(false, "Cannot open Bluetooth device");
    return;
  }
  if (device.isPaired) {
    Q_EMIT finished(true, {});
    return;
  }
  auto *state = new NativePairState;
  m_native = state;
  state->delegate = [DFPairDelegate new];
  state->delegate.owner = this;
  state->pair = [IOBluetoothDevicePair pairWithDevice:device];
  state->pair.delegate = state->delegate;
  m_active = true;
  m_deadline.start();
  const auto status = [state->pair start];
  if (status != kIOReturnSuccess)
    complete(false, QStringLiteral("Cannot start pairing: %1").arg(status));
}

void BluetoothPairing::confirm(bool accepted)
{
  if (m_active && m_native)
    [static_cast<NativePairState *>(m_native)->pair replyUserConfirmation:accepted];
}

void BluetoothPairing::providePin(const QString &pin)
{
  const auto bytes = pin.toUtf8();
  if (!m_active || !m_native || bytes.isEmpty() || bytes.size() > 16)
    return;
  BluetoothPINCode value = {};
  std::memcpy(&value, bytes.constData(), size_t(bytes.size()));
  [static_cast<NativePairState *>(m_native)->pair replyPINCode:bytes.size() PINCode:&value];
}

void BluetoothPairing::cancel()
{
  m_active = false;
  m_deadline.stop();
  if (auto *state = static_cast<NativePairState *>(m_native)) {
    state->delegate.owner = nullptr;
    state->pair.delegate = nil;
    [state->pair stop];
    delete state;
    m_native = nullptr;
  }
}
