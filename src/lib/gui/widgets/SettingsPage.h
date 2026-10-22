/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "gui/LoginStartup.h"
#include "gui/config/ServerConfig.h"
#include <QWidget>
#include <functional>

class QVBoxLayout;
class QLabel;
class QPushButton;
class QTabBar;
class QStackedWidget;
class QComboBox;
class QListWidget;
class QCheckBox;
class QDialog;
class KeySequenceWidget;

class SettingsPage : public QWidget
{
  Q_OBJECT
public:
  enum Category
  {
    General,
    Input,
    Hotkeys,
    Security,
    Network,
    Logs,
    More
  };
  explicit SettingsPage(ServerConfig &config, QWidget *parent = nullptr, LoginStartup startup = LoginStartup());
  bool isModified() const;
  void activate(Category category = General);
  void activateComputer(const QString &name);
  bool save();
  void discard();

Q_SIGNALS:
  void saved(bool restartCore);
  void showLogsRequested();
  void clearSettingsRequested();

protected:
  void changeEvent(QEvent *event) override;

private:
  struct Binding
  {
    QString key;
    QWidget *control;
    std::function<QVariant()> read;
    std::function<void(const QVariant &)> write;
    QVariant original;
    bool restart;
  };
  void build();
  void load();
  void updateState();
  void restoreDefaults();
  QVBoxLayout *category(const QString &title, const QString &description);
  QVBoxLayout *group(QVBoxLayout *parent);
  QWidget *row(QVBoxLayout *parent, const QString &title, const QString &description, QWidget *control);
  QCheckBox *toggle(
      QVBoxLayout *parent, const QString &key, const QString &title, const QString &description, bool restart = true
  );
  QComboBox *choice(
      QVBoxLayout *parent, const QString &key, const QString &title, const QString &description,
      const QList<QPair<QString, QVariant>> &items, bool restart = true
  );
  QWidget *number(
      QVBoxLayout *parent, const QString &key, const QString &title, const QString &description, int minimum,
      int maximum, const QString &suffix = {}, bool decimal = false
  );
  QWidget *path(QVBoxLayout *parent, const QString &key, const QString &title, const QString &description);
  void bind(
      const QString &key, QWidget *control, std::function<QVariant()> read, std::function<void(const QVariant &)> write,
      bool restart
  );
  QPushButton *button(const QString &text, const QString &name = {});
  void message(const QString &text, bool error = false);
  void buildHotkeys(QVBoxLayout *layout);
  void refreshHotkeys();
  void editAction(bool existing);
  void clearEditor();
  void buildDeviceEditor(QVBoxLayout *layout);
  void selectDevice(int index);
  bool serverDraftModified() const;

  ServerConfig &m_config;
  ServerConfig m_baseline;
  ServerConfig m_draft;
  LoginStartup m_startup;
  QList<Binding> m_bindings;
  bool m_loading = false;
  bool m_startupOriginal = false;
  bool m_regenerate = false;
  QCheckBox *m_startupToggle = nullptr;
  QLabel *m_state = nullptr;
  QLabel *m_hint = nullptr;
  QPushButton *m_save = nullptr;
  QPushButton *m_discard = nullptr;
  QTabBar *m_tabs = nullptr;
  QStackedWidget *m_categories = nullptr;
  QComboBox *m_devices = nullptr;
  QWidget *m_deviceFields = nullptr;
  QListWidget *m_hotkeys = nullptr;
  QListWidget *m_actions = nullptr;
  KeySequenceWidget *m_sequence = nullptr;
  QWidget *m_actionEditor = nullptr;
  QWidget *m_triggerEditor = nullptr;
  QWidget *m_ruleActions = nullptr;
  QLabel *m_hotkeyEmpty = nullptr;
  int m_triggerEditIndex = -1;
  QDialog *m_editor = nullptr;
  Hotkey m_editHotkey;
  Action m_editAction;
  QList<QWidget *> m_serverControls;
};
