# 扩展屏底层基础设施设计交付

版本 1.0 · 2026-10-27

主文档：[跨平台扩展屏底层基础设施架构设计](architecture.zh-CN.md)。共 30 个主体章节、4 个附录；覆盖 Windows、macOS、Linux 的发送与接收、多协议协商、资源与进程生命周期、安全和工程验证。不包含 UI 或用户操作流程设计。

配套文件：

- [公共数据契约 JSON Schema](contracts.schema.json)：ControlEnvelope、SessionPlan、ResourceRecord；后端标准信令仍需对应适配器语义验证。
- [示例协商信封](examples/control-envelope.json)、[示例会话计划](examples/session-plan.json)、[示例资源记录](examples/resource-record.json)：均为虚构占位数据，不能作为生产凭证或已支持能力声明。
- [40 项故障验证矩阵](fault-matrix.csv)：可作为后续测试用例登记输入，尚未执行实机故障注入。
- [开源参考阅读清单](reference-manifest.json)：文件 URL、内容 blob SHA 与固定提交记录。
- [文档契约校验结果](validation-result.json)：Schema 元校验、5 个有效示例、13 个应拒绝示例及结构检查。

设计已明确的内容：模块边界、权威状态、身份/代次、双端计划提交、资源所有权与耐久恢复、背压与停止语义、平台/媒体适配契约及安全底线。

仍需后端准入实验的内容：具体 WebRTC 引擎、RDP 身份适配、Windows 驱动、Mac 私有接口兼容范围、Linux 标准虚拟输出行为、GPU 表面连接和各方向协议策略。详见主文档第 30 节。

本交付是目标架构与可校验契约，没有实施协议迁移或宣告三平台产品验收通过。
