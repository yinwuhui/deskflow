# M031：Mac 采集媒体许可绑定

## 边界与设计对应

对应 INV-10 的撤销后阻止后续采集及 Worker 内媒体管线要求。仅在开发样机的已有活跃资源采集绑定上增加 M029 本地许可约束；尚未迁入独立 Worker，不宣称生产授权责任链完成。

## 实现与不变量

- provider 签发采集绑定时保留本地 owner、planHash、userSession，与已有 session/epoch、CGDisplay、稳定身份、拓扑摘要及代次一并匹配发送许可。
- withMediaPermit 只返回具有匹配有效许可的绑定副本，不改变原始证据。副本共享原有单次采集 claim 与资源 live 标志，不能用副本多开采集；禁止给已附许可副本重新绑定许可。
- 绑定拒绝空许可、撤销许可、接收方向、其他 owner/plan/userSession 或其他来源。最低 CPU 预算沿用当前三帧 1920×1080 BGRA 路径。
- start 在消费 claim 前拒绝失效许可。异步准备、采样入口、发布前、readNewest/takeCpuLease 均受同一共享许可约束；poll 观察失效记录 CaptureMediaPermitInactive 并启动已有停止流程。
- 撤销不销毁消费者持有的像素；native stop 和帧池排空仍分别确认。撤销前已通过准入检查的在途操作不提供跨线程全局事务或硬 OS 停止期限。

## 实际测试

Mac 原生桌面样机：拒绝接收方向、其他 owner、已撤销许可；使用发送许可开始真实 SCStream 采集并进入 WebRTC/原生呈现。持有真实采集租约后撤销同一发送许可，确认采集读取入口立即拒绝、poll 报失效并停止、租约持有时停止未完成；释放后完成引擎/采集排空和输出回收。147 帧采集、99 帧解码色块/NativePaintConfirmed；输出数量 1→1，owner 关闭、CPU 字节回零。详见 M031-test-results.json。

Mac 自动相关回归结果另见结果文件。没有执行新的 Ubuntu 原生采集或 Windows 编译；本阶段 Apple 专用四文件没有同步覆盖 Ubuntu 工作区。NativeMacWebRtcLoopback 和 MacPlatformHelperSmoke 已重新编译。

## 限制与未接入项

原始无媒体许可的 MacCaptureBinding 仍用于独立资源/采集诊断，受已有资源 live/claim 约束；只有显式绑定许可的副本受本模块的媒体撤销约束。这不构成生产入口强制授权。生产 owner 必须只向媒体工作入口交付绑定副本，资源授权、网络认证、输入屏障和进程隔离仍待装配。未执行跨机、锁屏/睡眠/热插拔、长时性能或总内存认证。M030 首次桌面 queue 错误原因仍未确定，本轮通过不消除其历史记录。

## 提交定位

四个实现/样机文件、回归结果及备忘录在同一阶段提交；索引通过 git log --follow 定位该文件对应提交。
