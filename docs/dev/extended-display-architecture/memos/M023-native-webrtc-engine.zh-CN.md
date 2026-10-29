# M023：原生 WebRTC 开发引擎与真实 Mac 桌面样机

2026-10-29。对应架构第 6、9、14、17、19、21、24–26 节。本阶段完成显式 SDK 开发目标和进程内引擎 SPI；**不是正式媒体 Worker、生产适配器或发行认证**。

## 边界与实现

- `media/gstreamer/NativeWebRtcEngine` 使用 GStreamer 1.28.2、公开 rswebrtc signaller/encoder/payloader 信号与 libnice。发送 appsrc 包装 M022 CPU 租约；完成回调最后释放后才归还源预算。软件 x264、1080p30 固定配置、实际 H.264 Constrained Baseline 检查、真实 GCC 与编码码率通知；未准入硬件编码。
- 默认不编译、不安装、不发布 available。只有显式 DESKFLOW_GSTREAMER_SDK/RUNTIME 才启用。开发复制工具创建新目录，目录 0700、库 0555，保留摘要清单；CMake 固定 18 个插件摘要，启动禁用默认扫描和默认信令服务器。核查实际加载的 GStreamer/libnice 路径和 GStreamer 编译版本；SDK rpath 被移到私有副本之后。Linux 诊断程序采用可传递 RPATH。此流程不是签名安装包策略或完整依赖闭包认证。
- 只绑定本机明确地址；libnice 解析并拒绝非许可地址、非 host/UDP、非 component 1、零端口候选；再次核查真实 selected pair。禁 STUN/TURN/UPnP/TCP、音频和输入数据通道。SDP 使用 GstSDP，要求单视频、预期方向、RTCP mux、DTLS SHA-256 fingerprint 结构；完整协商结果还需实际码流检查。外层调用方负责提供经认证控制连接绑定的信令，不能把 peer 自报身份当设备认证。
- 初始信令等待 SDK signaller start 后才发起 session；提前到达的信令限制 72 项/256 KiB。对外信令同样有界；帧关联、PTS 索引和源历史各有 64 项上限。原始 appsrc 队列 2 张；接收等待元数据最多 2 张，CPU 池最多 3 张/32 MiB。停止先关闭接受、使 pipeline NULL、移除回调，再 retire 并等待外部读租约；无法确认原生停止则隔离该进程运行时，不假报 quiesced。
- RTP 的 SSRC/timestamp 从公开 GstRTPBuffer 读取，不修改标准媒体头。用公开 GstSegment 的 stream-time 对应输入 PTS，避免猜测 x264 的时间偏移。NativeFrameCorrelation 是本地有类型 SPI，尚无认证网络序列化。收到关联之前不能发布解码帧；冲突拒绝，重复允许。RTP buffer list 中逐个检查，不能只检查首包。
- FrameDescriptor 新增采集时钟域与源采集序号。未知采集时间须显式零值/空时钟；不将解码到达时间伪装为采集时间。池内禁止时钟域切换和时间倒退；重复画面允许相同真实采集时间，同时使用新的媒体帧序号。
- Mac 活跃 binding 可填写 v3 WorkerPrepare 的源证据，此方法不消费采集许可，也不授予远端任意采集权。真实样机仍在同进程使用活跃 binding；没有绕过 provider 重新创建输出。

## 实际测试与失败修正

完整原始退出码、stdout/stderr、已有源码与二进制摘要见 [M023-native-results.json](M023-native-results.json)。早期记录没有的摘要未事后补造。

