// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "SetupAssistant.h"
#include "WorkspaceUi.h"
#include "common/Settings.h"
#include "gui/core/NetworkMonitor.h"
#include "gui/widgets/FingerprintPreview.h"
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <utility>
#ifdef Q_OS_MACOS
#include <ApplicationServices/ApplicationServices.h>
#endif
namespace deskflow::gui::workspace {
SetupAssistant::SetupAssistant(QWidget *parent) : QWidget(parent)
{
  setObjectName("setupAssistant");
  m_layout = new QVBoxLayout(this);
  m_layout->setContentsMargins(0, 0, 0, 0);
  m_layout->setSpacing(0);
}
void SetupAssistant::begin(
    bool source, bool chooseRole, const QStringList &existingPeers, const QString &savedAddress, int port
)
{
  if (m_identityPending)
    return;
  ++m_connectionRevision;
  ++m_identityRequest;
  m_active = true;
  m_source = source;
  m_chooseRole = chooseRole;
  m_existingPeers = existingPeers;
  m_address = savedAddress;
  m_port = port;
  m_attempt = QUuid::createUuid();
  m_attemptStarted = false;
  m_connected = false;
  m_localApproved = false;
  m_target.clear();
  m_requestedIdentifier.clear();
  m_failure.clear();
  m_identityIdentifier.clear();
  m_previousIdentity.clear();
  m_servers.clear();
  go(chooseRole ? Step::Role : Step::Permission);
}
bool SetupAssistant::permissionAvailable() const
{
#ifdef Q_OS_MACOS
  return AXIsProcessTrusted();
#else
  return true;
#endif
}
void SetupAssistant::go(Step step)
{
  m_step = step;
  render();
}
void SetupAssistant::startAttempt()
{
  m_attemptStarted = true;
  m_connected = false;
  m_failure.clear();
  go(m_source ? Step::Wait : Step::AwaitPeer);
  Q_EMIT connectRequested(m_source, m_address, m_port);
}
void SetupAssistant::setCandidates(const QUuid &attempt, const QVector<deskflow::mdns::DiscoveredServer> &servers)
{
  if (!m_active || attempt != m_attempt)
    return;
  m_servers = servers;
  if (m_step == Step::Discover)
    render();
}
void SetupAssistant::requestIdentity(
    const Fingerprint &peer, const Fingerprint &local, std::function<bool(bool)> decision
)
{
  if (m_identityPending)
    return;
  if (!m_active) {
    m_active = true;
    m_attempt = QUuid::createUuid();
    m_chooseRole = false;
    m_attemptStarted = true;
  }
  ++m_identityRequest;
  m_attemptStarted = true;
  m_identityIdentifier.clear();
  m_previousIdentity.clear();
  m_peer = peer;
  m_local = local;
  m_identityPending = true;
  m_localApproved = false;
  m_decision = std::move(decision);
  m_failure.clear();
  go(Step::Identity);
}
void SetupAssistant::setIdentityContext(const QString &identifier, const QByteArray &previous)
{
  m_identityIdentifier = identifier;
  m_previousIdentity = previous;
  if (m_step == Step::Identity)
    render();
}
bool SetupAssistant::showIncomingDevice(const QString &identifier)
{
  if (!m_active || !m_source || m_identityPending || (!m_target.isEmpty() && identifier != m_target) ||
      (!m_requestedIdentifier.isEmpty() && identifier != m_requestedIdentifier))
    return false;
  if (m_step == Step::RegisterDevice && identifier == m_requestedIdentifier)
    return true;
  m_requestedIdentifier = identifier;
  m_attemptStarted = true;
  go(Step::RegisterDevice);
  return true;
}
void SetupAssistant::waitForPeer()
{
  if (m_active && m_source && !m_identityPending)
    go(Step::AwaitPeer);
}
void SetupAssistant::observeConnected(const QString &identifier)
{
  if (!m_active || !m_attemptStarted || m_identityPending)
    return;
  if (m_source) {
    if (m_existingPeers.contains(identifier))
      return;
  } else if (identifier != m_address)
    return;
  if ((!m_target.isEmpty() && identifier != m_target) ||
      (!m_requestedIdentifier.isEmpty() && identifier != m_requestedIdentifier))
    return;
  if (!m_connected)
    ++m_connectionRevision;
  m_target = identifier;
  m_connected = true;
  m_failure.clear();
  if (m_step != Step::Verify && m_step != Step::Done)
    go(Step::Position);
}
void SetupAssistant::observeDisconnected(const QString &identifier)
{
  if (m_active && m_connected && m_target == identifier) {
    ++m_connectionRevision;
    m_connected = false;
    m_failure = tr("连接已中断，未记录输入成功。请重新连接后再验证。");
    if (m_step == Step::Verify || m_step == Step::Done)
      go(Step::AwaitPeer);
  }
}
void SetupAssistant::showFailure(const QString &message)
{
  if (!m_active)
    return;
  m_failure = message;
  render();
}
void SetupAssistant::cancel()
{
  if (!m_active)
    return;
  ++m_connectionRevision;
  ++m_identityRequest;
  m_active = false;
  auto decision = std::exchange(m_decision, {});
  m_identityPending = false;
  m_attempt = QUuid::createUuid();
  if (decision)
    decision(false);
  Q_EMIT canceled();
}
void SetupAssistant::showCompletion(bool verified, const QString &message)
{
  m_active = true;
  m_identityOnlyCompletion = false;
  m_completionVerified = verified;
  m_failure = message;
  go(Step::Done);
}
void SetupAssistant::showIdentityCompletion()
{
  m_active = true;
  m_identityOnlyCompletion = true;
  m_failure.clear();
  go(Step::Done);
}
void SetupAssistant::render()
{
  if (m_content) {
    m_layout->removeWidget(m_content);
    m_content->hide();
    m_content->deleteLater();
  }
  const QMap<Step, QString> titles{
      {Step::Role, tr("选择使用方式")},
      {Step::Permission, tr("检查系统权限")},
      {Step::Wait, tr("等待另一台电脑")},
      {Step::Discover, tr("找到键鼠来源")},
      {Step::Manual, tr("输入连接地址")},
      {Step::Identity, tr("核对设备身份")},
      {Step::AwaitPeer, tr("等待对方确认与连接")},
      {Step::RegisterDevice, tr("为新电脑保留位置")},
      {Step::Position, tr("安排电脑位置")},
      {Step::Verify, tr("验证跨屏输入")},
      {Step::Done, tr("本次设置完成")}
  };
  auto *cancelButton = button(m_step == Step::Done ? tr("返回设备") : tr("取消本次设置"));
  connect(cancelButton, &QPushButton::clicked, this, [this] {
    if (m_step == Step::Done) {
      m_active = false;
      Q_EMIT dismissed();
    } else
      cancel();
  });
  auto *body = page(titles.value(m_step), tr("设置与连接 · 身份核对、连接和输入验证分别确认"), cancelButton);
  m_error = label(m_failure, "error");
  m_error->setObjectName("setupError");
  m_error->setVisible(!m_failure.isEmpty());
  contents(body)->addWidget(m_error);
  auto addPanel = [body](const QString &title, const QString &description) {
    auto *box = panel();
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(16, 18, 16, 18);
    layout->setSpacing(12);
    layout->addWidget(label(title, "subtitle"));
    layout->addWidget(label(description, "muted"));
    contents(body)->addWidget(box);
    return layout;
  };
  if (m_step == Step::Role) {
    auto *source = button(tr("键盘与鼠标连接在这台电脑"), true);
    auto *receiver = button(tr("键盘与鼠标连接在另一台电脑"));
    source->setMinimumHeight(54);
    receiver->setMinimumHeight(54);
    contents(body)->addWidget(source);
    contents(body)->addWidget(receiver);
    contents(body)->addWidget(label(tr("日常更改角色会说明中断影响，再明确应用。"), "muted"));
    connect(source, &QPushButton::clicked, this, [this] {
      m_source = true;
      go(Step::Permission);
    });
    connect(receiver, &QPushButton::clicked, this, [this] {
      m_source = false;
      go(Step::Permission);
    });
  } else if (m_step == Step::Permission) {
    addPanel(
#ifdef Q_OS_MACOS
        permissionAvailable() ? tr("本机权限检查完成") : tr("需要允许控制输入"),
        permissionAvailable() ? tr("辅助功能权限已允许；连接后仍需要实际验证鼠标和键盘。")
                              : tr("请在系统设置中允许辅助功能访问。拒绝授权不会退出应用，可以稍后回诊断页继续。")
#else
        tr("等待后端授权与输入验证"),
        tr("输入能力取决于当前系统和后端授权。尚未验证的输入能力保持未知，连接后请实际验证。")
#endif
    );
    auto *actions = new QHBoxLayout;
    auto *guide = button(tr("查看授权指引"));
    auto *check = button(tr("重新检测"));
    auto *next = button(tr("继续"), true);
    actions->addWidget(guide);
    actions->addWidget(check);
    actions->addStretch();
    actions->addWidget(next);
    contents(body)->addLayout(actions);
    connect(guide, &QPushButton::clicked, this, &SetupAssistant::permissionHelpRequested);
    connect(check, &QPushButton::clicked, this, [this] { render(); });
    connect(next, &QPushButton::clicked, this, [this] {
      if (!permissionAvailable()) {
        showFailure(tr("权限尚未允许。请授权后重新检测，或取消后在诊断页继续。"));
        return;
      }
      if (m_source)
        startAttempt();
      else {
        go(Step::Discover);
        Q_EMIT discoveryRequested(m_attempt);
      }
    });
  } else if (m_step == Step::Wait) {
    auto *wait = addPanel(
        tr("在另一台电脑打开 Deskflow"),
        tr("选择“键盘与鼠标连接在另一台电脑”，再找到此电脑。来源端等待接收端发起连接。")
    );
    auto *address = label(
        tr("本机：%1\n可用地址：%2\n端口：%3")
            .arg(Settings::value(Settings::Core::ComputerName).toString(), NetworkMonitor::validAddresses().join(", "))
            .arg(m_port)
    );
    address->setTextInteractionFlags(Qt::TextSelectableByMouse);
    wait->addWidget(address);
    contents(body)->addWidget(label(tr("等待请求时不会中断已经建立的其他电脑连接。"), "muted"));
  } else if (m_step == Step::Discover) {
    auto *list = addPanel(
        tr("可用的键鼠来源"),
        m_servers.isEmpty()
            ? tr("正在查找。未发现电脑时，请确认同一网络、来源端已启动，并检查防火墙；也可以手动填写地址。")
            : tr("发现名称和广告指纹尚未经核对，不能作为身份证明。")
    );
    for (const auto &server : m_servers) {
      if (server.ipAddress.isEmpty() || server.port < 1 || server.port > 65535)
        continue;
      auto *select = button(tr("%1 · %2").arg(server.name, server.ipAddress));
      select->setAccessibleName(tr("选择来源 %1，地址%2").arg(server.name, server.ipAddress));
      list->addWidget(select);
      connect(select, &QPushButton::clicked, this, [this, server] {
        m_address = server.ipAddress;
        m_port = server.port;
        startAttempt();
      });
    }
    auto *actions = new QHBoxLayout;
    auto *manual = button(tr("手动输入地址"));
    auto *refresh = button(tr("刷新"));
    actions->addWidget(manual);
    actions->addWidget(refresh);
    actions->addStretch();
    contents(body)->addLayout(actions);
    connect(manual, &QPushButton::clicked, this, [this] { go(Step::Manual); });
    connect(refresh, &QPushButton::clicked, this, [this] { Q_EMIT discoveryRequested(m_attempt); });
  } else if (m_step == Step::Manual) {
    auto *address = new QLineEdit(m_address);
    address->setObjectName("setupAddress");
    address->setPlaceholderText(tr("主机名、IPv4 或 [IPv6]；不要填写协议 URL"));
    address->setAccessibleName(tr("来源地址"));
    contents(body)->addWidget(label(tr("电脑地址或主机名"), "subtitle"));
    contents(body)->addWidget(address);
    auto *port = new QSpinBox;
    port->setRange(1, 65535);
    port->setValue(std::clamp(m_port, 1, 65535));
    contents(body)->addWidget(row(tr("端口"), tr("保留来源端的实际监听端口"), port));
    connect(address, &QLineEdit::textChanged, this, [this](const QString &text) { m_address = text; });
    connect(port, &QSpinBox::valueChanged, this, [this](int value) { m_port = value; });
    auto *next = button(tr("连接并核对身份"), true);
    contents(body)->addWidget(next);
    connect(next, &QPushButton::clicked, this, [this] {
      m_address = m_address.trimmed();
      if (m_address.isEmpty() || m_address.contains("://")) {
        m_error->setText(tr("请输入主机名或 IP，不要填写带协议的 URL。"));
        m_error->show();
        return;
      }
      startAttempt();
    });
  } else if (m_step == Step::Identity) {
    auto *identity = addPanel(
        tr("请在另一台电脑上逐项核对完整指纹"), tr("显示名称与发现结果不能代替身份核对。图案与摘要来自同一身份数据。")
    );
    if (!m_identityIdentifier.isEmpty())
      identity->addWidget(label(tr("待绑定的连接标识：%1").arg(m_identityIdentifier), "subtitle"));
    if (!m_previousIdentity.isEmpty()) {
      identity->addWidget(
          label(tr("此设备的身份已经变化，连接保持阻断。请在设备上核实更换证书的原因，再决定是否替换记录。"), "error")
      );
      auto *old = label(
          tr("原指纹：%1").arg(QString::fromLatin1(QByteArray::fromHex(m_previousIdentity).toHex(':').toUpper())),
          "muted"
      );
      old->setTextInteractionFlags(Qt::TextSelectableByMouse);
      identity->addWidget(old);
    }
    auto *proof = new QHBoxLayout;
    proof->setSpacing(20);
    proof->addWidget(new FingerprintPreview(body, m_peer, tr("对端完整 SHA-256"), true), 1);
    proof->addWidget(new FingerprintPreview(body, m_peer, tr("身份图案"), false), 1);
    identity->addLayout(proof);
    auto *acknowledgment = new QCheckBox(tr("已在两台电脑上核对完整指纹，确认一致"));
    acknowledgment->setObjectName("setupIdentityAcknowledged");
    contents(body)->addWidget(acknowledgment);
    auto *actions = new QHBoxLayout;
    auto *local = button(tr("查看本机指纹…"));
    auto *confirm = button(tr("确认此设备身份"), true);
    confirm->setObjectName("confirmPeerIdentity");
    confirm->setEnabled(false);
    actions->addWidget(local);
    actions->addStretch();
    actions->addWidget(confirm);
    contents(body)->addLayout(actions);
    connect(acknowledgment, &QCheckBox::toggled, confirm, &QPushButton::setEnabled);
    connect(local, &QPushButton::clicked, this, &SetupAssistant::localFingerprintRequested);
    const auto identityRequest = m_identityRequest;
    connect(confirm, &QPushButton::clicked, this, [this, identityRequest] {
      if (m_step != Step::Identity || !m_identityPending || identityRequest != m_identityRequest)
        return;
      auto decision = std::exchange(m_decision, {});
      m_identityPending = false;
      const auto attempt = m_attempt;
      m_localApproved = true;
      go(Step::AwaitPeer);
      const auto accepted = decision && decision(true);
      if (attempt != m_attempt || !m_active || identityRequest != m_identityRequest)
        return;
      if (!accepted) {
        m_localApproved = false;
        m_identityPending = true;
        m_decision = std::move(decision);
        m_failure = tr("身份记录未能保存，尚未允许连接。请检查配置权限后重新发起。");
        go(Step::Identity);
      }
    });
  } else if (m_step == Step::RegisterDevice) {
    auto *request = addPanel(
        tr("收到新电脑的连接请求"),
        tr("请先在完整排列中为这台电脑保留位置。添加位置不代表已经连接，也不会记录输入成功。")
    );
    request->addWidget(label(tr("对端请求的连接标识：%1").arg(m_requestedIdentifier), "subtitle"));
    request->addWidget(label(tr("连接标识来自对端声明；设备身份仍以完整指纹核对和实际绑定为准。"), "muted"));
    auto *add = button(tr("设置电脑名称与位置…"), true);
    contents(body)->addWidget(add);
    connect(add, &QPushButton::clicked, this, [this] { Q_EMIT deviceRequested(m_requestedIdentifier); });
  } else if (m_step == Step::AwaitPeer) {
    addPanel(
        m_localApproved ? tr("本机已确认 · 等待对方确认") : tr("正在核对身份并建立连接"),
        tr("请在另一台电脑核对此电脑的完整指纹。只有收到核心的实际连接事件，才会进入下一步。")
    );
    auto *retry = button(tr("重试连接"));
    contents(body)->addWidget(retry);
    connect(retry, &QPushButton::clicked, this, &SetupAssistant::startAttempt);
  } else if (m_step == Step::Position) {
    addPanel(
        tr("连接已建立，输入尚未验证"), m_source ? tr("可以在完整排列中安排新电脑的位置；已有电脑和离线位置保持保留。")
                                                 : tr("这里仅显示本机与实际来源，完整排列由来源端管理。")
    );
    auto *actions = new QHBoxLayout;
    auto *arrange = button(tr("查看完整排列"));
    auto *next = button(tr("继续验证输入"), true);
    if (m_source)
      actions->addWidget(arrange);
    actions->addStretch();
    actions->addWidget(next);
    contents(body)->addLayout(actions);
    connect(arrange, &QPushButton::clicked, this, &SetupAssistant::arrangeRequested);
    const auto revision = m_connectionRevision;
    connect(next, &QPushButton::clicked, this, [this, revision] {
      if (m_connected && m_step == Step::Position && revision == m_connectionRevision)
        go(Step::Verify);
    });
  } else if (m_step == Step::Verify) {
    addPanel(
        tr("把光标移到另一台电脑，再试着输入"),
        tr("保持目标屏幕解锁，实际移动光标并输入测试文字。此处只记录人工确认，不读取或保存文字内容。")
    );
    auto *ack = new QCheckBox(tr("我确认目标电脑的鼠标与键盘输入均正常（人工验证）"));
    ack->setObjectName("setupInputAcknowledged");
    contents(body)->addWidget(ack);
    auto *actions = new QHBoxLayout;
    auto *later = button(tr("稍后验证"));
    auto *verified = button(tr("记录验证并完成"), true);
    verified->setEnabled(false);
    actions->addWidget(later);
    actions->addStretch();
    actions->addWidget(verified);
    contents(body)->addLayout(actions);
    connect(ack, &QCheckBox::toggled, verified, &QPushButton::setEnabled);
    connect(later, &QPushButton::clicked, this, [this] {
      m_active = false;
      Q_EMIT finished({});
    });
    const auto revision = m_connectionRevision;
    connect(verified, &QPushButton::clicked, this, [this, revision] {
      if (m_step != Step::Verify || revision != m_connectionRevision)
        return;
      if (!m_connected) {
        showFailure(tr("连接已不可用，未记录成功验证。"));
        return;
      }
      m_active = false;
      Q_EMIT finished(m_target);
    });
  }
  if (m_step == Step::Done) {
    if (m_identityOnlyCompletion)
      addPanel(
          tr("设备身份已重新核对"), tr("本次操作只更新身份核对记录。连接状态和已有人工输入验证仍以实际会话为准。")
      );
    else
      addPanel(
          m_completionVerified ? tr("本次输入已完成人工验证") : tr("输入仍保持未验证"),
          m_completionVerified ? tr("已记录本次连接的人工确认。连接中断或重新连接后，需要重新验证输入。")
                               : tr("可以稍后在设备页继续验证。这里不会将连接或身份核对当作输入成功。")
      );
  }
  contents(body)->addStretch();
  m_content = workspace::scroll(body);
  m_layout->addWidget(m_content);
}
} // namespace deskflow::gui::workspace
