# M020：媒体 Prepare v3 的稳定采集源证据

2026-10-28。对应架构第 5、9、11、13、14、19、26 节；推进隔离媒体 Worker 的源身份核查，不代表已实现媒体引擎或产品装配。

WorkerPrepare 新增 sourceIdentity、sourceContextDigest、sourceTopologyGeneration。编码到 source 对象的 identity/contextDigest/topologyGeneration；代次用十进制字符串，避免 JSON 浮点丢精度。发送方向必须有非零代次、SHA256 形状的上下文摘要和有界平台身份；Mac CGDisplay 身份必须是非空、无花括号、小写规范 UUID，且 display ID 在 uint32 范围。PipeWire/Windows 身份为有界机器标识，不允许路径、命令或任意文本。接收方向必须没有任何来源证据，不能通过这些字段获得采集权。

媒体上下文升级为 display-worker/3，bootstrap 升级为 display-worker-bootstrap/3；平台 helper 保持独立 display-platform/1。精确 typed schema 要求新字段存在，没有旧协议降级。既有加密封装/HMAC 算法未改变，版本由认证上下文绑定。生产仍使用旧媒体入口，此版本变动只影响新原生 Worker 实验链。

新增 WorkerSourceBinding 测试：完整源证据经过 IPC 编解码后逐字段一致；缺失/非法身份、非规范 UUID、摘要及零代次拒绝；接收端身份/拓扑字段拒绝；PipeWire 类型化证据结构可用。新增真实 WorkerLegacyBootstrap：向新 Worker 发送旧 /2 bootstrap，必须在建立连接前退出 42，不回退或启动媒体。

实际测试：Mac 重新链接完整相关目标后 118/118（26.81 秒）。首次定向 Worker 测试中 WorkerParentEof 有一次 no worker connection 的两秒夹具等待失败；后续完整回归及该单项独立连续三次（1.80 秒合计）均通过。未把这一次失败删除或当作已证明的协议缺陷；目前仍作为间歇启动时间项保留，后续需结合冷启动/负载验证。Windows 未编译实测。

Ubuntu 同步前七文件摘要全部与本阶段起点一致，仅同步这七个公共源码/测试文件，保留其余修改和不同历史。重新链接后完整相关回归 118/118（18.55 秒）。

未完成项：这些检查证明形状和角色边界，不能证明 sourceIdentity 对应活跃 OS 资源。Worker 仍必须在原生采集前核对 ID+稳定身份+拓扑，并遵守授权撤销。尚未把 M019 本地绑定序列化给真实媒体 Worker，也没有媒体、呈现或跨机证据。文件、测试和本备忘录同阶段提交，git log --follow -- 本文件可定位。
