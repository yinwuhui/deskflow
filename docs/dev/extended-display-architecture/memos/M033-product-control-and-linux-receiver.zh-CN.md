# M033：产品认证控制入口接线与 Ubuntu 隔离接收实测

## 阶段边界与设计对应

延续 M032，直接推进接收端可运行性及既有认证控制入口，未新增平行设备认证或网络栈。对应架构第 4、10、13、14、17、21、26、27 节及 INV-08/10/12。不是 Mac→Ubuntu 跨机闭环或产品入口替换验收。

## 实现与接口

- `InfrastructureControlChannel.*` 接现有 CoreInputChannel 的认证连接观察及控制事件、PeerUsageService 的断线事件；ProductDisplayRuntime 从现有本地证书 SHA256 身份创建适配器。只有 core 已观察的 peer/connection 才能引入绑定，收到任意 JSON 不能引入设备。
- 复用 ControlBinding 的双端 nonce、认证身份、实际本地连接引用和 display-infra/1 正文校验。连接替换/关闭移除原绑定并通知失效；错误连接、发送者、回放不发往后续 owner。上限 256 个连接，初始 hello 可能早于对端观察时通过首次有效 hello 回应补齐，不无限互相回应。
- CoreInputChannel::sendPeerControl 返回实际本地写入准入结果；按现有带 peer/connection 的外包装执行 16 KiB 上限，不放宽 core、键鼠网络或蓝牙预算；发送缓存包含新消息不得超过 64 KiB，异常部分写入关闭连接而不继续复用未提交序号。适配器预编码副本，只有写入准入成功才提交发送序号，超预算或断线返回 false。这仍不是远端接收确认，后续事务须处理断线/期限。
- `Receiver.cpp` 将 GStreamer 初始化、prepare/start、信令/映射、poll/stop 与引擎销毁放在同一媒体线程；窗口与监督 IPC 在 GUI 事件循环。媒体命令排队最多 72 项，向 GUI 待交付解码帧最多 1 个（另有窗口当前帧），借出帧继续按租约排空。停止标志独立于命令排队，许可在 GUI 停止边界立即撤销；停止后的晚到 native 错误不重新报告业务失败。
- `NativeReceiverProcessSmoke` 注册私有只读可执行映像，兼容 Ubuntu 构建目录的 group-writable umask，而不放宽 Worker 可执行路径政策。测试到 30 次真实窗口绘制时输出子进程 PID，供 `run_receiver_parent_loss.py` 在持有 Popen 进程身份期间杀死诊断父进程，验证接收 Worker 的真实父 EOF 退出。

## 实际测试（严格分层）

1. **构建**：Mac 与 Ubuntu 实际编译接收 Worker、产品控制适配器、core socket 集成测试；Mac 重建受影响 DisplayServiceTests/PeerUsageServiceTests。
2. **基础/产品服务回归**：Mac 139/139，通过（最终 19.20s）；Ubuntu 本轮 25/25 定向 Worker/许可/认证 core channel，通过（首轮 13.70s，最终结果另见归档）。Ubuntu 未宣称完整相关回归重跑。
3. **本地控制 socket 集成**：两端通过。测试实际 QLocalServer/Socket 和现有 CoreInputChannel 解析链；模拟 core 提供已认证记录，不是真实远端 TLS 身份认证。验证绑定摘要、错误连接、回放、16 KiB 超预算不消耗序号、core 断线失效。接收合法 Stop 仅通知信号，没有伪造业务授权或启动媒体。
4. **真实隔离媒体/可见呈现**：Mac 最终 202 帧、Ubuntu 活动用户 Wayland 会话最终 120 帧 NativePaintConfirmed；均收到 Quiesced、进程正常退出、源帧预算归零。两端各自使用 synthetic 本机发送端、标准 WebRTC RTP，非跨机媒体。未创建系统副屏，不能据此声称输出回收完成。
5. **真实故障**：两端分别在接收窗口确认至少 30 帧后杀死诊断父进程，接收进程在五秒观察期限内退出。该项验证真实接收媒体进程的 EOF 路径；不证明控制断线与生产授权撤销已接到此 Worker，也不证明虚拟输出消失。

