# 拓展屏底层重构：下一会话接手指导

交接日期：2026-10-29。实现基线：`0b986fb13`，分支 `dev-v1.27.0_todesk`。编写交接文档前工作区干净。本文件是后续指导，不是新增实现或验收证明；新会话先核对实际 HEAD 和工作区。

## 1. 用户目标与推进方式

用户已要求继续完成整个拓展屏底层重构，无须反复询问是否继续。最近用户明确质疑进度慢、自研过多、未形成可用产品闭环。本会话已承认：前期过多投入独立底座和局部验证，未足够早地贯通生产/跨机主线。

接下来围绕 **Mac 发送 → Ubuntu 接收的最小生产闭环** 推进：真实认证控制 → 双端计划提交/授权/输入屏障 → 隔离媒体 Worker → 真实 Mac 扩展桌面采集 → 标准媒体传输 → Ubuntu 可见呈现 → 撤销/断线后的停止和输出清理 → 产品入口接入。

这是后续优先顺序，不是缩减最终架构范围。暂不分散推进 RDP、Windows 或无直接闭环收益的基础模块；只新增当前闭环必需的契约和实现。避免再用“完成一个小模块”替代用户可使用的里程碑。每阶段仍遵守审计与测试要求。

## 2. 必读材料与环境约束

先读仓库根 AGENTS.md，然后：

1. `docs/dev/extended-display-architecture/architecture.zh-CN.md`：最终架构、权限和故障边界，严格遵循。
2. 同目录 `implementation-status.zh-CN.md`：全量任务状态。
3. `memos/README.md` 及 M023、M026–M031：实际媒体样机和最新许可接入边界。
4. `control-bodies.zh-CN.md` 和现有 Worker/帧契约，再修改控制或 IPC。

Mac 为主开发/编译/测试环境，仓库 `/Volumes/KINGSTON 250G/Code/deskflow`。
Ubuntu 用于集成和双机验证：`ssh -o BatchMode=yes ywh@10.20.1.71`，仓库 `/home/ywh/code/github/deskflow`。`docs/dev/ubuntu-remote-development.zh-CN.md` 有命令，但其旧“Ubuntu 主开发”文字被根 AGENTS.md 覆盖。

同步前检查远端工作区、HEAD 和目标文件摘要。两端历史可能不同；仅同步本阶段明确文件，不能 reset、覆盖远端已有改动。不要保存密码，不要用全目录同步覆盖远端。最近阶段是先对比远端每个目标文件 SHA256 与本地上一阶段基线，全部一致才写入。

每完成一个子模块，将代码、必要测试、备忘录一起提交。备忘录写清边界、设计对应、不变量、实际测试、已知限制、未接入项、提交定位。基础测试、真实子进程、媒体样机、真实桌面和跨机验收分别记录。Windows 没编译实测，不得标认证。

## 3. 当前真实进度

### 已有实现

- 纯规划/生命周期、控制契约、资源账本/执行器、Worker IPC/监督、帧预算/租约等独立模块（M001–M022）。
- Mac 真实虚拟输出 helper、provider、精确 CGDisplay 身份/拓扑绑定与 ScreenCaptureKit CPU 采集。
- GStreamer 原生 WebRTC 开发引擎（M023/M027），两端各自本机软件编解码、标准反馈和实际色块检查。
- 有界 SessionMailbox（M024）、控制正文注册表（M025）、控制媒体信令/帧关联序列化（M028）。
- Mac 样机资源所有者独立 QThread（M026），不是实际媒体 Worker 进程隔离。
- 完整上下文的进程内 MediaWorkPermit（M029），原生引擎入口及回调接入（M030），Mac 采集绑定接入（M031）。同一发送许可撤销后采集/引擎拒绝新工作，借出帧继续排空。

### 不可误报的缺口

- 产品媒体入口仍是原运行时；现有开发工具不是产品替换已完成。
- 没有实际装配好的隔离媒体 Worker；GStreamer/SCK/呈现仍在样机进程内。
- LabControlLoopback 是内存中实际编解码控制字节的夹具，身份由夹具断言，不是实际 TLS 或设备认证。
- MediaWorkPermit::issue 只验证结构和本地范围，不执行用户授权或输入屏障；生产 owner 必须在真实授权后签发。
- Mac 原始无媒体许可采集绑定仍支持独立采集诊断；显式 withMediaPermit 的副本才受媒体许可约束，不能声称所有产品采集入口已强制接入。
- 真实双端协商/PlanCommit、输入隔离、网络状态与 mailbox/生产 facade 接线未完成。
- Linux 原生虚拟输出/采集、RDP、Windows、共享句柄、系统恢复/热插拔、性能、打包均仍有缺口。
- Mac 桌面当前实际帧率没有达到/认证 30fps；设置 1080p30 不代表实际达标。

清单 M029/M030 行记录其当阶段边界，M031 已推进了 Mac 样机采集接入；不要把早期“未接入采集”重复当成最新未完成项。

## 4. 最新提交与验证证据

