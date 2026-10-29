# M026：Mac 桌面样机的独立资源 owner 线程

2026-10-29。对应架构第 6、10.3、11、19、26 节。本阶段只调整显式手动桌面样机，验证原生资源模块的跨线程装配边界；**不是生产 display-agent 或完整恢复服务**。

## 实现与不变量

新增开发工具内部 LabMacResourceThread，在其 run 内构造和销毁 DurableResourceLedger、ResourceExecutor、MacResourceProvider。真实子进程监督、原生拓扑探测、意图落盘、配置、binding 核查与资源核查均留在该线程；GUI 不再同步调用这些对象。原有 provider/执行器的 owner thread 校验保持开启。

GUI 只读取带 mutex 的有类型快照，提交单槽源 binding 刷新或优先清理请求。线程以自己的 Qt 事件循环持续服务 helper 的 IPC/监督租约。资源等待期间的局部事件处理仅位于资源线程，不重入 GUI 的资源状态机。

MacCaptureBinding 通过快照交给 GUI 主线程的 SCK facade，只转移原有受限共享许可，不传裸平台对象或 ledger/provider 指针。媒体停止证据使用 acquire/release 原子标志；正常清理必须在 capture、引擎、接收租约排空后置位，ResourceExecutor 仍按原 owner 核查。清理请求立即撤销创建授权，再等待资源线程处理，不延长原绝对期限。

关闭/析构请求资源线程自行退出，不使用 QThread::terminate，也不销毁运行中的 QThread。该**开发工具**最多等待 10 秒；未确认 join 则退出整个诊断进程（76）并保留审计目录，不能把它当生产硬期限或成功回收。provider 可能在错误退场时结束耦合 helper，未核查的账本 owner 仍须保留恢复。这是生产进程隔离/恢复服务尚未完成的限制。

## 验证

- 原版线程装配的两轮真实桌面链路通过：GUI 创建阶段定时器分别运行 356/120 次；解码色块和 native paint 分别 74/69；两轮输出数量均 1→1、owner 关闭、采集 CPU 预算归零。
- 新增 `--cancel-before-create`：GUI 在资源线程尚未启动前撤销授权并排队清理。实际结果 ResourcePermitRejected、资源意图数 0、owner 关闭、屏幕数量 1→1。没有执行采集/媒体，不能替代运行中撤销验收。
- 后续一轮最新桌面验证出现 `RTP mapping rejected`，此前错误缺少引擎代码。本阶段保留该次失败并补充 frame/capture/epoch/topology/SSRC/RTP timestamp 和 rx/tx 错误码诊断，没有放宽契约或将重跑成功当成原因已解释。

最终诊断轮次和准确源码/二进制摘要见 [M026-native-results.json](M026-native-results.json)。随后明确错误是先发生 Profile 标签拒绝，相关引擎证据修正独立归 [M027](M027-h264-codec-evidence.zh-CN.md)，先行提交 `26bfa9e69`，没有把其代码混入本阶段。最终真实桌面轮次采集 131、解码/色块/native paint 各 79，GUI 创建阶段计时器 182 次，输出 1→1、owner 关闭、采集 CPU 预算为 0。后续成功不追溯证明旧失败码流合格。M025 的公共相关自动回归仍为 Mac/Ubuntu 各 133/133；本阶段仅改 Mac 手动工具，没有重复无关测试，也没有将这些手动结果计入自动测试数量。

## 限制与提交定位

- 线程隔离不等于原生进程隔离。GStreamer 和 SCK 仍在诊断进程中；生产 facade、IPC 媒体 Worker、状态机/mailbox 接线与恢复服务待完成。
- 不保证内核同步 I/O、文件校验和 Qt 原生启动都可中断；磁盘停顿仍可能影响资源线程自身的 helper 心跳。GUI 响应证据不能宣称原历史输出配置间歇失败已修复。
- NativePaintConfirmed 不代表 compositor fence，也不证明 1080p30、长时、弱网或双机；Windows 未编译实测。Mac 专用手动源码未同步到 Ubuntu 冒充平台认证。
- 工具不携带真实用户桌面截图，只核查自己源窗口的色块和本地帧标识。失败审计目录保留，只有完整正常清理及断言通过后才自动移除。

源码、真实运行结果和本备忘录同阶段提交；通过 git log --follow -- 本文件定位。