复现命令使用交接 cache 与私有 SDK：

- Mac：`NativeReceiverProcessSmoke /private/tmp/bmouse-gst-runtime-20261028 /private/tmp/deskflow-build/bin/busymouse-display-media-receiver`；故障测试 `python3 tools/display-infrastructure-lab/run_receiver_parent_loss.py /private/tmp/deskflow-build/bin/NativeReceiverProcessSmoke /private/tmp/bmouse-gst-runtime-20261028 /private/tmp/deskflow-build/bin/busymouse-display-media-receiver`。
- Ubuntu：仓库内 `XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland build/bin/NativeReceiverProcessSmoke /tmp/bmouse-gst-runtime-20261029 build/bin/busymouse-display-media-receiver`；同样环境运行故障 Python 脚本。
- 回归：Mac `ctest --test-dir /private/tmp/deskflow-build/src/unittests -R '^Display\.(Infrastructure\.|Service\.|UsageService\.)' --output-on-failure`；Ubuntu `ctest --test-dir build/src/unittests -R '^Display.Infrastructure.(Worker|MediaWorkPermit|AuthenticatedCoreChannel)' --output-on-failure`。

原始日志和失败记录见 M033-test-results.json。

## 失败、修复与同步审计

- Ubuntu 首次诊断被 WorkerRegistration 拒绝，采用严格私有映像副本后消除，未改变安装包准入政策。
- Ubuntu 首次 GUI 运行因媒体准备阻塞 IPC，IpcLeaseExpired；不是无图形会话。迁移媒体线程后先 118 帧、最终 120 帧呈现/排空通过。
- Mac 线程版首次在 183 帧后撤销时收到晚到 ReceiverMediaFailed，随后正常排空。增加终止状态过滤后最终 202 帧正常撤销通过。历史 M030 queue 错误根因仍未解决，不能由本次成功消除。
- Mac 父 EOF 测试线程版一轮在达到 30 帧之前失败，当轮脚本未收集完整 stderr，不能追溯断言根因。已补充失败转录，重试通过。既有冷 Qt 启动风险保留；生产 WorkerProtocol 握手实际期限为三秒（M032 文中“两秒”混同了夹具等待），本阶段未放宽。
- 远端基线 `886aa83eb` 且存在既有修改；M032 七个已有目标逐个与 Mac a18f3014f SHA256 一致，两个新源码要求不存在，才同步九个明确源码/测试文件。后续本阶段目标逐个与 2ecc8be05 或刚同步版本完整摘要一致才写入；其余 ProductDisplayRuntime/MediaSupervisor/原型等既有远端工作保留，不 reset、不覆盖历史。一次同步被自动审批拒绝，改为写入前强制校验全部目标后获得批准。
- 私有映像在父进程被 SIGKILL 时可能作为 /tmp 诊断残留保留；故障脚本不按孤儿 PID 强杀或伪报系统资源清理。

## 未接入项与下一步

产品现在实例化了认证控制绑定适配器，但 envelopeReceived 尚未接入生产会话 owner/mailbox，bindingLost 尚未连到生产媒体会话。不能把收到合法 PlanCommit JSON 当作真实授权，未签发任何新生产发送许可、未替换旧运行时或发布 native backend available。

下一步主线仍是双端计划引用/提交、真实用户授权及输入隔离登记与媒体 owner 接线，然后 Mac 活跃输出证据交给发送 Worker（子端独立 native 身份/拓扑核查），再形成 Mac→Ubuntu 真实扩展桌面跨机呈现及断线输出回收。当前16 KiB控制预算会明确拒绝较大SDP；未实现分片或改变契约可用上限，后续计划必须处理该拒绝。

Windows 未编译、未实测。未进行跨机、长时、弱网、锁屏/睡眠/热插拔或签名安装认证。

## 提交定位与回滚

源码、集成/故障测试、结果与本备忘录同阶段提交，使用 git log --follow -- 本文件定位。回滚适配器时同时回滚 ProductDisplayRuntime 创建点与 CoreInputChannel 返回结果接口；旧媒体入口仍保留。本阶段没有再次改变媒体 IPC 版本。
