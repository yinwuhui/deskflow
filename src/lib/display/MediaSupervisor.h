// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#pragma once
#include "DisplayService.h"
#include "ResourceJournal.h"
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <memory>
namespace deskflow::display {
struct MediaRuntime {
  QString python;
  QString sessionScript;
  QString identityDirectory;
  QString nativeViewer;
  QString macosProbe;
  QString source;
  QString bindAddress = QStringLiteral("127.0.0.1");
  quint16 port = 24810;
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
};
struct TrustedMediaEndpoint {
  Peer peer;
  QString certificate;
  QString address;
  quint16 port = 24810;
};
// Typed internal adapter. GUI commands cannot select executables, private keys,
// certificates or bypass authorization. No shell and no GUI-thread waits.
class MediaSupervisor final : public QObject {
  Q_OBJECT
public:
  MediaSupervisor(DisplayService* service, MediaRuntime runtime, const QString& resourceDirectory, QObject* parent = nullptr);
  ~MediaSupervisor() override;
  Outcome addTrustedEndpoint(TrustedMediaEndpoint endpoint);
  ResourceJournal::Status journalStatus() const { return m_journal.status(); }
Q_SIGNALS:
  void listening(const QJsonObject& endpoint); // Authenticated control adapter transmits this after consent.
  void adapterError(const QString& reasonCode);
private:
  void command(const QJsonObject& command);
  void start(const Key& key, Role role);
  void consume(QProcess* process, const Key& owner);
  void finish(QProcess* process, const Key& owner, Role role);
  void cleanup(const Key& key);
  void finishCleanup(const Key& key, bool verified);
  void verifyResidual(const Key& cleanupKey);
  QPointer<DisplayService> m_service;
  MediaRuntime m_runtime;
  ResourceJournal m_journal;
  QHash<QString, TrustedMediaEndpoint> m_endpoints;
  QProcess* m_process = nullptr;
  Key m_owner;
  QByteArray m_stdout, m_stderr;
  bool m_cleanupVerified = false;
  bool m_exited = false;
  QTimer m_forceTimer;
};
} // namespace deskflow::display
