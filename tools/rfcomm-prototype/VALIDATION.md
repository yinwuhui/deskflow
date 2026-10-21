# 验证记录：2026-10-21

范围：Mac 蓝牙后端开发。按用户要求，产品 UI 未修改，Linux 联调暂缓。

## 已完成并验证

- macOS 27.0.1、Apple Silicon arm64、AppleClang 21、Qt 6.8.3、macOS 15.2 SDK。
- 根项目 `BUILD_BLUETOOTH=ON` 配置成功，deskflow-core、原有 GUI 和蓝牙后端编译链接成功。
- 独立诊断工具使用同一 `deskflow-bluetooth` 库编译成功；原生配对 Objective-C++ 源码编译成功。
- 6 组定向回归通过：RfcommBackendTests、ServerProxyTests、ClientProxyTests、ServerConfigTests、ServerTests、MdnsLifecycleTests。
- 蓝牙测试程序记录 15 项通过（含初始化和清理），覆盖 13 个功能/数据用例：无效桥接配置、无效服务发现、无效配对、碎片与合并帧、6 组异常协议输入、RTT、静默超时、200KB 双向二进制转发及断线后的全新连接。
- 实际打包核心的 `--bluetooth-scan` 正常退出，并收到 9 条设备发现事件；没有向发现的设备配对或连接。
- 诊断工具实际扫描和 RFCOMM 服务注册成功；测试白名单地址监听输出 `listening_rfcomm`，到期退出。
- 最终核心 CLI 验证：非法地址/冲突动作退出码为 3，关闭 TLS/证书检查退出码为 4。使用隔离的临时设置，没有启动键鼠捕获。
- Mac 包部署完整 Qt 依赖并进行 ad-hoc 签名，`codesign --verify --deep --strict` 通过。
- 日志统计脚本固定样本验证、启动器语法检查和 `git diff --check` 通过。

## 已实现，尚未做真实设备间验证

- 原生 Mac 配对确认码/PIN 交互、明确确认/取消和超时。
- 内嵌 RFCOMM 与已有 Deskflow TCP/TLS 核心连接。
- 断线立即中止流，销毁旧会话和队列，下一次连接重新握手。
- 监听器错误重试注册、适配器电源事件处理。
- 链路 RTT 和恢复时间统计接口。

自动化使用本机 TCP 字节流模拟 RFCOMM 对端，验证共享会话代码，不代表无线传输性能。
真实配对确认、Linux 构建、跨机数据、键鼠延迟、卡键释放、关闭蓝牙及休眠后的恢复、大剪贴板负载测试均暂缓。

## 交付

- `dist/bluetooth-backend/Deskflow.app`：原有 UI，核心支持蓝牙启动参数。
- `dist/rfcomm-prototype/deskflow-rfcomm.app`：扫描/服务发现/配对/桥接/RTT 诊断命令行工具。
- 开发接口和核心使用方式见 `docs/dev/bluetooth.md`。

Mac 包为本地 ad-hoc 签名，未做正式发布公证。默认根项目构建开关为 OFF，蓝牙运行需显式启用启动参数。
