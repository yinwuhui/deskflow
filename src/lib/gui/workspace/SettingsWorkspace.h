// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QMap>
#include <QStackedWidget>
#include <QVariant>
#include <QWidget>
#include <functional>
class QCheckBox;
class QComboBox;
class QLineEdit;
class QVBoxLayout;
class QLabel;
class QPushButton;
namespace deskflow::gui::workspace {
class SettingsWorkspace : public QWidget
{
  Q_OBJECT
public:
  explicit SettingsWorkspace(QWidget *parent = nullptr);
  bool hasUnsavedChanges() const;
  void reloadSaved();
  void navigate(const QString &section);
  void setManagedExternally(bool external, bool receiver);
  void installPage(const QString &section, QWidget *content);
  bool resetOrdinaryPreferences();
  bool saveSection(const QString &section);
  QVariant draftValue(const QString &key) const;
  void setIdentityGenerationState(bool busy, const QString &message = {});
  void setFileValidationResult(const QString &path, bool success, const QString &reason);
  void setDraftValue(const QString &key, const QVariant &value);

protected:
  void resizeEvent(QResizeEvent *event) override;
Q_SIGNALS:
  void saved(const QString &section);
  void requestTrust();
  void requestFingerprint();
  void requestIdentityGeneration(int keyLength);
  void requestClear();
  void requestBackup();
  void requestGraphExport();
  void requestRoutingExport();
  void requestRestore();
  void requestRoleChange();
  void requestValidateFile(const QString &path);

private:
  struct Field
  {
    QString section;
    QVariant baseline;
    QVariant persisted;
    QWidget *widget = nullptr;
    std::function<QVariant()> read;
    std::function<void(const QVariant &)> write;
  };
  QCheckBox *check(const QString &section, const QString &key);
  QComboBox *choice(const QString &section, const QString &key, const QList<QPair<QString, QVariant>> &items);
  QWidget *number(
      const QString &section, const QString &key, int minimum, int maximum, int step = 1, const QString &suffix = {}
  );
  QLineEdit *text(const QString &section, const QString &key);
  void bind(
      const QString &section, const QString &key, QWidget *widget, std::function<QVariant()> read,
      std::function<void(const QVariant &)> write
  );
  void changed(const QString &key);
  void updateFieldAvailability();
  void buildPages();
  void updateHeaderHeight();
  void addPage(const QString &section, QWidget *page);
  void addSave(QWidget *page, const QString &section, const QString &text);
  void showError(const QString &section, const QString &message);
  QMap<QString, Field> m_fields;
  QMap<QString, QWidget *> m_sections;
  QMap<QString, QLabel *> m_errors;
  QMap<QString, QPushButton *> m_tabs;
  QStackedWidget *m_headers = nullptr;
  QStackedWidget *m_stack = nullptr;
  QWidget *m_advancedLinks = nullptr;
  QLabel *m_managementNotice = nullptr;
  QLabel *m_receiverNotice = nullptr;
  bool m_loading = false;
  bool m_external = false, m_receiver = false;
};
} // namespace deskflow::gui::workspace
