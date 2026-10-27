// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QJsonArray>
#include <QWidget>
namespace deskflow::display {
class ScreenInventoryService;
}
class QLabel;
class QToolButton;
class QPushButton;
class QVBoxLayout;
class QTimer;
class DeviceScreenInfo final : public QWidget
{
  Q_OBJECT
public:
  explicit DeviceScreenInfo(QWidget *parent = nullptr, deskflow::display::ScreenInventoryService *service = nullptr);
  void setDevice(const QString &device, bool local = false);
  QWidget *extensionSettings() const
  {
    return m_preferences;
  }
  void setExtensionSelected(bool selected);

private:
  void refresh();
  void save();
  void rebuildScreens(const QJsonArray &screens);
  QString screenName(const QJsonObject &screen, int index) const;
  deskflow::display::ScreenInventoryService *m_service = nullptr;
  QString m_device;
  bool m_local = false, m_extensionSelected = false;
  QJsonArray m_screenData;
  QLabel *m_state, *m_mode, *m_result;
  QWidget *m_preferences, *m_cards;
  QVBoxLayout *m_cardLayout;
  QToolButton *m_target;
  QPushButton *m_windowed, *m_fullscreen;
  QTimer *m_resultTimer;
};
