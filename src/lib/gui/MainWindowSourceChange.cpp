// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "MainWindow.h"
#include <QPushButton>

void MainWindow::setupSourceChange()
{
  using Transaction = deskflow::gui::SourceChangeTransaction;
  m_sourceChangeTimer.setSingleShot(true);
  connect(&m_sourceChangeTimer, &QTimer::timeout, this, [this] {
    m_sourceChange.failed(m_sourceChange.attempt(), m_sourceChange.expectedSource(), tr("等待来源连接超时。"));
  });
  connect(m_cancelSourceChange, &QPushButton::clicked, this, [this] {
    if (m_sourceChange.phase() == Transaction::Phase::Candidate)
      m_sourceChange.failed(m_sourceChange.attempt(), m_sourceChange.expectedSource(), tr("用户取消新来源连接。"));
  });
  connect(&m_sourceChange, &Transaction::changed, this, [this] {
    m_cancelSourceChange->setVisible(m_sourceChange.active());
    m_cancelSourceChange->setEnabled(m_sourceChange.phase() == Transaction::Phase::Candidate);
    refreshWorkspace();
  });
  connect(&m_sourceChange, &Transaction::restoreRequested, this, [this] {
    m_sourceChangeTimer.stop();
    m_sourcePhaseStarted = false;
    m_lastOperationError = tr("新来源尚未连接，正在尝试恢复旧来源。");
    m_checkedServers.clear();
    if (!m_coreProcess.restoreRuntimeConfiguration(m_sourceBeforeChange)) {
      m_sourceChange.failed(
          m_sourceChange.attempt(), m_sourceChange.expectedSource(), tr("旧来源运行快照不可读，无法恢复。")
      );
      return;
    }
    m_activeSource = m_sourceBeforeChange.source;
    m_sourceChangeTimer.start(60000);
    m_coreProcess.restart();
    refreshWorkspace();
  });
  connect(&m_sourceChange, &Transaction::finished, this, [this](bool accepted, bool restored, const QString &reason) {
    m_sourceChangeTimer.stop();
    const auto message = accepted ? tr("已连接新来源；请重新验证输入。")
                         : restored
                             ? tr("新来源切换失败，已重新连接旧来源。新来源配置保持已保存、待应用。\n%1").arg(reason)
                         : m_sourceChange.phase() == Transaction::Phase::Aborted
                             ? reason
                             : tr("来源切换失败，未能确认恢复旧来源。已请求停止共享，请检查诊断。\n%1").arg(reason);
    if (m_applyingRevision) {
      m_applicationState.finishApply(m_applyingRevision, accepted, accepted ? QString() : message);
      m_applyingRevision = 0;
    }
    if (accepted) {
      m_activeExternalConfig = m_applyingExternalConfig;
      m_confirmedRuntimeFile = m_coreProcess.runtimeSettingsFile();
    }
    if (!accepted && !restored && m_sourceChange.phase() != Transaction::Phase::Aborted) {
      m_trustResumeAllowed = false;
      m_coreProcess.stop();
    }
    m_lastOperationError = message;
    persistWorkspaceState();
    refreshWorkspace();
  });
  m_cancelSourceChange->hide();
}
