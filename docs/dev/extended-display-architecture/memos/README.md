# 底层重构开发备忘录与审计索引

本索引按用户 2026-10-27 的要求维护：每完成一个子模块，提交独立备忘录。主开发环境按最新指令为当前 Mac，Ubuntu 用于集成及双机验证。

“完成”仅表示备忘录规定的子模块边界已实现并通过列出的检查，不表示已经完成全部重构、接入产品链路或通过所有平台实机验收。待接入/待验证工作必须独立列出，禁止用单元测试代替系统证据。

| 编号 | 子模块边界 | 状态与验证 | 备忘录 |
| --- | --- | --- | --- |
| M001 | 有向能力及管线规划纯核心 | Mac 编译，9 项行为测试通过 | [能力与规划](M001-directional-planner.zh-CN.md) |
| M002 | 无 I/O 生命周期核心 | Mac 编译，9 项状态/竞态测试通过 | [生命周期](M002-session-lifecycle.zh-CN.md) |
| M003 | 公共控制封装、连接绑定及计划编码 | Mac 编译，4 项边界/安全测试通过 | [控制契约](M003-control-contracts.zh-CN.md) |
| M004 | 本地耐久资源账本 | Mac 编译，8 项真实文件测试通过 | [耐久账本](M004-resource-ledger.zh-CN.md) |
| M005 | 类型化 Worker IPC 协议层 | Mac/Ubuntu，7 项协议/失联测试通过 | [Worker IPC](M005-worker-ipc.zh-CN.md) |
| M006 | 原生进程监督及 IPC 平台接入 | Mac/Ubuntu，5 项真实子进程测试通过 | [Worker 进程](M006-worker-process.zh-CN.md) |
| M007 | 进程内帧描述/预算/租约 | Mac/Ubuntu，8 项行为测试通过；GPU 为模拟对象 | [帧模型](M007-frame-model.zh-CN.md) |
| M008 | 通用资源执行器/恢复 SPI | Mac/Ubuntu，11 项测试通过；平台为受控模拟 | [资源执行](M008-resource-executor.zh-CN.md) |
| M009 | 控制限频/命令幂等缓存 | Mac/Ubuntu，6 项测试通过；待网络装配 | [控制准入](M009-control-admission.zh-CN.md) |
| M010 | 媒体准备/信令 IPC v2 | Mac/Ubuntu，新增 4 项；相关 17 项通过 | [媒体进程契约](M010-media-worker-contracts.zh-CN.md) |
| M011 | Mac 原生虚拟输出 helper/配置落盘屏障 | Mac 正常/强制退出真实输出清理；两端 113 项回归通过 | [原生输出 helper](M011-mac-platform-helper.zh-CN.md) |
| M012 | 原生进程出生身份与监督器绑定 | Mac/Ubuntu 各 8 项相关测试，Mac 两种真实输出清理通过 | [原生进程身份](M012-native-process-identity.zh-CN.md) |
| M013 | Mac 资源 provider/耐久恢复核查 | Mac 四项原生验证；两端 115 项回归通过，待产品装配 | [Mac 资源提供者](M013-mac-resource-provider.zh-CN.md) |
| M014 | Mac 显示拓扑探测进程隔离 | Mac 真机资源测试与模拟探测超时通过；完整超时认证待补 | [探测进程](M014-mac-probe-isolation.zh-CN.md) |
| M015 | Mac 探测消息解码前边界 | Mac 实际输出与异常长度/超时夹具通过；其余故障注入待补 | [探测消息边界](M015-probe-framing.zh-CN.md) |
| M016 | Worker 可执行文件描述符校验 | Mac/Ubuntu 各 6 项监督器回归；执行前替换竞态仍待解决 | [可执行文件校验](M016-worker-executable-validation.zh-CN.md) |
| M017 | 既有清理探测回归期限 | 修正测试等待，增加合法输出后挂起场景；Mac/Ubuntu 各 115/115 | [清理探测期限](M017-cleanup-probe-test-deadline.zh-CN.md) |
| M018 | Linux 描述符执行/统一原生启动 | Linux 确定性路径替换测试通过；两端各 116/116，Mac 路径竞态仍在 | [描述符执行](M018-linux-descriptor-execution.zh-CN.md) |
| M019 | Mac 活跃资源绑定与原生 CPU 采集 | 九项原生场景通过；CPU 租约排空通过，待隔离媒体 Worker/产品接入 | [Mac 采集源](M019-mac-capture-source.zh-CN.md) |
| M020 | 媒体 Prepare v3 源身份/拓扑证据 | 源绑定与旧 bootstrap 拒绝测试；待真实媒体 Worker 核查/装配 | [媒体源证据](M020-media-source-binding.zh-CN.md) |
| M021 | Worker 权限位/Mac 路径及 ACL 准入 | 两端权限回归、Mac 三项原生复测通过；待签名安装策略/原子执行 | [路径权限](M021-worker-path-permissions.zh-CN.md) |
| M022 | CPU 输入回调租约/明确像素视图 | 跨线程保活/回收及 Mac 实际采集通过，待编码器接入 | [CPU 回调租约](M022-cpu-callback-lease.zh-CN.md) |
| M023 | 原生 WebRTC 开发引擎 SPI/桌面样机 | 两端色块与反馈、Mac 真实采集/NativePaintConfirmed 通过；生产 Worker/预算认证待完成 | [原生引擎](M023-native-webrtc-engine.zh-CN.md) |
| M024 | 生命周期 owner 有界 mailbox | 两端各 128/128；关键事件保留、晚到清理与真实多线程通过，待生产 facade 接线 | [Owner mailbox](M024-session-mailbox.zh-CN.md) |
| M025 | 公共控制正文注册表 | 15 类双向强制准入、非法正文不消耗序号；两端各 133/133，状态/权限装配待完成 | [正文注册表](M025-control-body-registry.zh-CN.md) |
| M026 | Mac 桌面样机资源线程 | 真实桌面/GUI 响应及创建前取消通过；含历史失败，非生产 agent | [资源线程样机](M026-mac-lab-resource-thread.zh-CN.md) |
| M027 | 实际 AVC 头 Profile 证据 | 两端各 17 项政策与真实色块/反馈/排空；不以兼容 caps 标签代替码流头 | [AVC 证据](M027-h264-codec-evidence.zh-CN.md) |
| M028 | 媒体控制面序列化/帧关联 | 两端各 136/136，真实媒体及 Mac 桌面经过序列化绑定；无实际网络/Worker 接线 | [媒体控制编码](M028-media-control-serialization.zh-CN.md) |
| M029 | 本地媒体工作许可原语 | 作用域/到期/四线程不可逆撤销；尚未接入引擎及采集入口 | [媒体工作许可](M029-media-work-permit.zh-CN.md) |
| M030 | 原生引擎媒体许可接入 | 双端政策/真实媒体撤销及租约排空；采集许可和生产授权待接入 | [引擎许可](M030-native-engine-permit.zh-CN.md) |

