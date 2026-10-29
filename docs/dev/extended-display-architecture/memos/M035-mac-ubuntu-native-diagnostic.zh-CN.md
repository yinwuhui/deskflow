# M035：Mac→Ubuntu 真实副屏跨机诊断闭环

## 边界与设计对应

对应架构的控制/媒体分离、来源身份、Worker 隔离、呈现证据和停止回收要求。Mac 真实新增扩展桌面由隔离发送 Worker 采集编码，经局域网标准 WebRTC 到 Ubuntu 隔离接收 Worker，在真实 Wayland 会话绘制。控制使用 SSH 主机认证及诊断宿主断言的会话身份；不是产品设备认证、双端授权/输入屏障或 PlanCommit 验收。产品入口未切换。

## 实现与不变量

新增 NativeRemoteReceiverAgent 与 LabReceiver 诊断代理，复用既有 WorkerProcess、私有挑战、受控可执行副本、媒体信令/帧关联编码与有界队列。逐帧像素不进入 SSH；Ubuntu 本地解码并绘制。固定 diagnostic profile 只在实际解码像素为绿色且完成 native paint 时计数，其他 profile 行为保持原样。

远端启动有期限，stdin EOF/stop 会撤销 Worker；在途信令不能覆盖最初错误。清理分别报告 Quiesced、正常进程退出、业务错误；Mac 独立回收来源，不因接收端失败跳过回收。媒体线程错误只保留固定安全标识及 factory/domain/code，不记录 SDP 或桌面内容。

Mac 原生布局曾在拓扑变化后仍沿用第一次计算的右侧位置。现在只重配本 owner 的精确 native 输出，至多三次，要求完整拓扑连续稳定250ms及原有全部身份/格式/布局检查后才 ready；原整体启动/授权期限不延长。来源绑定后拓扑改变仍按设计停止，不能隐式换源。

新增诊断 --recover-ledger 模式：仅同用户/同启动身份、原 helper 的完整出生身份确认已不存在、原资源命名空间不存在后，调用现有 provider/executor 独立回收及关闭账本；不按 PID 杀进程、不删除其他输出，保留账本。已无 active owner 仅报告幂等账本结果，不冒充再次证明系统资源不存在。

## 实际测试

Mac 编译平台 helper、发送/接收 Worker、NativeMacWebRtcLoopback；Ubuntu 编译接收 Worker 和 NativeRemoteReceiverAgent。远端每次先核对工作区及所有目标 SHA256，仅同步本阶段明确文件，保留原有修改和历史。

命令入口（运行时及路径见工具 README）：

```
NativeMacWebRtcLoopback <platform-helper> <runtime> --sender-worker=<sender-worker> --ubuntu-receiver
NativeMacWebRtcLoopback <platform-helper> <runtime> --sender-worker=<sender-worker> --ubuntu-disconnect
ctest --test-dir build/src/unittests -R '^Display.Infrastructure.(Worker|MediaWorkPermit)' --output-on-failure
```

最终固定布局正常连接：本地收到245次 Ubuntu 绿色像素 native paint，远端最终246；双端 Quiesced/正常退出，副屏不存在、owner关闭、Mac显示器数量1→1。最终控制断线：30次真实呈现后关闭控制输入，双端正常停止，来源回收/owner关闭，数量1→1。Ubuntu v5 Worker/许可26/26通过，14.75s。Mac此前M034相应26项通过；本阶段不以此声称完整产品回归。

原始日志均在 M035-test-results.json。最初248次呈现发生在加入绿色像素核查前，仅保留为旧证据。开发中多次布局/来源启动失败、32帧后的 RemoteInputState、123帧后的 RemoteAgentAbnormalExit 均保留；后者当时缺少退出详情，不能倒推根因。用户确认测试期间有其他软件调整显示布局并可暂停；固定布局后的两轮才是最终结果。历史M030 queue错误未证明解决。

一次旧版接收排空失败曾留下 active ledger，已按上述保守恢复确认原 helper/输出不存在并关闭；其余失败账本只读核查均 closed、无对应原生输出。核查时其他软件有显示器存在，未修改它们。原始失败现场与日志保留，临时跟踪代码已移除。

## 限制、影响与提交定位

这是真实跨机诊断媒体闭环，尚不是产品授权链验收。未认证实际30fps、长时/弱网/内存预算、热插拔/锁屏/睡眠、硬崩溃启动恢复、反方向或Windows。接收目标仍为本地诊断窗口，生产目标管理待接线。下一步将真实认证连接、用户授权及核心输入隔离事实接入生产计划 owner，再迁移入口。

代码、结果和此备忘录同阶段提交；使用 git log --follow -- 本文件定位。回滚需成套回滚工具代理/Agent及诊断像素证据，保留现场账本；旧产品入口不受替换。
