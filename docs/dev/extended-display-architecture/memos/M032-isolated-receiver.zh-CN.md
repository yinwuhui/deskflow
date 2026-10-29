# M032：隔离接收 Worker 首次装配（开发入口）

## 边界、设计对应与实现

对应架构第 4、13、14、17、21、26 节。新增 busymouse-display-media-receiver：复用 WorkerProcess/WorkerConnection 和 NativeWebRtcEngine，在真实受监督子进程内完成 GStreamer 解码与 Qt 窗口绘制。只在显式私有 GStreamer SDK 配置下构建，不安装、不发布生产 available，不切换旧产品入口。

私有媒体 IPC 升为 display-worker/4、bootstrap/4；平台 IPC 保持 v1。增加 WorkerReceiveBinding（本地地址、远端源资源/拓扑、接收目标登记 ID）与现有 WebRtcFrameCorrelation 的控制编码复用。目标是本地创建的独立窗口，登记 ID 不被解释成 OS 句柄或物理屏幕；不支持任意导入窗口，也尚未接入用户选择显示器及生产目标登记表。

代码：src/apps/display-media-worker/Receiver.cpp、supervision/WorkerProtocol.*、WorkerProcess.cpp；真实进程诊断工具 NativeReceiverProcessSmoke.cpp。窗口中的 QImage cleanup context 保活 CPU 租约，复制图像不会提前释放解码帧。

## 不变量与停止

私有启动挑战、完整 WorkerBinding、现有有界 IPC/心跳/外部硬停止期限继续复用。帧映射要求接收方向、已启动、源资源/拓扑及 epoch 一致。准备消息和来源绑定由可信本地宿主提交，不接受网络指定原始像素、程序路径或 OS 目标句柄。

本地许可在接收绑定后产生，停止、父 EOF/监督租约失效撤销同一许可、隐藏窗口并释放持有帧；引擎排空后才报告 Quiesced。最终消息通过 socket 排空/断开后结束 Qt 循环，不能 app.quit 早于写入排空。Quiesced 不证明系统副屏消失，本 Worker 不创建副屏。

注意：许可签发仍依赖调用宿主先完成真实授权/屏障。本开发工具只提供诊断身份，不能被称作生产授权。接收许可最长 24 小时，并受现有 1.5 秒监督租约及停止约束；生产 owner 的授权有效期传播仍未接入。

## 实际测试与失败保留

Mac 构建了接收 Worker、真实进程媒体工具、协议/进程测试。命令（使用交接所述 CMake/PYTHONPATH）：

- cmake --build /private/tmp/deskflow-build --target busymouse-display-media-receiver NativeReceiverProcessSmoke DisplayWorkerProtocolTests DisplayWorkerProcessTests --parallel 4
- ctest --test-dir /private/tmp/deskflow-build/src/unittests -R '^Display.Infrastructure.(Worker|MediaWorkPermit)' --output-on-failure
- /private/tmp/deskflow-build/bin/NativeReceiverProcessSmoke /private/tmp/bmouse-gst-runtime-20261028 /private/tmp/deskflow-build/bin/busymouse-display-media-receiver

24 项定向回归通过（包含新增来源/epoch/拓扑映射、停止后拒绝、缺少绑定拒绝）。真实本机子进程最终 195 帧 NativePaintConfirmed，Quiesced=true、正常退出、源预算归零。这是 synthetic 发送端和真实 RTP/子进程绘制验证，不是实际扩展桌面或跨机验收。未重新运行完整 138 项回归。

一次重复定向回归 23/24，WorkerLegacyBootstrap 两秒夹具退出等待到期；最终重建后 v3 拒绝单项及完整定向回归 24/24 通过。保留该间歇启动/退出等待问题。

失败：首次诊断发送器多保留像素 owner，FramePool 拒绝 adopt，已修正；其后 201 帧绘制但最终消息未排空导致 EOF/强制结束，已修正；修复后一次冷启动触发现有 IpcHandshakeDeadline，重试通过，不抹去冷启动风险。初次沙箱构建 uic neon 特性检测失败，沙箱外相同 cache 构建通过。原始摘要见 M032-test-results.json。

## 产品调用链与后续主线

已确认 ProductDisplayRuntime 通过 CoreInputChannel::authenticatedConnectionObserved/peerControlReceived/sendPeerControl 接现有认证控制，通过 UsageDisplayBridge::receiverInputReady 接输入就绪，通过 PeerUsageService::connectionChanged 接断线。继续复用这些入口，不另建平行设备信任系统。

仍缺：真实认证控制的 display-infra/1 适配、双端计划提交和授权登记、生产接收目标登记、Mac 活跃输出证据跨进程交接及采集 Worker、Ubuntu 图形会话实测、媒体 Worker 活跃解码时父 EOF 专项、mailbox/产品 facade、用户断开/跨机清理验收。既有 WorkerParentEof 测试不能替代新媒体进程的 EOF 实测。Mac 冷 GUI 启动可能超过现有两秒握手期限，尚未修复。Windows 未编译/认证。

## 迁移与提交定位

旧运行时未切换；旧 v3 媒体 bootstrap 必须拒绝，相关进程测试更新为明确拒绝 v3。回滚须成套回滚协议及工具。实现、测试、此备忘录和结果同阶段提交；使用 git log --follow -- 本文件定位。