| 提交 | 阶段 |
| --- | --- |
| `7bf38db21` | M028 控制信令/帧关联序列化 |
| `bea03c6d1` | M029 本地媒体许可原语 |
| `6cf2137ea` | M030 原生引擎许可/撤销接入 |
| `0b986fb13` | M031 Mac 采集媒体许可绑定 |

- M029/M030：Mac、Ubuntu 各 137 项相关回归通过。
- M030：两端各 20 项原生政策检查；真实本机媒体撤销测试通过，包括有效输入/映射/信令被拒、持有解码帧阻止 stop 完成、释放后预算归零。
- M031：Mac 137 项相关回归通过；真实桌面采集 147 帧，解码色块/NativePaintConfirmed 各 99 帧。撤销后采集读取拒绝，采集租约排空，输出数 1→1、owner 关闭、CPU 字节回零。
- M031 是 Apple 专用四文件阶段，没有覆盖同步 Ubuntu 原生平台文件，也没做 Ubuntu 原生采集/Windows 验证。
- 各阶段原始日志和统计已归档在 `memos/M0xx-test-results.json`，优先读归档，不依赖 /private/tmp 文件永久存在。

已知未解释问题：M030 首次桌面复测在 72 个解码色块后失败，`NativePipelineError: queue/gst-stream-error-quark/1`；重试 91 帧通过，M031 99 帧通过，不代表根因已解决。另有历史 Mac ResourceConfigurationFailed 间歇失败、M020 两秒启动等待偶发失败，见进度文档，不能用后续成功抹掉历史。

## 5. 代码入口

- `src/lib/display/core/InfrastructureSession.*`、`SessionMailbox.*`：权威生命周期及事件邮箱。
- `src/lib/display/control/ControlBodyRegistry.*`：类型正文/信令/帧关联编码，尚非业务授权。
- `src/lib/display/supervision/WorkerProtocol.*`、`WorkerProcess.*`：现有私有 IPC、启动挑战、监督、父 EOF/租约。validWorkerBinding 只做结构校验。
- `src/lib/display/supervision/MediaWorkerContracts.*`：WorkerPrepare、标准信令/帧关联类型。已有源身份/拓扑证据，但帧关联和接收呈现目标尚需实际 Worker 契约装配；若扩展版本，旧 bootstrap 必须拒绝。
- `src/lib/display/supervision/MediaWorkPermit.*`：进程内对象，不可序列化成跨机“授权布尔值”。
- `src/lib/display/platform/macos/MacResourceProvider.*`、`MacCaptureSource.*`、`MacVirtualOutput.*`：真实输出/精确采集，MacCaptureBinding 只能从 live Active provider 证据产生。
- `src/lib/display/media/gstreamer/NativeWebRtcEngine.*`：开发引擎；public API 单 owner 线程，回调内部同步，帧租约可跨消费者线程释放。
- `tools/display-infrastructure-lab/NativeMacWebRtcLoopback.mm`：当前最完整本机样机。
- 同目录 `NativeWebRtcLoopback.cpp`、`ControlLoopback.h`、`MacResourceThread.h`：本机媒体夹具、控制字节回环、资源线程。
- `src/lib/display/DisplayService.*`、`PeerUsageService.*`：后续查找实际产品入口和现有信任/输入边界时的入口，不表示已经接入新架构。

## 6. 下一阶段具体行动与退出条件

先核对现有产品信任、控制网络、输入屏障和 viewer 代码，避免另写平行的设备认证或网络栈。先识别产品调用链，再给出一页以内的实现顺序，随后直接开发，不停在建议阶段。

优先完成实际媒体进程装配：

1. 复用 WorkerProcess/WorkerProtocol，定义最小完整的发送源和接收目标契约，绑定 session/attempt/owner/plan/epoch/方向。禁止对端选择任意 CGDisplay、窗口或 OS 句柄；接收呈现目标也要来自本地所有者证据。
2. 将采集+编码+网络留在发送 Worker，解码+呈现留在接收 Worker，避免把逐帧 CPU 像素塞进控制 IPC。原始像素指针不得跨进程。
3. 设计活跃资源证据的父→子交接和子端独立 native 身份/拓扑核查；不能为方便暴露“任意编号即可构造绑定”的公共工厂。需要生产权限事实的部分必须明确阻塞条件，不能用夹具授权冒充。
4. 许可在各本地可信 owner 边界产生；撤销/父 EOF/租约过期联动停止。外部监督器施加硬期限，stop 完成与系统资源消失分别确认。
5. 信令/帧关联经过实际控制连接，复用已完成编码和作用域校验。将 Worker 完成事件接入 mailbox；旧代次、晚到回调走清理，不影响新会话。
6. 形成 Mac→Ubuntu 可见闭环，再接产品入口；Ubuntu 呈现需要真实图形会话与目标确认，headless decode 不能代替可见呈现。

