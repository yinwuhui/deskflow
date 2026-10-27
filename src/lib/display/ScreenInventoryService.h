// SPDX-License-Identifier: MIT
#pragma once
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <memory>
class QScreen;
namespace deskflow::display {
class CoreInputChannel;
class PeerUsageService;
// Metadata only. No capture, virtual displays, permission requests or video sockets.
class ScreenInventoryService final : public QObject {
  Q_OBJECT
public:
  explicit ScreenInventoryService(QObject* parent=nullptr,const QString& cacheFile={},bool watchScreens=true);
  static ScreenInventoryService* applicationService();
  void attach(CoreInputChannel*,PeerUsageService*);
  QJsonObject localInventory() const { return m_local; }
  QJsonObject peerInventory(const QString& peer) const;
  QJsonObject forDevice(const QString& device) const;
  bool setPreference(const QString& peer,const QString& screenId,const QString& presentationMode);
  QJsonObject selectionForPeer(const QString& peer) const;
  bool resolveLocalSelection(const QJsonObject& selection,QString* screenName=nullptr);
  void observeConnection(const QJsonObject& connection);
  void receiveControl(const QJsonObject& event);
  void refreshLocalScreens();
  // Validated local adapter input; also permits deterministic headless tests.
  bool updateLocalScreens(const QJsonArray& screens);
Q_SIGNALS:
  void changed();
  void outboundControl(const QString& peer,const QString& connection,const QJsonObject& body);
private:
  static bool validInventory(const QJsonObject&);
  void publish(const QString& peer,const QString& type="inventory");
  void cache(const QString& peer);
  void queryNativeScreens();
  QHash<QString,QJsonObject> m_nativeScreens;
  bool m_nativePending=false;
  QJsonObject preference(const QString& peer) const;
  std::unique_ptr<QSettings> m_settings;
  QJsonObject m_local;
  QHash<QString,QJsonObject> m_connections,m_peers;
  QHash<QString,QString> m_devices;
  QSet<QScreen*> m_watched;
  QTimer m_debounce;
  bool m_watch=false,m_attached=false;
};
}
