// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "MainWindow.h"
#include "common/LayoutReload.h"
#include "ui_MainWindow.h"
#include "workspace/DeviceWorkspace.h"
#include "workspace/SettingsWorkspace.h"
#include "workspace/SetupAssistant.h"
#include <QDesktopServices>
#include <QLineEdit>
#include <QMessageBox>
#include <QRadioButton>
#include <QSettings>
#include <QStackedWidget>
#include <QUrl>

void MainWindow::showSetupAssistant()
{
  beginSetupAssistant(false);
}

void MainWindow::beginSetupAssistant(bool chooseRole)
{
  if (m_identityRegenerating)
    return;
  if (!m_setupAssistant->active()) {
    if (m_settingsWorkspace->hasUnsavedChanges() || m_devicesWorkspace->hasUnsavedChanges()) {
      QMessageBox::information(
          this, tr("请先处理未保存的更改"), tr("请保存或取消设置和排列中的草稿，再开始连接设置。")
      );
      return;
    }
    const auto role = Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>();
    m_setupStartedCore = false;
    m_setupAssistant->begin(
        role != CoreMode::Client, chooseRole || role == CoreMode::None, m_connectedDevices,
        Settings::value(Settings::Client::RemoteHost).toString(), Settings::value(Settings::Core::Port).toInt()
    );
  }
  m_setupArranging = false;
  m_navigation->setCurrentRow(0);
  m_deviceModes->setCurrentIndex(2);
}

void MainWindow::setupAssistantSignals()
{
  using deskflow::gui::workspace::SetupAssistant;
  connect(m_setupAssistant, &SetupAssistant::deviceRequested, this, &MainWindow::serverConnectionConfigureClient);
  connect(&m_coreProcess, &CoreProcess::controlFinished, this, [this](const QJsonObject &result) {
    if (m_layoutReloadRequest.isEmpty() || result.value("id").toString() != m_layoutReloadRequest)
      return;
    const bool success = result.value("success").toBool();
    m_layoutReloadRequest.clear();
    if (!success)
      m_coreProcess.restoreRuntimeConfiguration(m_beforeLayoutReload);
    else
      m_confirmedRuntimeFile = m_coreProcess.runtimeSettingsFile();
    m_applicationState.finishApply(
        m_layoutReloadRevision, success,
        success ? QString() : tr("排列热更新未被核心确认，原连接保持不变。请查看诊断后重试。")
    );
    m_layoutReloadRevision = 0;
    persistWorkspaceState();
    refreshWorkspace();
  });

  connect(
      m_setupAssistant, &SetupAssistant::connectRequested, this,
      [this](bool source, const QString &address, int port) {
        const bool running = m_coreProcess.processState() != ProcessState::Stopped;
        const auto requested = source ? CoreMode::Server : CoreMode::Client;
        // Adding to an existing source does not restart its established sessions.
        if (running && m_coreProcess.mode() == CoreMode::Server && source)
          return;
        if (running && requested == CoreMode::Client && m_coreProcess.mode() == requested &&
            address == m_activeSource && port == Settings::value(Settings::Core::Port).toInt() &&
            !m_applicationState.pending()) {
          if (m_coreProcess.connectionState() == ConnectionState::Connected)
            m_setupAssistant->observeConnected(m_activeSource);
          return;
        }
        ui->rbModeServer->setChecked(source);
        ui->rbModeClient->setChecked(!source);
        ui->lineHostname->setText(address);
        Settings::setValue(Settings::Core::Port, port);
        if (!persistConfiguration()) {
          m_setupAssistant->showFailure(tr("配置未能保存，尚未开始连接。请检查诊断。"));
          return;
        }
        if (running)
          applySavedConfiguration();
        else {
          m_setupStartedCore = true;
          startCore();
        }
      }
  );
  connect(m_setupAssistant, &SetupAssistant::discoveryRequested, this, [this](const QUuid &attempt) {
    if (!m_mdnsBrowser) {
      m_mdnsBrowser = new deskflow::mdns::MdnsBrowser(this);
      m_mdnsBrowser->start();
    }
    // Each subscription belongs to one visible attempt; stale results are ignored by the assistant.
    if (m_setupDiscoveryConnection)
      disconnect(m_setupDiscoveryConnection);
    m_setupDiscoveryConnection =
        connect(m_mdnsBrowser, &deskflow::mdns::MdnsBrowser::serversChanged, this, [this, attempt] {
          m_setupAssistant->setCandidates(attempt, m_mdnsBrowser->servers());
        });
    m_setupAssistant->setCandidates(attempt, m_mdnsBrowser->servers());
  });
  connect(m_setupAssistant, &SetupAssistant::localFingerprintRequested, this, &MainWindow::showMyFingerprint);
  connect(m_setupAssistant, &SetupAssistant::permissionHelpRequested, this, [this] {
#ifdef Q_OS_MACOS
    QDesktopServices::openUrl(QUrl("x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"));
#else
    m_navigation->setCurrentRow(2);
#endif
  });
  connect(m_setupAssistant, &SetupAssistant::canceled, this, [this] {
    m_pendingIdentities.clear();
    m_pendingFingerprints.clear();
    if (m_sourceChange.phase() == deskflow::gui::SourceChangeTransaction::Phase::Candidate)
      m_sourceChange.failed(m_sourceChange.attempt(), m_sourceChange.expectedSource(), tr("已取消新来源设置。"));
    else if (m_setupStartedCore && !m_sourceChange.active())
      stopCore();
    m_setupStartedCore = false;
    m_setupArranging = false;
    refreshWorkspace();
  });
  connect(m_setupAssistant, &SetupAssistant::dismissed, this, [this] {
    m_setupStartedCore = false;
    m_setupArranging = false;
    refreshWorkspace();
  });
  connect(m_setupAssistant, &SetupAssistant::finished, this, [this](const QString &target) {
    const bool verified =
        !target.isEmpty() && !m_paused && !m_applicationState.applying() && m_applicationState.verifyManually(target);
    const auto reason =
        !target.isEmpty() && !verified ? tr("连接或共享状态已变化，未记录输入成功。请恢复连接后重新验证。") : QString();
    if (!reason.isEmpty())
      m_lastOperationError = reason;
    m_setupStartedCore = false;
    m_setupArranging = false;
    m_setupAssistant->showCompletion(verified, reason);
    refreshWorkspace();
  });
  connect(m_setupAssistant, &SetupAssistant::arrangeRequested, this, [this] {
    m_setupArranging = true;
    m_navigation->setCurrentRow(0);
    m_deviceModes->setCurrentIndex(0);
    m_devicesWorkspace->beginEditing();
    // The global Add button resumes this same attempt after arranging.
  });
  connect(&m_coreProcess, &CoreProcess::error, this, [this] {
    if (m_setupAssistant->active())
      m_setupAssistant->showFailure(tr("本次连接遇到错误。请查看诊断，修正后重试；输入仍保持未验证。"));
  });
}

