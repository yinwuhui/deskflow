# M028：媒体信令与帧关联的有界控制面序列化

2026-10-29。对应架构第 13、14.4、17 节及附录 A.4/A.6。把开发引擎的进程内映射提升为公共、受限、可经连接封装的媒体契约；**尚未接入实际网络传输、设备认证或 Native Worker IPC**。

## 实现与契约

- 新增公共运行时类型 WebRtcFrameCorrelation；原 NativeFrameCorrelation 成为同类型别名，SDK 适配器与控制编码器使用同一结构及 validFrameCorrelation 校验。它是 WebRTC 专用标准帧关联，不强迫 RDP 使用 RTP 字段。
- ControlBodyRegistry 新增 FrameCorrelation 正文及媒体信令的类型化编码/解码。公共正文共 16 类；更新外层 schema、架构 A.4 和 [正文契约](../control-bodies.zh-CN.md)。没有修改标准 RTP/媒体包头或传输像素。
- 映射必须声明 backend=webrtc 和 mappingProfile=control.rtp-map.v1，携带抽象源资源、拓扑代次、epoch、采集时钟域/真实时间/源帧序号、媒体帧序号、SSRC/RTP timestamp 和重复标志。只对双方锁定支持该映射契约的 profile/实现版本使用；旧 peer 不认识必选类型时拒绝，不自动假定兼容。
- 64 位计数/时间使用规范十进制字符串，避免 JSON double 损失精度；epoch 为正 uint32，SSRC/timestamp 为 uint32 且允许零与回绕。时钟域上限 56 字节，接收侧仍加 remote.，不把对端时间当本机延迟。未知时间不能伪造为有效关联。
- 精确字段集合拒绝额外本地指针、CGDisplay/PipeWire 号、文件路径、句柄、像素或程序参数。FrameCorrelation 依赖外层 session/attempt/planHash，连接封装继续检查 nonce 绑定、认证来源参数和传输序号；实际 SDK 再核查源/拓扑/epoch 并拒绝冲突。正文结构通过不等于获得采集或资源权力。
- 有界信令解码保留 signalSequence 和 WorkerMediaSignal 四种互斥载荷，标准 SDP/ICE 仍交给成熟引擎解析。

新增开发工具内部 ControlLoopback：两端先建立 ControlBinding，将 SDP/ICE 与帧关联编码成真实控制字节，再接收校验、检查精确 session/attempt/plan/epoch，最后交给 SDK。它替换了两份样机原先直接复制关联对象的路径。双方 fixture 身份由同进程显式提供，**不是 TLS/设备身份认证证明**。开发 profile 改为明确的 diagnostic.webrtc.h264.control-map.v1，没有发布 available。

## 测试

新增三项公共控制行为测试：

1. FrameCorrelationWire：U64_MAX 采集时间与序号无损、RTP uint32 最大值/零值/回绕、有连接封装的往返、重放拒绝和缺少计划绑定拒绝。
2. FrameCorrelationMalformed：逐项缺失/null、epoch/SSRC/时间戳范围、非规范计数、未知映射版本、RDP 混入、超长时钟名、原生定位符与指针字段拒绝。
3. MediaSignalTypedWire：offer/answer/ICE/end 四种标准载荷及最大信令序号往返，零序号拒绝。

原正文全部类型/字段测试扩至 16 类；原生政策中的错误拓扑映射改为先序列化/解码，再由 SDK 按当前范围拒绝。Mac 控制八项 8/8（3.38 秒），完整自动回归 **Mac 136/136（24.85 秒）、Ubuntu 136/136（19.93 秒）**。

Mac 本机合成媒体：序列化关联 227，解码/色块 221，GCC 38/encoder 39，读取租约阻止提前停止，释放后排空与源预算归零。真实 Mac 桌面：采集 116、序列化关联 87、解码/色块/native paint 各 83，GUI 创建计时器 219 次，输出数量 1→1、owner 关闭、采集 CPU 字节 0。

Ubuntu 本机合成媒体：政策 17 项通过，序列化关联 222、解码/色块 220、GCC 38/encoder 39；租约阻止提前停止、随后排空和源预算归零。两端均为同版源码。所有原始日志及摘要见 [M028-test-results.json](M028-test-results.json)。上述三类结果分别是公共协议/线程/进程自动回归、本机媒体、真实 Mac 桌面，不互相替代，更不是跨机认证。

## 未完成、影响与提交

- Wire codec 可用于受认证控制传输，但尚无产品连接/计划事务接线。映射发送方权限、当前候选/能力、operationId 缓存及 grant/barrier 的真实性仍由宿主核查，不能因结构合法而创建资源。
- Worker IPC 仍为 v3，尚未传递 FrameCorrelation；没有跨进程原始帧、OS 句柄或共享内存实现。C++ 别名指向公共类型，消费者须重编译链接。
- profile/实现版本组合支持核查、真实双机连接、媒体 Worker、生产呈现/预算/长时/弱网/打包仍未完成；Windows 未编译实测。生产旧入口不变。
- Ubuntu 同步十个已有文件前逐项匹配 HEAD 摘要，另一个新头文件确认不存在后创建；未覆盖其他工作或重置历史。Mac 手动资源线程源仍不在 Ubuntu 编译。

源码、公共测试、真实媒体结果及本备忘录同阶段提交，通过 git log --follow -- 本文件定位。