1. Mac/Ubuntu 自动回归各 **121/121**，分别 20.87/18.81 秒；新增 FrameTimestampEvidence 检查未知时间、缺少时钟、时钟切换。不是媒体认证。
2. 两端政策夹具各 9 项：生产环回拒绝、源代次不符、越界地址/TCP/relay 候选拒绝、关联代次、幂等/冲突，以及仅关联不产生画面。调用真实 SDP/ICE 引擎，无设备授权认证。
3. 最终 8 秒合成媒体：Mac 输入 233、读取 221、色块 221、GCC 22/encoder 23；Ubuntu 输入 239、读取 224、色块 224、GCC 28/encoder 29。两端读取租约阻止提前停止，释放后原生停止和源预算归零。均为各自本机 UDP，不是双机。
4. 最终真实 Mac：创建附加输出→精确 SCK 采集→标准 WebRTC→解码色块→原生接收窗口。采集 160、解码/色块 94、native paint 93；窗口在物理主屏可见，源在单独虚拟屏。停止后输出数量 1→1、owned 输出缺席、账本 owner 闭合、采集 CPU 字节为 0。证据仅 NativePaintConfirmed，不冒充 compositor fence。
5. 独立采集四模式：capture-cancel、capture-revoke、capture-callback 通过；capture 一轮 ResourceConfigurationFailed，未进入采集。保留失败，不用桌面样机成功替代这项。虚拟输出创建/配置的间歇问题仍未解决。

开发中真实失败及修正：

- SDK 源路径 rpath 优先导致加载错误；改为私有副本优先并用 dladdr 核查。Ubuntu 缺少传递依赖时启动退出 127；修正诊断目标 RPATH 后通过。
- 过早 session 信号触发 rswebrtc panic；等待公开 start 回调，并缓冲提前信令。
- 缺 videorate/errorignore 插件；补入固定 allowlist，而非开放目录扫描。
- x264 输出 PTS 与输入 PTS 不同；使用 GstSegment 的公开映射。
- 第一轮桌面创建失败、早期原生证据不完整保留。增加源/解码像素计数后确认：源色块存在，接收端灰帧。原因是 leaky queue 的 max-size-buffers=2 实际按 RTP 分片丢包，破坏 H.264 依赖。**移除此队列**，保留标准 WebRTC jitterbuffer/恢复语义；最终两端色块断言通过。不能宣称这个删除使 SDK 内部所有队列都获得硬预算。
- H.264 协商 caps 可能先于首个 SPS，缺 profile 时等待真实证据；出现明确不符 profile 则失败。未获得 profile 不发布解码帧。

## 复现

先运行 `tools/display-infrastructure-lab/prepare_gstreamer_runtime.py --sdk-prefix <SDK> --output <new-private-root>`；显式配置两个 CMake cache 路径，构建 NativeWebRtcLoopback、Mac 的 NativeMacWebRtcLoopback。

- `NativeWebRtcLoopback <private-root> --policy`
- `NativeWebRtcLoopback <private-root>`
- `NativeMacWebRtcLoopback <absolute-platform-helper> <private-root>`（手动：短暂创建副屏并显示自己的测试窗口，需要现有录屏许可）

Mac SDK 位于依赖目录的 GStreamer.framework/Versions/1.0；Ubuntu 新建 gstreamer-native-m023 私有 SDK 副本，补提取 libnice-dev 0.1.23-2（deb SHA256 `83de8c6f6dea5aaf358fe25091dad720d4446a696bdcdba555175df89c92fe14`），未安装系统包。远端公共文件摘要全部匹配上一阶段再同步，未覆盖其他修改或重置历史。

## 未接入、限制与提交

- 未接入隔离媒体 Worker、设备授权/计划承诺、控制正文注册表、网络帧关联、产品入口及专用资源线程。GUI 样机含第三方媒体库，只允许独立开发使用；生产仍旧入口。
- 没有证明 SDK 内部 jitterbuffer/编码器/解码器/GPU/Qt backing store 的总预算，未完成恶意 AU 的进程内硬分配限制、进程 RSS 限制、所有 OOM 异常路径和原生阻塞的外部硬截止；同步 set_state 可能阻塞，必须由隔离 Worker 及外部监督兜底。
- 实际桌面本轮约 12 张解码帧/秒，**不是 1080p30 性能认证**。没有弱网、长时、双机、锁屏/睡眠/热插拔或第三方标准端点互通验收。
- 依赖签名/SBOM/更新原子屏障、Linux 真实输出/采集、RDP、Windows 均未完成；Windows 本阶段未编译或实测。
- 模块源码、测试、结果和本备忘录同阶段提交。通过 `git log --follow -- 本文件` 定位；不改生产开关即可回退开发目标。
