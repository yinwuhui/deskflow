# 公共控制正文 v1 注册表

2026-10-29，M025。对应架构附录 A.4。`display-infra/1` 外层和每个正文都必须通过校验；字段名大小写敏感，下面列出的字段全部必填，其他字段拒绝。`contracts.schema.json` 仍只约束公共外层、计划和资源；本文件及 ControlBodyRegistry 定义正文额外约束。后端 SDP/ICE 标准解析、计划查找、状态、授权和屏障真实性必须分别验证。

ID128 为非零 32 位小写 hex；摘要/PeerId 为非零 64 位小写 hex。U64 使用规范十进制字符串，拒绝前导零和溢出；允许零的字段只接受字符串 `"0"`。受限整数和布尔必须使用对应 JSON 类型，不能用字符串冒充。

计划命令和 Stop/Stopped 中的 `operationId` 用于 M009 幂等缓存，独立于外层 `messageId` 和传输序号。重发可以换 messageId/sequence，但不得变更同一 operationId 的参数。注册表只校验操作标识格式，宿主仍须接入缓存和状态准入。

| 消息 | 全部必填字段及关键约束 |
| --- | --- |
| CapabilitiesSnapshot | peerIdentity、revision、environment、directNetworkAllowed、sessionAvailable、cleanupVerified、pipelines；peerIdentity 必须是认证发送者；pipeline 至多 16 项 |
| PlanOffer | operationId、senderPeerId、receiverPeerId、senderCapabilityRevision、receiverCapabilityRevision、minimum、candidateRefs；双方不同且发送者是其中一方；候选摘要 1–8 个且不重复 |
| PlanAnswer | operationId、accepted、reason、candidateRef、senderCapabilityRevision、receiverCapabilityRevision；接受时精确摘要且 reason 空，拒绝时 candidateRef 空且 reason 为错误标识 |
| PlanPropose | operationId、planHash、planRef；两者必须等于外层 planHash。planRef 是同会话内已取得、按摘要索引的不可变计划引用，不是 URL/路径或下载指令 |
| PlanAccept | operationId、planHash、accepted、reason、senderCapabilityRevision、receiverCapabilityRevision；hash 同外层，接受/拒绝与 reason 一致 |
| PlanCommit | operationId、planHash、senderAuthorizationDigest、receiverAuthorizationDigest、senderBarrierRef、receiverBarrierRef；摘要同外层；授权摘要与屏障引用仅是声明，必须查本地有效登记 |
| PlanCommitted | operationId、planHash、committed、localCommitRef；hash 同外层、committed=true |
| MediaSignal | backend、signalSequence、kind、description、candidate、mid、mediaLine；当前仅 webrtc，单视频 mediaLine=0；offer/answer/ice/end-of-candidates 字段互斥，复用 M010 结构预算 |
| FrameCorrelation | backend=webrtc、mappingProfile=control.rtp-map.v1、sourceResource、captureClockId、topologyGeneration、frame、captureFrame、captureNanoseconds、epoch、ssrc、rtpTimestamp、repeated；U64 字符串保留精度；RTP uint32 允许 0 与回绕；必须绑定外层 session/attempt/plan |
| EndpointReady | backend、resourceRef、prepared；backend 为 rdp/webrtc、resourceRef 为 ID128、prepared=true。只承载端点登记引用，不承载私钥、路径、任意 URL 或 peer 可选的本地程序 |
| MediaReady | backend、planHash、video、securityProfileId；hash 同外层，视频与安全配置必须和后端相容；还需与实际锁定计划和协商结果比较 |
| FirstFrameEvidence | epoch、frame、sourceResource、targetResource、targetValid、evidence；epoch 为正 uint32；等级为 decoded/submitted/native-paint-confirmed/compositor-presented；声明等级不等于本机原生证据 |
| HealthSummary | sequence、health、valid、captured、decoded、presented、retainedCpuBytes；health 为 healthy/idle/degraded/failed；三个帧计数允许零，CPU 字节整数 0–64 MiB；valid=false 时全部指标须为零 |
| Stop | operationId、reason、lastEpoch、lastSequence；reason 为 Cancelled/Revoked/ConnectionLost/Deadline/Shutdown/BackendFailed；lastEpoch 整数允许零、lastSequence U64 允许零 |
| Stopped | operationId、stopped、localCleanup、lastEpoch；stopped=true；localCleanup 为 verified/pending/unknown；不能据此关闭本机资源账本 |
| Error | code、stage、relatedMessageId、resourceImpact；前两者为至多 64 字节的受限标识，不接受堆栈文本；影响为 none/possibly-created/owned/unknown |

## 嵌套声明

`minimum` 包含 width=1920、height=1080、fpsNumerator=30、fpsDenominator=1、colorProfileId、cursorMode、requireHardware，以及 allowAudio/allowRemoteInput/allowRedirection=false。反馈、呈现、清理等本地最低策略由 core 的 Requirements/SelectionPolicy 再核查，peer 不能放宽。

`video` 包含上述四个固定尺寸/帧率字段、graphicsMode、colorProfileId、cursorMode。WebRTC graphicsMode 只能 h264-constrained-baseline，RDP 只能 rdp-avc420/rdp-avc444；色彩只接受 sdr-bt709-limited-8bit/sdr-bt709-full-8bit，光标只接受 embedded/metadata。

每个 pipeline 必须包含 id、implementation、implementationVersion、environment、direction、backend、readiness、reason、adapterVersion、security、formats、memory、hardware、rateFeedback、refreshFeedback、presentationEvidence、verifiedCleanup、resourceCoupledToWorker。环境摘要必须与外层快照一致；direction 为 send/receive；同 direction/id 不重复；adapterVersion=1；security 只有一个且与 backend 匹配；formats 1–8 项且不重复；memory 为 cpu/d3d11/iosurface/dmabuf/backend-native。条件/失败必须带受限 reason，可用时 reason 必须为空。能力只表示对端声明，不能替代本地认证表。

WebRTC 安全标识为 webrtc.authenticated-dtls-srtp.v1；RDP 为 rdp.authenticated-tls13.v1。RDP 标准信令载荷尚未准入，因此 MediaSignal 不接受 backend=rdp；这不把未实现的 RDP backend 宣告可用。

## 边界

正文合规不等于当前状态允许执行，更不等于得到采集/创建输出/接收输入授权。网络装配必须查验当前基础连接、session/attempt/epoch、能力修订、精确候选、计划引用、授权和屏障，再经 owner mailbox 调用核心。对端不能把新声明的 capability、localCleanup 或 FirstFrameEvidence 变成本机平台证据。首帧和统计没有跨机时间差字段，不能用对端时刻伪造本机延迟。

M028 增加 FrameCorrelation 后共 16 类正文。仅双方锁定包含 control.rtp-map.v1 的 profile/实现版本组合才可交换；旧 peer 不认识该必选消息时必须拒绝，不能默认兼容或降级。当前只由显式开发样机使用，未发布生产 available。Native Worker IPC 暂未加入此消息。