void MainWindow::applyAddedDeviceLayout()
{
  if (m_applicationState.applying() || m_coreProcess.mode() != CoreMode::Server || !m_coreProcess.isStarted() ||
      m_activeExternalConfig)
    return;
  if (m_coreProcess.serviceMode()) {
    m_lastOperationError = tr("排列已经保存。后台服务需要明确应用配置以同步其重启参数；现有会话保持运行。");
    refreshWorkspace();
    return;
  }
  m_beforeLayoutReload = m_coreProcess.runtimeConfiguration();
  if (!m_coreProcess.prepareConfiguration()) {
    m_lastOperationError = tr("排列已经保存，但核心检查未通过；现有连接保持不变。请查看诊断。");
    refreshWorkspace();
    return;
  }
  QSettings active(m_beforeLayoutReload.settingsFile, QSettings::IniFormat);
  QSettings candidate(m_coreProcess.runtimeSettingsFile(), QSettings::IniFormat);
  if (!deskflow::permitsLayoutReload(active, candidate)) {
    m_coreProcess.restoreRuntimeConfiguration(m_beforeLayoutReload);
    m_lastOperationError = tr("排列已经保存，但还有其他运行参数变化。请明确应用配置后再连接新电脑，现有连接保持不变。");
    refreshWorkspace();
    return;
  }
  m_layoutReloadRevision = m_applicationState.beginApply();
  m_layoutReloadRequest = m_coreProcess.control("reloadLayout", m_coreProcess.runtimeSettingsFile());
}
