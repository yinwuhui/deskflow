# M005：媒体 Worker 类型化 IPC 契约

## 边界及设计对应

完成设计第 4、5、13.4 节的**进程间协议层**，代码为 `src/lib/display/supervision/WorkerProtocol.{h,cpp}`，测试为 `src/unittests/display/WorkerProtocolTests.cpp`。不包含 socket/pipe 建立、原生 peer credentials、进程创建和终止；这些由随后监督器模块提供。当前产品入口尚未接入本协议。

`WorkerBinding` 固定绑定 session、attempt、local generation、epoch、resource owner、worker instance、本地用户会话、构建兼容版本、白名单 backend/profile 和 plan hash。跨进程传输前必须由本地可信装配器构造，不允许网络 JSON 指定可执行文件。

## 实现及关键不变量

- IPC 版本 `display-worker/1`，4 字节网络序长度前缀；消息 8 KiB，接收块及累积缓冲、待分发队列（32 条）分别有界。先检查长度，再走已有严格 JSON 解析器。
- 父子两端使用独立随机 nonce，以经私有继承通道交付的 32 字节启动秘密证明启动实例。角色、版本和完整绑定参与证明，握手后派生 HMAC-SHA256 会话密钥，并清除本对象的启动秘密。秘密不进入 argv/环境/错误文本。Qt 调用方的原始启动秘密仍须由监督器清除。
- 每条消息有角色、严格连续序号和 MAC；拒绝重放、错序、不同启动实例和变更字段。比较 MAC 时采用固定长度逐字节累积比较。
- `feed` 还要求调用者提供已独立验证的本地传输身份。MAC 不替代 UID/ACL/进程身份验证；此布尔参数是可信平台适配器契约，不能从消息中读取。
- 公共消息为封闭 variant：Start、Stop、Heartbeat、Refresh、Ready、Presented、Quiesced、Failure，无任意正文、媒体字节或资源路径。Ready 后才可 Start，开始后才可报告递增帧；Stop/Failure 后不得重新开始。
- Quiesced 只表示 Worker 已停止活动，**不代表系统资源已验证清理**。权威清理仍由生命周期和资源执行器验证。
- 握手绝对 3 秒；建立后接收租约 1.5 秒，仅真实有效入站消息续期，出站心跳不能维持已失联的父进程。EOF、错误或超限清除待处理动作，进入不可复用失败态。调用者必须立即停止/关闭并独立清理资源。

## 实际验证

Mac arm64、Qt 6.8.3、本地 CMake 构建；7/7 CTest 通过（2026-10-27）。

```sh
cmake --build /private/tmp/deskflow-build --target DisplayWorkerProtocolTests -j 4
ctest --test-dir /private/tmp/deskflow-build/src/unittests -R '^Display.Infrastructure.Worker' --output-on-failure
```

覆盖：双方握手/错误秘密/未经认证的本地通道；8 种绑定不一致；逐字节分片和合并消息；超限长度及读取预算；Ready/Start/Stop 顺序与方向；呈现证据；旧进程消息和重放；消息篡改；握手和租约截止；EOF；队列上限及失败后不交付旧动作。首次运行发现 ID 字节数与十六进制字符数混用导致合法握手被拒绝，修正后全部通过。

## 审查、迁移及剩余事项

本模块不改变已有运行中的 Python 媒体路径，可独立回滚。需要审查挑战通过私有通道交付、平台凭据判断、进程退出和资源验证之间的接线；仅本模块通过不构成这些集成项已完成的证据。尚未在 Ubuntu/Windows 执行本模块；媒体库和实机呈现均未认证。后续监督器需要为 socket 读写设相同的硬预算、周期调用 tick、按绑定限制可执行文件，并将失败变成自停和资源恢复。

提交定位：本备忘录与协议实现/测试在同一提交，`git log --follow -- docs/dev/extended-display-architecture/memos/M005-worker-ipc.zh-CN.md`。


## 后续跨平台验证补记

M006 阶段在 Ubuntu x86_64 / Qt 6.10.2 构建并执行前五个模块的 37 项测试，全部通过。Mac 仍为主开发环境；此补记不代表产品链路或 Windows 认证。
