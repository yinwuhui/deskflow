// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include "MacVirtualOutput.h"
// Existing attributed MIT private declarations; see its LICENSE.md/SOURCE.md.
#import "third_party/DeskPad/CGVirtualDisplayPrivate.h"
#import <IOKit/IOKitLib.h>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#import <Security/AuthSession.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <sys/sysctl.h>
#include <unistd.h>
namespace deskflow::display::infra {
namespace {
constexpr uint32_t vendor = 0x424d, product = 0x0201;
QString displayIdentity(CGDirectDisplayID id)
{
  CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(id);
  if (!uuid)
    return {};
  CFStringRef text = CFUUIDCreateString(kCFAllocatorDefault, uuid);
  CFRelease(uuid);
  if (!text)
    return {};
  NSString *value = CFBridgingRelease(text);
  return QString::fromNSString(value).toLower();
}
bool selectors(Class cls, std::initializer_list<const char *> names)
{
  if (!cls)
    return false;
  for (const auto *name : names)
    if (![cls instancesRespondToSelector:sel_registerName(name)])
      return false;
  return true;
}
} // namespace
MacDisplayInventory macDisplayInventory()
{
  @autoreleasepool {
    MacDisplayInventory result;
    uint32_t count = 0;
    if (CGGetOnlineDisplayList(0, nullptr, &count) != kCGErrorSuccess || !count || count > 64)
      return result;
    std::vector<CGDirectDisplayID> ids(count);
    uint32_t actual = 0;
    if (CGGetOnlineDisplayList(count, ids.data(), &actual) != kCGErrorSuccess || actual != count)
      return result;
    std::sort(ids.begin(), ids.end());
    QJsonArray rows;
    for (const auto id : ids) {
      const auto b = CGDisplayBounds(id);
      const auto pw = CGDisplayPixelsWide(id), ph = CGDisplayPixelsHigh(id);
      if (!std::isfinite(b.origin.x) || !std::isfinite(b.origin.y) || !std::isfinite(b.size.width) ||
          !std::isfinite(b.size.height) || std::abs(b.origin.x) > 65536 || std::abs(b.origin.y) > 65536 ||
          b.size.width < 0 || b.size.height < 0 || b.size.width > 32768 || b.size.height > 32768 || pw > 32768 ||
          ph > 32768)
        return {};
      MacDisplayRecord r{
          id,
          CGDisplayVendorNumber(id),
          CGDisplayModelNumber(id),
          CGDisplaySerialNumber(id),
          uint32_t(pw),
          uint32_t(ph),
          displayIdentity(id),
          QRectF(b.origin.x, b.origin.y, b.size.width, b.size.height),
          bool(CGDisplayIsActive(id)),
          CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay || bool(CGDisplayIsInMirrorSet(id))
      };
      if (r.identity.isEmpty())
        return {};
      rows.append(
          QJsonObject{
              {"id", double(r.id)},
              {"identity", r.identity},
              {"vendor", double(r.vendor)},
              {"product", double(r.product)},
              {"serial", double(r.serial)},
              {"x", r.bounds.x()},
              {"y", r.bounds.y()},
              {"width", r.bounds.width()},
              {"height", r.bounds.height()},
              {"pixelWidth", double(r.pixelWidth)},
              {"pixelHeight", double(r.pixelHeight)},
              {"active", r.active},
              {"mirrored", r.mirrored}
          }
      );
      result.displays.push_back(std::move(r));
    }
    result.digest = QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(rows).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()
    );
    result.complete = true;
    return result;
  }
}
MacDesktopState macDesktopState()
{
  @autoreleasepool {
    MacDesktopState result;
    NSDictionary *info = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    result.onConsole = info && [info[(__bridge NSString *)kCGSessionOnConsoleKey] boolValue] &&
                       [info[(__bridge NSString *)kCGSessionUserIDKey] unsignedIntValue] == geteuid();
    NSNumber *quartz = info[@"CGSSessionScreenIsLocked"];
    if ([quartz isKindOfClass:[NSNumber class]]) {
      result.lockKnown = true;
      result.locked = quartz.boolValue;
    }
    io_registry_entry_t root = IORegistryGetRootEntry(kIOMainPortDefault);
    CFTypeRef locked =
        root ? IORegistryEntryCreateCFProperty(root, CFSTR("IOConsoleLocked"), kCFAllocatorDefault, 0) : nullptr;
    if (locked && CFGetTypeID(locked) == CFBooleanGetTypeID()) {
      const bool value = CFBooleanGetValue((CFBooleanRef)locked);
      result.locked = result.lockKnown ? (result.locked || value) : value;
      result.lockKnown = true;
    }
    if (locked)
      CFRelease(locked);
    if (root)
      IOObjectRelease(root);
    return result;
  }
}
QString macUserSessionReference()
{
  SecuritySessionId session = 0;
  SessionAttributeBits attributes = 0;
  if (SessionGetInfo(callerSecuritySession, &session, &attributes) != errSecSuccess ||
      !(attributes & sessionHasGraphicAccess))
    return {};
  return QString("mac.uid.%1.session.%2").arg(geteuid()).arg(session);
}
QString macBootInstanceReference()
{
  char value[128]{};
  size_t size = sizeof(value);
  if (sysctlbyname("kern.bootsessionuuid", value, &size, nullptr, 0) != 0 || !size || size > sizeof(value))
    return {};
  return QString::fromLatin1(value).toLower();
}
uint32_t macResourceSerial(const QString &owner, const QString &resource)
{
  const auto digest =
      QCryptographicHash::hash(owner.toUtf8() + QByteArray(1, '\0') + resource.toUtf8(), QCryptographicHash::Sha256);
  const auto value = qFromBigEndian<uint32_t>(digest.constData()) & 0x7fffffffU;
  return value ? value : 1;
}
bool macResourceHintPresent(const MacDisplayInventory &inventory, const QString &owner, const QString &resource)
{
  const auto serial = macResourceSerial(owner, resource);
  return std::any_of(inventory.displays.begin(), inventory.displays.end(), [&](const auto &d) {
    return d.vendor == vendor && d.product == product && d.serial == serial;
  });
}
struct MacVirtualOutput::Impl
{
  __strong CGVirtualDisplay *display = nil;
  WorkerPrepareOutput request;
  QString identity, owner;
  bool layoutApplied = false;
  int32_t origin = 0;
  std::shared_ptr<std::atomic<bool>> terminated = std::make_shared<std::atomic<bool>>(false);
};
MacVirtualOutput::MacVirtualOutput() : m_impl(std::make_unique<Impl>())
{
}
MacVirtualOutput::~MacVirtualOutput()
{
  destroy();
}
bool MacVirtualOutput::supported() const
{
  return selectors(
             NSClassFromString(@"CGVirtualDisplayDescriptor"),
             {"init", "setDispatchQueue:", "setName:", "setMaxPixelsWide:", "setMaxPixelsHigh:",
              "setSizeInMillimeters:", "setVendorID:", "setProductID:", "setSerialNum:"}
         ) &&
         selectors(NSClassFromString(@"CGVirtualDisplay"), {"initWithDescriptor:", "applySettings:", "displayID"}) &&
         selectors(NSClassFromString(@"CGVirtualDisplaySettings"), {"init", "setModes:", "setHiDPI:"}) &&
         selectors(NSClassFromString(@"CGVirtualDisplayMode"), {"initWithWidth:height:refreshRate:"});
}
bool MacVirtualOutput::create(const WorkerPrepareOutput &request, const QString &owner, QString &error)
{
  @autoreleasepool {
    @try {
      if (![NSThread isMainThread] || m_impl->display || !supported() ||
          !validOutputPreparation(request, Clock::now())) {
        error = "NativeOutputNotSupported";
        return false;
      }
      const auto desktop = macDesktopState();
      if (!desktop.onConsole || !desktop.lockKnown || desktop.locked) {
        error = "NativeDesktopUnavailable";
        return false;
      }
      const auto before = macDisplayInventory();
      if (!std::any_of(before.displays.begin(), before.displays.end(), [](const auto &r) {
            return r.active && r.bounds.width() > 0 && r.bounds.height() > 0;
          })) {
        error = "NativeDesktopUnavailable";
        return false;
      }
      if (!before.complete || before.digest != request.preCreateContextDigest) {
        error = "NativeTopologyChanged";
        return false;
      }
      if (macResourceHintPresent(before, owner, request.resourceId)) {
        error = "NativeResourceHintConflict";
        return false;
      }
      Class descriptorClass = NSClassFromString(@"CGVirtualDisplayDescriptor"),
            displayClass = NSClassFromString(@"CGVirtualDisplay"),
            settingsClass = NSClassFromString(@"CGVirtualDisplaySettings"),
            modeClass = NSClassFromString(@"CGVirtualDisplayMode");
      CGVirtualDisplayDescriptor *descriptor = [[descriptorClass alloc] init];
      [descriptor setDispatchQueue:dispatch_get_main_queue()];
      descriptor.name = [@"BMouse " stringByAppendingString:request.resourceId.toNSString()];
      descriptor.maxPixelsWide = request.width;
      descriptor.maxPixelsHigh = request.height;
      descriptor.sizeInMillimeters = CGSizeMake(508, 286);
      descriptor.vendorID = vendor;
      descriptor.productID = product;
      descriptor.serialNum = macResourceSerial(owner, request.resourceId);
      if ([descriptor respondsToSelector:@selector(setTerminationHandler:)]) {
        auto flag = m_impl->terminated;
        descriptor.terminationHandler = ^(id, CGVirtualDisplay *) {
          flag->store(true, std::memory_order_release);
        };
      }
      m_impl->request = request;
      m_impl->owner = owner;
      m_impl->display = [[displayClass alloc] initWithDescriptor:descriptor];
      if (!m_impl->display) {
        error = "NativeOutputCreateFailed";
        return false;
      }
      CGVirtualDisplaySettings *settings = [[settingsClass alloc] init];
      settings.hiDPI = 0;
      settings.modes = @[ [[modeClass alloc] initWithWidth:request.width
                                                    height:request.height
                                               refreshRate:request.refreshRate] ];
      if (![m_impl->display applySettings:settings]) {
        error = "NativeOutputModeFailed";
        destroy();
        return false;
      }
      return true;
    } @catch (NSException *) {
      error = "NativeOutputException";
      destroy();
      return false;
    }
  }
}
std::optional<WorkerOutputObserved> MacVirtualOutput::observation()
{
  if (!m_impl->display || m_impl->terminated->load(std::memory_order_acquire))
    return {};
  const auto id = m_impl->display.displayID;
  const auto identity = displayIdentity(id);
  if (!id || identity.isEmpty() || (!m_impl->identity.isEmpty() && identity != m_impl->identity))
    return {};
  m_impl->identity = identity;
  return WorkerOutputObserved{m_impl->request.resourceId, identity, {NativeSourceKind::CgDisplay, id, 0}, 1};
}
bool MacVirtualOutput::configureLayout()
{
  const auto claim = observation();
  if (!claim)
    return false;
  if (m_impl->layoutApplied)
    return healthy();
  const auto inventory = macDisplayInventory();
  if (!inventory.complete)
    return false;
  const auto id = uint32_t(claim->source.primary);
  const auto target = std::find_if(inventory.displays.begin(), inventory.displays.end(), [&](const auto &r) {
    return r.id == id && r.identity == claim->nativeIdentity && r.active && !r.mirrored;
  });
  if (target == inventory.displays.end())
    return false;
  double right = 0;
  for (const auto &other : inventory.displays)
    if (other.id != id)
      right = std::max(right, other.bounds.right());
  if (right > INT32_MAX - m_impl->request.width)
    return false;
  CGDisplayConfigRef config = nullptr;
  if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess)
    return false;
  if (CGConfigureDisplayOrigin(config, id, int32_t(std::ceil(right)), 0) != kCGErrorSuccess) {
    CGCancelDisplayConfiguration(config);
    return false;
  }
  if (CGCompleteDisplayConfiguration(config, kCGConfigureForSession) != kCGErrorSuccess)
    return false;
  m_impl->origin = int32_t(std::ceil(right));
  m_impl->layoutApplied = true;
  return false; // Verify the resulting native topology on the next tick.
}
bool MacVirtualOutput::healthy()
{
  const auto claim = observation();
  if (!claim || !m_impl->layoutApplied)
    return false;
  const auto inventory = macDisplayInventory();
  if (!inventory.complete)
    return false;
  for (const auto &r : inventory.displays)
    if (r.id == claim->source.primary && r.identity == claim->nativeIdentity)
      return r.active && !r.mirrored && r.vendor == vendor && r.product == product &&
             r.serial == macResourceSerial(m_impl->owner, m_impl->request.resourceId) && r.pixelWidth == 1920 &&
             r.pixelHeight == 1080 && r.bounds.width() == 1920 && r.bounds.height() == 1080 &&
             r.bounds.x() == m_impl->origin && r.bounds.y() == 0;
  return false;
}
void MacVirtualOutput::destroy()
{
  if (m_impl)
    m_impl->display = nil;
}
bool MacVirtualOutput::released() const
{
  if (m_impl->display)
    return false;
  const auto inventory = macDisplayInventory();
  if (!inventory.complete)
    return false;
  if (m_impl->identity.isEmpty())
    return m_impl->request.resourceId.isEmpty() ||
           !macResourceHintPresent(inventory, m_impl->owner, m_impl->request.resourceId);
  return std::none_of(inventory.displays.begin(), inventory.displays.end(), [&](const auto &d) {
    return d.identity == m_impl->identity;
  });
}
} // namespace deskflow::display::infra
