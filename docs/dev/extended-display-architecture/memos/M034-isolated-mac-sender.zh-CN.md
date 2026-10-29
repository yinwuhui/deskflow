# M034：真实 Mac 副屏的隔离采集/发送 Worker

## 边界与设计对应

对应架构第 4、11、13、14、17、19、21、26 节与 INV-03/07/08/10。首次将真实副屏的 SCK 采集、编码和媒体网络运行在独立发送 Worker；平台资源所有者继续独立持有输出。生产 PlanCommit、真实用户授权和输入屏障仍待接线，本阶段使用既有明确诊断授权，不发布生产 available。

## 实现与不变量

- `WorkerSourceLease` 仅能由完成私有启动挑战及 native 父身份检查的 WorkerConnection，在验证 Prepare 后构造。不能从裸 CGDisplay 数字/JSON 直接构造；每个连接只能取出一次，副本共享单次 capture claim 和 live 原子标志。Stop、父 EOF、租约错误及析构使来源证据失效。
- MacCaptureBinding::claimWorkerSource 在可信父 owner 交付来源前消费原 capture claim，不能再在父进程或从副本多开采集。普通 fillWorkerSource 保留无副作用查询语义。
- 子端 MacCaptureSource 接受该私有来源租约及完整匹配媒体许可，独立核查实际当前用户会话、完整拓扑摘要、CGDisplay/UUID、资源 owner/resource 派生 serial 与虚拟输出 vendor/product；沿用 start 中 active/non-mirror/1920×1080、桌面锁定和 SCK 精确匹配核查。serial 不是单独授权依据。没有任意编号公共绑定工厂。
- 新 `busymouse-display-media-sender`：GUI/native 主事件循环运行 SCK 与监督 IPC，`NativeMediaThread` 单 owner 线程运行媒体引擎 prepare/start/submit/poll/stop/销毁。共享自 M033 接收线程；命令排队至多72，待编码原始 CPU 输入至多1。原始像素只在同 Worker 内传递，不进入控制 IPC；没有跨进程指针。
- 媒体私有 IPC 为 v5/bootstrap v5，新增本地 SendBinding 地址；子端独立检查该地址是本机网络接口。发送 Worker 可向 owner 报告有完整来源/epoch/拓扑的帧关联，接收方向及来源范围不能反转；停止后在途发送关联被认证消费但不调度。平台 IPC 保持v1。旧v4 bootstrap拒绝测试随阶段更新。
- 撤销/父 EOF 立即阻止新的采集与发送工作；capture.stopped 与媒体线程排空均满足后才发 Quiesced。父端仍独立验证副屏消失并关闭资源账本，不能用进程退出替代系统回收证据。

## 实际测试

Mac 实际编译发送/接收 Worker、平台 helper、NativeMacWebRtcLoopback 和受影响测试。使用交接文档 CMake/PYTHONPATH；命令：

```
/private/tmp/deskflow-build/bin/NativeMacWebRtcLoopback /private/tmp/deskflow-build/bin/busymouse-display-platform-helper /private/tmp/bmouse-gst-runtime-20261028 --sender-worker=/private/tmp/deskflow-build/bin/busymouse-display-media-sender
ctest --test-dir /private/tmp/deskflow-build/src/unittests -R '^Display.Infrastructure.(Worker|MediaWorkPermit)' --output-on-failure
```

真实新增副屏→SCK/编码独立子进程→本机标准 WebRTC→186 帧解码色块、186次 NativePaintConfirmed，可见窗口；撤销后发送 Quiesced/正常退出、副屏不存在、owner关闭、显示器数量1→1。原始输出见 M034-test-results.json。测试仅有本机接收端，控制身份来自 LabControlLoopback 夹具，不是真实认证控制或跨机验收。显示帧数不认证30fps。

26/26 Worker/许可相关测试通过（13.97s）。新增真实子进程来源租约领取一次、Prepare上下文一致、Stop前失效核查；新增发送帧关联方向/拓扑、停止后在途映射过滤。未重跑完整基础设施/服务回归。共享线程提取后的接收媒体复测结果另归档。

## 影响、限制与下一步

生产入口没有切换。媒体许可仍由本地诊断宿主授权上下文交付；WorkerSourceLease 证明私有可信启动/来源交付，不替代用户授权、输入屏障或 PlanCommit。许可最大24小时以及生产授权实际有效期传播仍待 owner 接线。

本阶段没有同步覆盖 Ubuntu；IPC v5 及共享接收头须在后续明确摘要验证后同步并编译。Windows未编译/认证。未验证跨机、发送Worker活跃采集时父EOF/崩溃副屏恢复、热插拔/锁屏/睡眠、长时/弱网、硬件编码与签名打包；M030间歇queue错误、M033冷启动风险仍保留。失败须保留账本，不能仅恢复测试输出掩盖资源状态。

代码、测试、结果及此备忘录同阶段提交，使用 git log --follow -- 本文件定位。回滚须将v5 IPC、WorkerConnection来源租约和两个Worker的共享媒体头成套回滚；旧产品媒体入口仍在。
