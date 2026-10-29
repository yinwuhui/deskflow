# M025：公共控制正文注册表与双向强制准入

2026-10-29。对应架构第 13、17、21、26 节及附录 A.4。完成公共正文结构校验并接到 M003 的发送、接收入口；没有完成网络会话装配或状态/授权准入。

## 实现与协议边界

新增 ControlBodyRegistry，规范字段与预算见 [公共控制正文 v1](../control-bodies.zh-CN.md)。覆盖 CapabilitiesSnapshot、六类计划协商消息、MediaSignal、EndpointReady、MediaReady、FirstFrameEvidence、HealthSummary、Stop、Stopped、Error，共 15 类。

所有类型精确校验必填字段，拒绝额外字段、null、错误 JSON 类型、未知必选枚举和不规范计数。能力快照必须绑定外层传输认证的 sender，限定环境摘要、管线唯一性/数量、相容安全配置与视频格式、可用/条件/失败原因。计划命令及 Stop/Stopped 要求独立 operationId；后续由 M009 缓存绑定参数摘要。

计划正文摘要与外层 planHash 必须一致；PlanPropose 本阶段只接受同会话内按摘要取得的不可变计划引用，不能当作路径/URL 下载指令。PlanCommit 的授权摘要和屏障引用须完整，但结构通过不证明登记真实。最低要求拒绝音频、远端输入和重定向。

MediaSignal 复用 M010 WorkerMediaSignal 的结构预算：SDP 32 KiB、候选 1 KiB、单视频和互斥字段；成熟媒体引擎继续负责标准语义、fingerprint/地址策略和实际协商。RDP 标准载荷尚未实现，因此拒绝用 backend=rdp 的 MediaSignal 冒充已准入信令。EndpointReady 仅允许资源登记引用，不提供任意地址、私钥、程序路径等入口。

首帧区分等级和目标有效性，统计区分有效/未知，不包含可被误当本机时钟的远端时间差。Stopped 的 localCleanup 是对端声明，不能代替本机账本关闭证据。

M003 ControlBinding::encode 在序号递增前校验正文；receive 在更新 m_rx 前校验。非法正文不消耗有效序号。未经绑定的连接及外层未知字段、旧序号仍由既有检查先行拒绝。`contracts.schema.json` 仍只提供外层/计划/资源 schema，不把它描述成完整正文验证器；架构 A.4 增加实现契约链接。

## 测试

新增 DisplayControlBodyRegistryTests 五项：

- BodyRegistryAllTypes：15 类经过真实 ControlBinding 的认证绑定、编码、接收和正文往返；接受/拒绝分支和未知消息类型。
- BodyRegistryFields：每类逐个移除或置 null 所有必填字段、注入额外 program；内外计划替换、计数类型/前导零/uint64 溢出、无效统计伪造。
- BodyRegistryCapabilities：发送身份替换、跨环境、非布尔字段、重复格式/管线、错误安全配置/后端、缺条件原因、超限管线/候选及输入权限扩张。
- BodyRegistrySignals：offer/answer/ICE/end 字段互斥、SDP 上限、额外轨道、控制字符和未实现 RDP 载荷；明确允许结构正确但须后端进一步解析的 SDP，不能把本测试当标准解析认证。
- BodyRegistrySequence：接收非法正文后同序号的合法消息仍通过；发送失败不递增序号。

原 M003 测试更新为完整能力快照和完整空候选结束消息，原有四项控制行为断言保留。Mac 相关九项 9/9（3.25 秒）；重新链接所有相关目标后，Mac **133/133（24.23 秒）**、Ubuntu **133/133（19.36 秒）**。原始日志和源码摘要见 [M025-test-results.json](M025-test-results.json)。这些都是公共协议/核心/既有监督回归，不新增媒体或平台认证。

Ubuntu 四个已有文件覆盖前均匹配 M024 阶段摘要，另三个新文件确认不存在后创建，未同步其他修改或重置历史。

## 影响与未完成项

- 原先只通过外层的简略/任意正文现在会被拒绝；旧开发夹具不是最终 v1 正文兼容实现。没有降级到宽松正文的回退。新控制入口尚未用于生产网络，既有生产媒体路径未变。
- 尚未把有效 JSON 解码装配为真实协商事务、能力缓存、计划仓库、当前候选查找、operationId 缓存、权限/期限与屏障登记检查，也未接入产品通道和 M024 mailbox。结构校验不能授予资源操作权限。
- 完整 SessionPlan 传输/引用仓库、跨机 RTP 帧关联、RDP 端点凭证与信令仍待实现。即使声明 available，也必须经过本机认证策略和真实探测，不据此发布未验证后端。
- 上游标准解析、实际媒体连接与呈现、Windows 编译及实测未增加。

源码、测试、契约文档和备忘录同阶段提交；通过 `git log --follow -- 本文件` 定位。