以上四项位于同一基础设施阶段提交。使用 `git log --follow -- <备忘录路径>` 可定位包含其源码的提交；后续修复必须补充原备忘录或新增后续备忘录，不能修改旧测试结果冒充新验证。

前四项新增测试共 30 项；连同已重建的既有显示服务/用途服务，Mac 当前相关回归共 68 项通过。Windows 条件编译代码未在 Windows 编译或实测。前五项共 37 个新增测试已在 Ubuntu 通过。具体执行命令见各备忘录。

M005/M006 另有 12 项协议和真实进程测试在 Mac/Ubuntu 通过；Mac/Ubuntu 当前相应完整回归各 80 项通过。

M007 新增 8 项在 Mac/Ubuntu 通过，累计新增基础设施测试 50 项。

M008 新增 11 项通过，累计新增基础设施测试 61 项。最新完整回归（含既有显示/用途服务）：Mac 99/99（12.73s），Ubuntu 99/99（11.82s）。

M009 另有 6 项独立测试通过，累计新增测试 67 项。

下一步范围：消息正文注册表/控制 mailbox、跨进程内存/句柄、真实 Worker 媒体实现、平台资源提供者、正式媒体适配器、产品装配及双端协商集成。当前生产入口仍使用原媒体运行时；本阶段没有悄然将空后端发布为 available。

## 后续每份备忘录的必填内容

1. 模块目标及不在本次范围的事项。
2. 对应设计章节、代码路径、接口/持久化/协议变化。
3. 关键决策和安全/资源不变量。
4. 测试命令、实际环境、结果及故障覆盖。
5. 对既有功能的影响、迁移与回滚边界。
6. 未完成接入、已知限制、平台未验证项。
7. 审查重点与提交定位方式。


## 媒体实验（不计入已完成生产模块）

- [E001：原生 WebRTC 软件管线](../experiments/E001-native-webrtc.zh-CN.md)：Mac/Ubuntu 本机 1080p BGRA 解码及真实 GCC→encoder 反馈通过；授权、真实副屏、呈现和双机认证仍待完成。


M010 最新完整相关回归：Mac/Ubuntu 各 109/109，通过；累计新增基础设施测试 71 项。原生后端、平台资源和产品装配仍未完成。

M011 最新相关回归：Mac/Ubuntu 各 113/113；累计新增基础设施测试 75 项。Mac 原生输出生命周期有独立实测证据，持久平台提供者、采集/媒体和产品装配仍待完成。

M013 最新相关回归：Mac/Ubuntu 各 115/115。Mac provider 已有独立原生/账本重开验证，仍不是整进程崩溃恢复、媒体或产品验收。

M020/M021 当前相关自动回归为 Mac/Ubuntu 各 118 项通过。M019/M021 原生采集仅有独立模块证据，尚未接入隔离媒体 Worker、产品入口或跨机呈现。

完整当前开发清单见 [开发状态](../implementation-status.zh-CN.md)。M022 最新相关回归：Mac/Ubuntu 各 120/120；独立采集回调租约仍不代表媒体引擎已接入。

M023 最新自动回归：Mac/Ubuntu 各 121/121。两端本机真实媒体与 Mac 桌面诊断有独立证据；仍未生产装配。独立 capture 模式一轮输出配置失败，保留原生间歇问题。

M024 最新相关自动回归：Mac/Ubuntu 各 128/128；该阶段是纯生命周期/线程队列验证，不新增平台或媒体认证。

M025 最新相关自动回归：Mac/Ubuntu 各 133/133；公共正文结构校验不代表生产协商与授权已装配。

M028 最新完整相关自动回归：Mac/Ubuntu 各 136/136。控制字节回环与真实本机媒体已有组合证据，仍非设备认证或双机验收。
