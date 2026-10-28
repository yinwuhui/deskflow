# M010：媒体进程准备与标准信令的类型化契约

2026-10-28。对应设计第 4、5、9、13、14 节。

## 完成范围

新增 `src/lib/display/supervision/MediaWorkerContracts.{h,cpp}`，扩展 `WorkerProtocol`/`WorkerProcess` 及其测试。完成同机媒体进程启动后的 Prepare/Ready/Start 边界、源资源定位符、格式/队列预算与 WebRTC 标准信令的结构化承载。尚未实现具体平台资源提供者、原生媒体 worker 主程序或把消息接入产品基础连接。

## 接口、版本与不变量

- 内部 IPC 与 bootstrap 升到 `display-worker/2` / `display-worker-bootstrap/2`；Hello/KDF 使用版本 2 域。旧版不能误当新版接受。当前原生进程契约尚未用于已发布生产路径，不隐式兼容未知旧 helper。
- 启动绑定新增 localIdentity、peerIdentity、controlBinding、localDirection，全部参与启动身份摘要；身份不可相同，不能在同一个 worker 内改绑。原 owner/session/attempt/generation/epoch/worker instance/build/profile/plan hash 绑定继续有效。
- 明确 `Prepare → Ready → Start`。Prepare 只能由 agent 发送一次，必须匹配已白名单选择的协议/安全剖面；worker 无配置不得 Ready，Ready 不续期，禁止启动后换配置。
- 配置含 VideoFormat、固定大小队列预算、同机 steady-clock 绝对准备截止，以及经过本地 agent 核准的 peer IP 列表。截止最多剩余 10 秒；原始帧最多 6 槽/64 MiB，编码队列最多 2 帧/4 MiB。实际后端仍须实现/证明这些预算，传入数值本身不是证明。
- 发送端必须有 ResourceId 和类型化 CGDisplayID / PipeWire node / Windows adapter+target 定位符；接收端必须没有捕获源。定位符用有界整数，不传路径、命令、任意插件或裸进程地址。它只解决传递形状，平台提供者仍要独立确认资源归属及当前代次。
- peer 地址必须为 IP，拒绝 DNS 名、重复、未指定地址、广播/多播。loopback 仅显式本地诊断开关允许；该开关不能从公共 peer 请求直接导入。完整网络授权/接口约束仍由可信 agent 和后端执行。
- WebRTC 消息为 Offer、Answer、ICE、EndOfCandidates，只有单视频 m-line=0；SDP 解码 UTF-8 最多 32 KiB，candidate 最多 1 KiB、禁止控制字符，每方向每 attempt 最多 64 个 candidate。发送 worker 只发 Offer，接收 worker 只发 Answer，agent 转交相反方向；不同 SDP 不允许隐式重新协商。
- 这些是结构/角色预算检查，**不是手写的完整 SDP/ICE 解析器**。媒体适配器必须继续用成熟引擎解析标准正文，验证 fingerprint、协议、单视频、实际格式及候选地址政策。
- IPC 完整编码消息上限 64 KiB；启动材料仍限 8 KiB；待分发最多 72 项，以容纳 64 个候选及状态/SDP 突发；写积压上限约 256 KiB。满时失败关闭，不能无限排队。Stop 后晚到的已发送信令只校验/消耗序号，不再分发执行。

## 公共 JSON 边界修正

原严格解析器把引号/转义字节算入字符串 32 KiB 限额，导致合法上限 SDP 被误拒绝。现按解码后的 UTF-8 字节数检查 32 KiB；lexical token 另有转义最坏情况硬上限，公共完整消息仍先受 64 KiB 限制。duplicate key、非法 Unicode、深度、字段/数组和总量检查保留；新增原始上限、超限与转义测试。不以放宽完整消息预算绕过限制。

## 实际验证

Mac arm64 / Qt 6.8.3 与 Ubuntu x86_64 / Qt 6.10.2：四项新增契约测试及既有 IPC/真实进程/严格 JSON，共 17 项分别通过。新增覆盖：合法/非法准备与源方向、IP/内存预算、无配置 Ready 拒绝、配置不可替换；32 KiB SDP 穿越 IPC/超限拒绝；64 candidate 上限/换行注入；Offer/Answer 方向与重新协商拒绝；Ready 不延长绝对截止。

两端完整相关回归均 **109/109**：Mac 17.58s，Ubuntu 12.68s；包括 71 项新增基础设施测试和 38 项既有显示/用途服务回归。

```sh
cmake --build <build> --target DisplayWorkerProtocolTests DisplayWorkerProcessTests DisplayInfrastructureControlTests -j 4
ctest --test-dir <build>/src/unittests -R '^Display.Infrastructure.(Worker|StrictJson)' --output-on-failure
ctest --test-dir <build>/src/unittests -R '^Display\.(Infrastructure\.|Service\.|UsageService\.)' --output-on-failure
```

Mac 工具沙箱首次阻断 Qt 的宿主 CPU 特性探测并报缺少 neon；在宿主正常测试权限下重跑通过，没有修改 SIMD 检查或把该初始化失败算作通过。持久构建工具目录为 `/Volumes/KINGSTON 250G/Code/bmouse-dependencies/build-tools`；构建目录仍 `/private/tmp/deskflow-build`。

## 接入与审查重点

生产入口仍为旧运行时，版本变更不代表已切换视频协议。后续必须由可信平台/会话装配器生成 Prepare，严格验证来源资源；为 PipeWire/native GPU 句柄添加 OS 句柄转移；实现标准 SDP/IP/DTLS 校验、native 采集/呈现和实际预算；按真实 paint 证据发布 Streaming。平台 helper 的本地资源命令尚待实现。Windows 未编译/实测。

本备忘录与源码/测试同一阶段提交，可通过 `git log --follow -- <本文件>` 定位。
