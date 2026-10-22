/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QSet>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QStackedWidget;
class QVBoxLayout;
class QPushButton;

class DeviceListPage : public QWidget
{
  Q_OBJECT
public:
  enum class Kind
  {
    Local,
    Receiver,
    Server,
    History
  };
  enum class State
  {
    Local,
    Connected,
    Discovered,
    Offline
  };
  struct Device
  {
    QString id, name, address, source;
    Kind kind = Kind::History;
    State state = State::Offline;
    bool recent = false;
    bool configured = false;
    bool discovered = false;
    bool operator==(const Device &) const = default;
  };
  explicit DeviceListPage(QWidget *parent = nullptr);
  void setDevices(const QList<Device> &devices, bool sharing, const QString &status, bool busy = false);
  void selectDevice(const QString &id);
  QString selectedDevice() const
  {
    return m_selected;
  }
  void message(const QString &text, bool error = false);
  void setDiscoveryAvailable(bool available);
  static QString normalizedAddress(const QString &address, int defaultPort = 24800);

Q_SIGNALS:
  void connectRequested(const QString &address, const QString &name, bool autoConnect);
  void disconnectRequested();
  void layoutRequested(const QString &name);
  void settingsRequested(const QString &name);
  void inputSettingsRequested();
  void diagnosisRequested();
  void refreshRequested();
  void addServerRequested(const QString &name, const QString &address);

protected:
  void resizeEvent(QResizeEvent *event) override;
  void changeEvent(QEvent *event) override;

private:
  void build();
  void rebuildList();
  void rebuildDetail();
  void showAddForm();
  void submitAdd();
  QString stateText(State state) const;
  bool matches(const Device &device) const;
  void addGroup(const QString &key, const QString &title, const QList<Device> &devices);
  void attachAccessibleRow(QListWidgetItem *item);
  void addFact(QVBoxLayout *layout, const QString &label, const QString &value);
  const Device *device() const;
  QList<Device> m_devices;
  QString m_selected;
  QSet<QString> m_collapsed{QStringLiteral("recent")};
  bool m_sharing = true;
  bool m_busy = false;
  bool m_rebuilding = false;
  QListWidget *m_list = nullptr;
  QLineEdit *m_search = nullptr;
  QComboBox *m_filter = nullptr;
  QLabel *m_count = nullptr;
  QLabel *m_discovery = nullptr;
  QLabel *m_status = nullptr;
  QStackedWidget *m_detail = nullptr;
  QWidget *m_details = nullptr;
  QComboBox *m_addKind = nullptr;
  QLineEdit *m_addName = nullptr;
  QLineEdit *m_addAddress = nullptr;
  QWidget *m_addressRow = nullptr;
  QLabel *m_addError = nullptr;
};