里程碑退出条件：正常连接可显示真实 Mac 新增扩展桌面；用户断开/许可撤销/控制断线/媒体 Worker 异常均有停止和清理证据；主控制线程不运行媒体库；两端归档方向、身份/来源/目标证据、实际呈现及输出回收。然后推进反方向和其他平台。

## 7. 已有开源复用及排错经验

媒体不是自写协议栈：当前复用 GStreamer 1.28.2、rswebrtc、libnice、x264、libav、h264parse、codec-utils；Mac 使用 ScreenCaptureKit。不要重写 SDP、ICE 或 H.264 解析器。

架构第 29 节记录 FreeRDP/GNOME Remote Desktop、Sunshine/Moonlight、RustDesk、Looking Glass 的参考边界。继续研究应围绕本阶段具体问题，优先上游公开接口、示例和源码，并记录版本/许可；“看过参考”不能冒充对应功能已集成。RDP 的 FreeRDP 正式适配尚未完成。

保持已有修复：

- 等待 rswebrtc signaller 公开 start 回调再启动会话；过早信号曾触发 panic。
- 禁止在已编码 RTP 分片处随意 leaky 丢包；此前导致真实灰帧，已删并加色块断言。
- H.264 caps 标签可能是兼容标签，实际 AVC 头由成熟 codec-utils 核查；不要退回只检查标签。
- 不把跨机单调时钟直接相减；实际 capture 时间、时钟身份、RTP 映射必须保留。
- 原生停止和回调可能晚到；不得强制释放 Reading 租约或伪报 stopped。进程退出也不能证明系统副屏消失。
- Qt/图像包装若可被复制保留，需要用 cleanup 回调持有像素租约，不能假设一个栈上的 QImage 已覆盖全部生产呈现生命周期。

## 8. 现有构建与运行路径

先检查这些目录是否仍存在；它们是当前会话环境，不是永久安装保证。复用现有 CMake cache，勿用通用配置覆盖其显式 SDK 设置。

Mac：

```sh
export PYTHONPATH='/Volumes/KINGSTON 250G/Code/bmouse-dependencies/build-tools'
'/Volumes/KINGSTON 250G/Code/bmouse-dependencies/build-tools/cmake/data/bin/cmake' --build /private/tmp/deskflow-build --target NativeWebRtcLoopback NativeMacWebRtcLoopback --parallel 4
'/Volumes/KINGSTON 250G/Code/bmouse-dependencies/build-tools/cmake/data/bin/ctest' --test-dir /private/tmp/deskflow-build/src/unittests -R '^Display\.(Infrastructure\.|Service\.|UsageService\.)' --output-on-failure
/private/tmp/deskflow-build/bin/NativeWebRtcLoopback /private/tmp/bmouse-gst-runtime-20261028 --policy
/private/tmp/deskflow-build/bin/NativeWebRtcLoopback /private/tmp/bmouse-gst-runtime-20261028
/private/tmp/deskflow-build/bin/NativeMacWebRtcLoopback /private/tmp/deskflow-build/bin/busymouse-display-platform-helper /private/tmp/bmouse-gst-runtime-20261028
```

改了静态库后先重建相关测试/工具再执行；不要同时重建和运行同一二进制。真实桌面工具会创建副屏、短暂显示窗口，提前告知用户并串行执行；失败时保留 ledger 审计目录，不擅自删除现场。

Mac Qt：`/tmp/deskflow-qt/6.8.3/macos`。
Mac SDK：`/Volumes/KINGSTON 250G/Code/bmouse-dependencies/gstreamer-1.28.2/framework/GStreamer.framework/Versions/1.0`。
clang-format：`/Volumes/KINGSTON 250G/Code/bmouse-dependencies/build-tools/clang_format/data/bin/clang-format`。

Ubuntu 已有 build：`/home/ywh/code/github/deskflow/build`；SDK 副本 `/home/ywh/code/bmouse-dependencies/gstreamer-native-m023/usr`；私有运行时 `/tmp/bmouse-gst-runtime-20261029`。已使用 Qt 6.10.2/GCC 15。远端 NativeWebRtcLoopback 可带同样 --policy 参数；ctest 使用 build/src/unittests 同样正则。

SDK/运行时通过 CMake 的 DESKFLOW_GSTREAMER_SDK 和 DESKFLOW_GSTREAMER_RUNTIME 显式启用；默认不编译/安装/发布新可用后端。工具 prepare_gstreamer_runtime.py 创建新的私有副本和摘要，不覆盖旧运行时、不改系统安装。保留私有库路径核查和插件白名单。不要因测试加载失败放开全局插件扫描。

当前环境沙箱可能要求构建、原生测试、SSH 和 git 写入走正常工具提权审批；如自动审查拒绝，说明拒绝的动作和原因，不绕过。不要把工具权限限制误报为代码失败。

## 9. 下一会话汇报要求

先简短说明核对到的当前状态，然后直接推进闭环实现。每轮报告：本次实际实现、实际测试、提交定位、仍未接入的生产边界。不要声称“全部完成”、不用模块数量换算百分比，不用本机色块或单测替代真实双机交付。
