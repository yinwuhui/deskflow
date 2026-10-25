# 虚拟扩展屏阶段 A 实测与开发记录

日期：2026-10-25。

## 当前结论

当前 Ubuntu GNOME 和本机 macOS 均已跑通真正的虚拟屏创建、目标屏采集和清理。
这验证了两个主机后端的基础可行性，另已验证离线 H.264 编码及双向样本交叉解码。尚未实现跨设备实时画面传输、产品媒体管线、实际输入注入和产品界面，不能据此宣称完整功能已完成。

用户已明确当前没有 Windows 设备，实机测试先限定 Mac 与 Linux；允许当前 Mac 编译和运行独立虚拟屏探测程序。
常规开发、主工程编译和测试仍在 Ubuntu 远端进行。Windows 的驱动与显示后端保留为待开发、待实测项。

## 参考源码完整性

9 个主仓库均成功浅克隆、工作区干净，`git fsck --connectivity-only --no-dangling` 均通过。
Windows-driver-samples 按计划稀疏检出 `video/IndirectDisplay`，不是丢失该示例源码。

| 项目 | 固定提交 | 顶层许可证标识/文本 | 检查时未初始化子模块数 |
| --- | --- | --- | --- |
| OpenDisplay | 018a6a34ebcaa9c4efee825f62c5839457f2039c | GPL-3.0 | 0 |
| DeskPad | c3349f0e237e000cb4826fb3ea1cdd1c44949461 | MIT | 0 |
| Virtual Display Driver | d7244969b2aa8bb38e76d79505eda217996cefea | MIT | 1 |
| Sunshine | d0b4a88ef727f8dabf80cc970a14739b55c885de | GPL-3.0 | 16 |
| Moonlight Qt | 781b26fd1ca245cc241ded27da252452533fa3b0 | GPL-3.0 | 3 |
| Apollo | adc5c5a0bd80831ce495434bb16aee2cd4175fb8 | GPL-3.0 | 17 |
| Windows driver samples | bb68ed5e770bce04dc5395ec9258dafab034184d | MS-PL | 1 |
| Weylus | 38a01a8f8e429500c7e9f67fc1c88ca37a4d1e93 | AGPL-3.0-or-later，贡献文件另有说明 | 0 |
| Deskreen | b5dc3d4d1a74ec38091d36578fd84174db3792bc | AGPL-3.0 | 0 |

顶层许可证不是每个文件或依赖的授权结论；复用时继续检查具体文件。当前仅引入 DeskPad 的 MIT 私有接口声明并保留完整许可与来源；没有把 GPL/AGPL 实现复制进公共库。
主仓库完整不等于已经具备构建依赖。后续复查确认递归子模块补齐命令仍在运行，Sunshine、Apollo 的部分依赖尚未完成指定版本检出；保留该中间状态，不重置正在拉取的目录。子模块快照和本地补齐脚本位于独立参考目录清单中。

## Ubuntu 实测

| 环境 | 观察值 |
| --- | --- |
| 系统 | Ubuntu 26.04.1，x86_64，Linux 7.0.0-38-generic |
| 桌面 | GNOME Shell 50.1，实际图形会话 Wayland |
| 用户总线 | ScreenCast Portal AvailableSourceTypes = 7；Mutter ScreenCast Version = 4 |
| 采集 | GStreamer PipeWire source 1.6.2，BGRx，1920×1080 和 1280×720 |
| 逻辑拓扑 | 原 eDP-1 保留，新增独立 Meta-0 逻辑显示，退出后仅保留 eDP-1 |

运行 4 次 1920×1080 和 1 次 1280×720 的创建/采集/停止。
每次均收到实际图像缓冲、确认独立逻辑输出、确认采集尺寸，清理后拓扑与开始前一致。
这 5 次采集首帧等待为 392～658 ms，包含本实验的诊断开销，不是产品启动 SLA。

额外执行两种异常退出：

- SIGTERM：探测程序释放采集管线并 Stop 会话，报告 cleanupVerified。
- SIGKILL：进程没有机会执行 finally；D-Bus 连接关闭后 Mutter 移除虚拟显示，外部检查确认拓扑恢复。

共 7 项通过。不是 100 次可靠性认证，也不是所有 GNOME/KDE/Hyprland 支持声明。

### 已解决的协商问题

虚拟节点实际公布 `framerate=0/1`、`maxFramerate=30/1`；最初请求固定 `30/1` 导致 `no more input formats`。
使用可变帧率、显式 BGRx 和相同尺寸后成功采集。先读取真实格式再调整，避免将失败误判为“不支持虚拟屏”。
GNOME 50 的显式 modes 参数与采集尺寸保持一致。捕获仅针对本次 RecordVirtual 的节点，没有回退采集主屏。

## macOS 实测

