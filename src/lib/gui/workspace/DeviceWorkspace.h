// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "gui/ComputerSetupModel.h"
#include "gui/config/ServerConfig.h"
#include "gui/core/ApplicationState.h"
#include <QWidget>
#include <functional>
class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;
class QGridLayout;
class QSpinBox;
namespace deskflow::gui::workspace {
class DeviceCanvas;
class DeviceWorkspace : public QWidget
{
  Q_OBJECT
public:
  explicit DeviceWorkspace(ServerConfig &saved, std::function<bool()> persist, QWidget *parent = nullptr);
  bool hasUnsavedChanges() const;
  void reloadSaved();
  void setSessionState(
      const QStringList &connected, const QMap<QString, ApplicationState::Session> &sessions, bool external,
      bool receiver, const QString &source
  );
  void beginEditing();
  bool moveSelectedTo(int column, int row);
  const ServerConfig &draftLayout() const
  {
    return m_draft;
  }
  void addDevice(const QString &identifier = {});
  void editSelected();
  QWidget *createRulesPage();
  bool saveLayout();
  bool saveRules();
  void discardLayout();
  QString selectedIdentifier() const;
Q_SIGNALS:
  void requestConfiguration();
  void requestTrust();
  void requestVerify(const QString &identifier);

private:
  void build();
  void updateCards();
  bool moveDevice(int fromColumn, int fromRow, int toColumn, int toRow);
  void sync(bool refreshCanvas = true);
  void removeSelected();
  bool validate(const ComputerList &computers, const HotkeyList &rules, QString &error) const;
  void showIssue(const QString &message);
  void updateRuleRows();
  void editRule(int index);
  ServerConfig &m_saved;
  ServerConfig m_draft;
  ServerConfig m_base;
  ComputerSetupModel m_model;
  std::function<bool()> m_persist;
  DeviceCanvas *m_canvas = nullptr;
  QComboBox *m_selection = nullptr;
  QLabel *m_selectionName = nullptr, *m_selectionDescription = nullptr, *m_issue = nullptr, *m_footer = nullptr;
  QSpinBox *m_positionColumn = nullptr, *m_positionRow = nullptr;
  QPushButton *m_verify = nullptr;
  QPushButton *m_edit = nullptr, *m_save = nullptr, *m_cancel = nullptr, *m_add = nullptr, *m_remove = nullptr,
              *m_details = nullptr;
  QWidget *m_coordinates = nullptr, *m_cards = nullptr, *m_rulesContent = nullptr;
  QVBoxLayout *m_ruleRows = nullptr;
  QString m_selectedId;
  QStringList m_connected;
  QMap<QString, ApplicationState::Session> m_sessions;
  bool m_editing = false, m_external = false, m_receiver = false;
};
} // namespace deskflow::gui::workspace
