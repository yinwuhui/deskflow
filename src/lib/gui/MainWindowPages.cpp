// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "MainWindow.h"
#include "common/UrlConstants.h"
#include "common/VersionInfo.h"
#include "gui/dialogs/FingerprintDialog.h"
#include "gui/widgets/LogDock.h"
#include "gui/widgets/LogWidget.h"
#include "net/FingerprintDatabase.h"
#include "net/PeerIdentityStore.h"
#include "workspace/SettingsWorkspace.h"
#include "workspace/SetupAssistant.h"
#include "workspace/WorkspaceUi.h"
#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QStackedWidget>
#include <QStyleHints>
#include <QSysInfo>
#include <QTextDocument>
using namespace deskflow::gui;
using namespace deskflow::gui::workspace;
void MainWindow::navigateSettings(const QString &section)
{
  m_navigation->setCurrentRow(1);
  m_settingsWorkspace->navigate(section);
}
void MainWindow::refreshWorkspaceTheme()
{
  if (auto *root = centralWidget()) {
    applyTheme(root);
    m_navigation->refreshIcons();
    root->update();
  }
}
QWidget *MainWindow::createDiagnosticsPage()
{
  auto *check = button(tr("重新检查"));
  auto *result = page(tr("连接诊断"), tr("按权限、网络、身份、会话、输入的顺序检查，先给出下一步。"), check);
  auto *box = panel();
  auto *layout = new QVBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  const QList<QPair<QString, QString>> steps{
      {"permission", tr("系统权限")},
      {"network", tr("本机网络")},
      {"identity", tr("设备身份与加密")},
      {"session", tr("会话连接")},
      {"input", tr("跨屏输入")}
  };
  for (const auto &step : steps) {
    auto *status = label({}, "muted");
    m_diagnosticLabels.insert(step.first, status);
    auto *action = button(
        step.first == "input"      ? tr("验证输入")
        : step.first == "identity" ? tr("管理信任")
        : step.first == "network"  ? tr("网络设置")
        : step.first == "session"  ? tr("设备与连接")
                                   : tr("查看指引")
    );
    auto *entry = row(step.second, {}, action);
    auto *text = qobject_cast<QHBoxLayout *>(entry->layout())->itemAt(0)->layout();
    text->addWidget(status);
    layout->addWidget(entry);
    if (step != steps.last()) {
      auto *line = new QFrame;
      line->setProperty("separator", true);
      layout->addWidget(line);
    }
    connect(action, &QPushButton::clicked, this, [this, key = step.first] {
      if (key == "identity")
        showTrust();
      else if (key == "network")
        navigateSettings("network");
      else if (key == "input")
        verifySelectedInput();
      else if (key == "session")
        m_navigation->setCurrentRow(0);
      else {
#ifdef Q_OS_MACOS
        QDesktopServices::openUrl(QUrl("x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")
        );
#else
    QMessageBox::information(this,tr("输入权限指引"),tr("请检查当前桌面会话的输入权限与后端。连接后需要在目标电脑上实际验证输入。"));
#endif
      }
    });
  }
  contents(result)->addWidget(box);
  m_diagnosticStatus = label({}, "error");
  contents(result)->addWidget(m_diagnosticStatus);
  auto *checkedAt = label(tr("尚未手动检查；运行状态会随核心事件更新。"), "muted");
  contents(result)->addWidget(checkedAt);
  connect(check, &QPushButton::clicked, this, [this, checkedAt] {
    refreshWorkspace();
    checkedAt->setText(tr("本机检查时间：%1。对端可达性和实际输入需分别验证。")
                           .arg(QLocale().toString(QDateTime::currentDateTime(), QLocale::ShortFormat)));
  });
  auto *actions = new QHBoxLayout;
  actions->addStretch();
  auto *logs = button(tr("查看技术日志"));
  auto *report = button(tr("预览诊断报告…"));
  actions->addWidget(logs);
  actions->addWidget(report);
  contents(result)->addLayout(actions);
  connect(logs, &QPushButton::clicked, this, [this] { m_diagnosticsPages->setCurrentIndex(1); });
  connect(report, &QPushButton::clicked, this, &MainWindow::exportDiagnosticReport);
  contents(result)->addStretch();
  return result;
}
QWidget *MainWindow::createLogsPage()
{
  auto *detach = button(tr("在独立窗口打开"));
  auto *result = page(tr("技术日志"), tr("保留完整日志等级、查找、文件记录和独立窗口。"), detach);
  auto *searchRow = new QHBoxLayout;
  auto *search = new QLineEdit;
  search->setAccessibleName(tr("查找技术日志"));
  search->setPlaceholderText(tr("查找名称、错误或关键字"));
  auto *previous = button(tr("上一处"));
  auto *next = button(tr("下一处"));
  auto *matches = label(tr("输入关键字查找"), "muted");
  searchRow->addWidget(search, 1);
  searchRow->addWidget(previous);
  searchRow->addWidget(next);
  searchRow->addWidget(matches);
  contents(result)->addLayout(searchRow);
  auto *log = new LogWidget(result, m_logDock->document());
  log->setMinimumHeight(280);
  contents(result)->addWidget(log, 1);
  connect(next, &QPushButton::clicked, log, [log, search] { log->findNext(search->text()); });
  connect(previous, &QPushButton::clicked, log, [log, search] { log->findPrevious(search->text()); });
  connect(search, &QLineEdit::returnPressed, next, &QPushButton::click);
  connect(search, &QLineEdit::textChanged, log, [log](const QString &text) { log->findNext(text); });
  connect(log, &LogWidget::searchResult, matches, [matches](int count) {
    matches->setText(QObject::tr("%1处匹配").arg(count));
  });
  auto *actions = new QHBoxLayout;
  auto *back = button(tr("返回连接诊断"));
  auto *settings = button(tr("日志等级与文件设置…"));
  auto *exportLog = button(tr("导出日志预览…"));
  actions->addWidget(back);
  actions->addStretch();
  actions->addWidget(settings);
  actions->addWidget(exportLog);
  contents(result)->addLayout(actions);
  connect(back, &QPushButton::clicked, this, [this] { m_diagnosticsPages->setCurrentIndex(0); });
  connect(settings, &QPushButton::clicked, this, [this] { navigateSettings("logs"); });
  connect(detach, &QPushButton::clicked, this, [this] {
    m_logDock->setFloating(true);
    m_logDock->show();
    m_logDock->raise();
  });
  connect(exportLog, &QPushButton::clicked, this, [this] {
    auto text = m_logDock->document()->toPlainText();
    for (const auto &computer : std::as_const(m_serverConfig).computers())
      if (!computer.isNull())
        text.replace(computer.name(), tr("[电脑]"));
    for (const auto &address : NetworkMonitor::validAddresses())
      text.replace(address, tr("[本机地址]"));
    for (const auto &address : m_activeSource.split(',', Qt::SkipEmptyParts))
      text.replace(address, tr("[来源地址]"));
    text.replace(QRegularExpression("\\b(?:\\d{1,3}\\.){3}\\d{1,3}\\b"), tr("[地址]"));
    QDialog preview(this);
    preview.setObjectName("workspaceRoot");
    preview.setWindowTitle(tr("日志导出预览"));
    preview.resize(720, 520);
    auto *layout = new QVBoxLayout(&preview);
    layout->addWidget(label(tr("已隐藏已知设备名称与地址。请检查预览内容；不会自动上传。"), "muted"));
    auto *output = new QPlainTextEdit(text);
    output->setReadOnly(true);
    layout->addWidget(output, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &preview, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &preview, &QDialog::reject);
    applyTheme(&preview);
    if (preview.exec() != QDialog::Accepted)
      return;
    const auto path = QFileDialog::getSaveFileName(this, tr("保存日志"), "deskflow-log.txt", tr("文本日志 (*.txt)"));
    if (path.isEmpty())
      return;
    QSaveFile file(path);
    const auto data = text.toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
      QMessageBox::warning(this, tr("导出失败"), tr("日志文件未能完整保存，请检查路径和权限。"));
  });
  return result;
}
QWidget *MainWindow::createHelpPage()
{
  auto *result = page(tr("关于 Deskflow"), tr("版本信息、开源许可、贡献者与更新。"));
  auto *copy = button(tr("复制版本信息"));
  auto *box = panel();
  auto *layout = new QVBoxLayout(box);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(row(
      "Deskflow",
      tr("%1 · %2 · %3").arg(kDisplayVersion, QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture()), copy
  ));
  auto *update = button(tr("检查更新…"));
  auto *status = label(tr("不会自动下载或安装；更新检查失败不影响共享。"), "muted");
  auto *updateRow = row(tr("检查更新"), {}, update);
  qobject_cast<QHBoxLayout *>(updateRow->layout())->itemAt(0)->layout()->addWidget(status);
  layout->addWidget(updateRow);
  contents(result)->addWidget(box);
  auto *download = button(tr("打开官方下载页面"));
  contents(result)->addWidget(download, 0, Qt::AlignLeft);
  connect(download, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(kUrlDownload)); });
  auto *licenses = button(tr("查看开源许可与第三方致谢…"));
  auto *help = button(tr("使用帮助"));
  contents(result)->addWidget(row(tr("许可与贡献者"), tr("GPL-2.0-only WITH LicenseRef-OpenSSL-Exception"), licenses));
  contents(result)->addWidget(row(tr("连接与使用说明"), tr("查看本地帮助文档"), help));
  connect(copy, &QPushButton::clicked, this, [] {
    QApplication::clipboard()->setText(
        QString("Deskflow %1\n%2\n%3")
            .arg(kDisplayVersion, QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture())
    );
  });
  connect(licenses, &QPushButton::clicked, this, &MainWindow::openAboutDialog);
  connect(help, &QPushButton::clicked, this, &MainWindow::showHelpViewer);
  connect(update, &QPushButton::clicked, this, [this, status, update] {
    update->setEnabled(false);
    status->setText(tr("正在检查更新…"));
    m_versionChecker.checkLatest();
  });
  connect(
      &m_versionChecker, &VersionChecker::checkFinished, status,
      [status, update](bool success, const QString &version, const QString &reason) {
        update->setEnabled(true);
        status->setText(
            !success            ? reason
            : version.isEmpty() ? QObject::tr("当前版本已是最新可用版本。 ")
                                : QObject::tr("发现新版本：%1。请主动打开发布页面下载。").arg(version)
        );
      }
  );
  contents(result)->addStretch();
  return result;
}
QWidget *MainWindow::createTrustPage()
{
  auto *result = page(tr("设备信任"), tr("连接状态、身份信任和输入验证分别记录。撤销不会删除排列。"));
  m_trustContent = new QWidget;
  m_trustRows = new QVBoxLayout(m_trustContent);
  m_trustRows->setContentsMargins(0, 0, 0, 0);
  m_trustRows->setSpacing(12);
  contents(result)->addWidget(m_trustContent);
  contents(result)->addWidget(
      label(tr("旧记录可能只有指纹，无法可靠关联设备时会明确标记。共用同一证书的连接属于同一身份。"), "muted")
  );
  return result;
}
void MainWindow::refreshTrustPage()
{
  if (!m_trustRows)
    return;
  QByteArray signature;
  QList<PeerIdentityStore::Record> bindings;
  PeerIdentityStore store(Settings::peerIdentityDb());
  const bool bindingsAvailable = store.records(bindings);
  QFile identityFile(Settings::peerIdentityDb());
  if (identityFile.open(QIODevice::ReadOnly))
    signature += identityFile.readAll();
  for (auto it = m_applicationState.sessions().cbegin(); it != m_applicationState.sessions().cend(); ++it) {
    signature += it.key().toUtf8() + it->attempt.toByteArray() + QByteArray::number(it->connected) +
                 it->verificationAttempt.toByteArray();
  }
  QList<QPair<QString, Fingerprint>> entries;
  for (const auto &path :
       {Settings::tlsTrustedServersDb(), Settings::tlsTrustedClientsDb(), Settings::tlsRevokedDb()}) {
    QFile file(path);
    if (file.open(QIODevice::ReadOnly))
      signature += file.readAll();
    FingerprintDatabase db;
    db.read(path);
    for (const auto &fp : db.fingerprints())
      entries.append({path, fp});
  }
  for (const auto &computer : std::as_const(m_serverConfig).computers())
    signature += computer.name().toUtf8() + computer.displayName().toUtf8() + computer.stableId().toUtf8();
  const auto digest = QCryptographicHash::hash(signature, QCryptographicHash::Sha256);
  if (digest == m_trustDigest)
    return;
  m_trustDigest = digest;
  while (auto *item = m_trustRows->takeAt(0)) {
    if (item->widget())
      item->widget()->deleteLater();
    delete item;
  }
  if (entries.isEmpty()) {
    bool hasPeers = false;
    for (const auto &computer : std::as_const(m_serverConfig).computers())
      if (!computer.isNull() && !computer.isServer()) {
        auto *card = panel();
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(8);
        layout->addWidget(label(computer.displayName(), "subtitle"));
        layout->addWidget(label(tr("尚未核对身份 · 输入未验证"), "muted"));
        layout->addWidget(label(tr("预先添加的位置不代表已连接或已信任。连接请求到达后核对完整指纹。"), "muted"));
        m_trustRows->addWidget(card);
        hasPeers = true;
      }
    if (!hasPeers)
      m_trustRows->addWidget(
          label(tr("还没有已核对的设备。在设备页添加电脑并完成双方身份核对后，记录会显示在这里。"), "muted")
      );
    return;
  }
  for (const auto &entry : entries) {
    const auto path = entry.first;
    const auto fp = entry.second;
    const bool revoked = path == Settings::tlsRevokedDb();
    auto *card = panel();
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);
    layout->addWidget(label(
        path == Settings::tlsTrustedServersDb() ? tr("键鼠来源身份")
        : revoked                               ? tr("已撤销身份")
                                                : tr("接收端身份"),
        "subtitle"
    ));
    QStringList names;
    QStringList peerStates;
    for (const auto &binding : bindings) {
      if (binding.fingerprint != fp.data.toHex() ||
          (!revoked && ((binding.role == "server") != (path == Settings::tlsTrustedServersDb()))))
        continue;
      QString name = binding.identifier;
      QString sessionIdentifier = binding.identifier;
      for (const auto &computer : std::as_const(m_serverConfig).computers())
        if (!computer.isNull() && computer.stableId() == binding.key) {
          name = computer.displayName();
          sessionIdentifier = computer.name();
          break;
        }
      names.append(name);
      const auto session = m_applicationState.sessions().value(sessionIdentifier);
      const bool verified =
          session.connected && session.verificationAttempt == session.attempt && session.manuallyVerifiedAt.isValid();
      peerStates.append(tr("%1 · %2 · %3")
                            .arg(
                                name, session.connected ? tr("已连接") : tr("离线"),
                                verified ? tr("本次输入已人工验证") : tr("输入未验证")
                            ));
    }
    if (!names.isEmpty()) {
      layout->addWidget(label(names.join(tr("、")), "subtitle"));
      layout->addWidget(label(peerStates.join("\n"), "muted"));
    }
    layout->addWidget(label(
        revoked              ? tr("已阻止此身份重新连接")
        : !bindingsAvailable ? tr("设备关联记录不可读，连接保持阻断")
        : names.isEmpty()    ? tr("身份曾核对 · 旧记录尚无可靠设备名称关联")
                             : tr("设备身份与完整指纹已绑定"),
        "muted"
    ));
    const auto text = QString::fromLatin1(fp.data.toHex(':').toUpper());
    auto *hash = label(text);
    hash->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    hash->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(hash);
    auto *actions = new QHBoxLayout;
    actions->addStretch();
    auto *copy = button(tr("复制完整指纹"));
    auto *reverify = button(tr("重新核对"));
    auto *revoke = button(tr("撤销信任…"));
    revoke->setProperty("danger", true);
    revoke->setEnabled(!revoked && Settings::isWritable());
    reverify->setEnabled(Settings::isWritable());
    actions->addWidget(copy);
    actions->addWidget(reverify);
    actions->addWidget(revoke);
    layout->addLayout(actions);
    connect(copy, &QPushButton::clicked, this, [text] { QApplication::clipboard()->setText(text); });
    connect(revoke, &QPushButton::clicked, this, [this, fp, path, text] {
      if (QMessageBox::warning(
              this, tr("撤销此设备身份"),
              tr("撤销以下完整指纹并断开对应连接？同一证书的连接会一起受影响。\n%1").arg(text),
              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel
          ) != QMessageBox::Yes)
        return;
      FingerprintDatabase revoked;
      revoked.read(Settings::tlsRevokedDb());
      revoked.addTrusted(fp);
      if (!revoked.write(Settings::tlsRevokedDb())) {
        m_lastOperationError = tr("撤销记录保存失败，原信任未修改。");
        refreshWorkspace();
        return;
      }
      m_lastOperationError.clear();
      FingerprintDatabase trusted;
      trusted.read(path);
      trusted.removeTrusted(fp);
      if (!trusted.write(path))
        m_lastOperationError = tr("撤销已记录，但旧信任列表未能清理。请检查配置权限。");
      if (m_coreProcess.isStarted())
        m_coreProcess.control("revoke", QString::fromLatin1(fp.data.toHex()));
      if (m_lastOperationError.isEmpty())
        m_lastOperationError = tr("信任已撤销，连接终止结果以核心回执和实际设备状态为准。");
      m_trustDigest.clear();
      refreshTrustPage();
      refreshWorkspace();
    });
    connect(reverify, &QPushButton::clicked, this, [this, fp, bindings] {
      if (m_setupAssistant->identityPending()) {
        showSetupAssistant();
        return;
      }
      QStringList roles;
      QString identifier;
      for (const auto &binding : bindings)
        if (binding.fingerprint == fp.data.toHex()) {
          if (!roles.contains(binding.role))
            roles.append(binding.role);
          identifier = binding.identifier;
        }
      QString role;
      if (roles.size() == 1)
        role = roles.first();
      else {
        bool accepted = false;
        const auto choice = QInputDialog::getItem(
            this, tr("选择核对的设备类型"), tr("旧撤销记录无法唯一确认设备类型。请根据对端的实际用途选择。"),
            {tr("键鼠来源身份"), tr("接收输入设备身份")}, 0, false, &accepted
        );
        if (!accepted)
          return;
        role = choice == tr("键鼠来源身份") ? "server" : "client";
      }
      const bool source = role == "client";
      m_setupStartedCore = false;
      m_setupAssistant->begin(
          source, false, m_connectedDevices,
          source ? QString() : Settings::value(Settings::Client::RemoteHost).toString(),
          Settings::value(Settings::Core::Port).toInt()
      );
      m_setupAssistant->requestIdentity(fp, m_fingerprint, [this, fp, source](bool accepted) {
        if (!accepted)
          return false;
        const auto path = source ? Settings::tlsTrustedClientsDb() : Settings::tlsTrustedServersDb();
        FingerprintDatabase trusted;
        trusted.read(path);
        trusted.addTrusted(fp);
        if (!trusted.write(path)) {
          m_lastOperationError = tr("核对记录保存失败，身份保持阻断。");
          refreshWorkspace();
          return false;
        }
        FingerprintDatabase revoked;
        revoked.read(Settings::tlsRevokedDb());
        revoked.removeTrusted(fp);
        const bool saved = revoked.write(Settings::tlsRevokedDb());
        m_lastOperationError =
            saved ? tr("已重新核对。请查看实际连接状态，再单独验证输入。") : tr("撤销记录未能清除，此身份仍受阻断。");
        m_trustDigest.clear();
        if (saved)
          m_setupAssistant->showIdentityCompletion();
        refreshTrustPage();
        refreshWorkspace();
        return saved;
      });
      m_setupAssistant->setIdentityContext(identifier, {});
      m_setupArranging = false;
      m_navigation->setCurrentRow(0);
      m_deviceModes->setCurrentIndex(2);
    });
    m_trustRows->addWidget(card);
  }
}