| 环境 | 观察值 |
| --- | --- |
| 系统 | macOS 27.0.1，Build 26A434 |
| 编译 | 当前 Xcode clang++，C++20、Objective-C ARC，Wall/Wextra/Werror |
| 显示 | CGVirtualDisplay，1920×1080、30 Hz、非 HiDPI |
| 采集 | ScreenCaptureKit，BGRA，只选择本次创建的 display ID |

先执行创建/销毁，再执行真实采集验证；正式重复矩阵包括 3 次创建/采集/停止、SIGTERM、SIGKILL，共 5 项通过。
每次创建前为一个活动屏，运行时增加一个非镜像活动屏，退出后恢复原活动屏集合。
真实采集均收到至少一个完整状态的像素缓冲，尺寸为 1920×1080，停止后完成资源清理。
静止桌面没有持续运动负载，不能把缓冲数当作 30 fps 测试。

## 离线编码与交叉解码

| 方向 | 编码器与样本 | 另一平台的独立解码结果 |
| --- | --- | --- |
| macOS → Linux | VideoToolbox H.264 Baseline，确认 hardwareEncoder=true，1920×1080，5 帧 | Ubuntu FFmpeg 严格错误模式解码成功，ffprobe 读取 5 帧，无解码错误 |
| Linux → macOS | FFmpeg/libx264 软件编码，Constrained Baseline，1920×1080，14 帧 | Mac FFmpeg 严格错误模式解码成功，ffprobe 读取 14 帧，无解码错误 |

两类样本均来自本次虚拟屏，未采集主屏。经已认证 SSH 传送临时文件用于兼容性验证；没有启用实时视频网络服务。
Linux 的待编码原始帧队列最多 2 帧，写管线在独立线程，队列满时丢弃旧的未编码帧。
Mac 使用 VideoToolbox 实时编码选项、禁用 B 帧，并检查 Annex B 转换的长度边界。

另验证 Linux 编码器输出目录无效时返回失败，编码写线程退出，虚拟屏仍完整清理。Mac 探测器拒绝覆盖已有输出文件，失败后虚拟屏仍清理，原样本哈希保持不变。
静止桌面和实验固定时间戳不构成 30/60 fps、实时延迟、色彩或办公文字质量认证。
Mac 硬编码已实测；Linux 当前仅测软件基线，不能声称硬编码已通过。

## 公共库实现

新增默认关闭的 `BUILD_VIRTUAL_DISPLAY`，仅构建实验公共库和测试，尚无视频入口或监听端口。

- DFVD/1：固定 40 字节头，大端编码，不序列化 C++ 内存布局。
- FrameReader：处理任意半包与粘包，在分配负载前验证类型和长度，限制单次读入量，错误后拒绝继续解析。
- 隔离：按通道限制类型，可绑定 SessionId 与 DisplayEpoch，拒绝重复/递减序号。
- SessionState：显式启动、授权、准备、首帧、恢复和清理阶段；停止、重配置和恢复更新操作代次，旧回调不能推进新状态。
- InputLease：唯一身份与租约，拒绝旧 epoch/过期/重复租约；抢占、超时和撤销返回需要释放的按键与鼠标按钮。

这些公共组件还没有网络认证、JSON schema、实际输入适配器和产品媒体后端；独立探测工具中的离线编码不等同于这些模块已集成。
尤其不能把解析器的 ChannelBind 消息类型理解为已经实现绑定令牌验证。

Ubuntu 独立构建使用 GCC 15.2.0、`-Werror`、AddressSanitizer 和 UndefinedBehaviorSanitizer。
13 项测试全部通过，覆盖固定字节向量、逐字节/任意分段、粘包、坏版本/类型、截断、负载上限、4 MiB 边界、通道/会话/epoch、重放、取消、恢复与输入租约到期。

## 继续开发顺序

1. 用户补齐参考项目固定子模块；不自动更新其主分支版本。
2. 将已验证的采集和离线编码提炼为产品媒体后端，验证 Linux 硬编码和两端呈现。
3. 完成身份、三条 TLS 连接、一次性绑定令牌、JSON schema 和操作授权后，再启用任何网络画面或输入。
4. 建立 Mac ↔ Linux 的真实视频链路，测清晰度、持续运动帧率、延迟与拥塞恢复。
5. 接入 GUI 与现有输入模式互斥；完成锁屏、权限撤销、长时间运行和回归。
6. Windows 有可用设备和驱动工具链后补齐其后端与六方向矩阵。

主工程在 Ubuntu 启用 BUILD_VIRTUAL_DISPLAY 和 BUILD_BLUETOOTH 后完整构建成功，60 项 CTest 全部通过（含 13 项新增显示基础测试）。远端测试基线为 2532816e1 加本轮改动，本地另外两项已提交的文档/UI 改动保留原状，不纳入这次远端回归结论。未执行的网络与 Windows 验证不计为通过。
