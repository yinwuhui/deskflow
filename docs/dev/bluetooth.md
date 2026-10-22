# 蓝牙后端（Mac 开发阶段）

自动主备初版现已接入核心，包含非阻塞恢复通知；配置与验证边界见 [auto-failover.md](auto-failover.md)。下文介绍单通道蓝牙模式。

当前完成 Mac 端后端实现，按需求暂不改动产品 UI、不进行 Linux 联调。
核心通过 `--bluetooth-peer` 启用内嵌 RFCOMM 桥接，无需另起桥接进程；未指定该参数时沿用原来的网络连接。

## 后端组成

源码在 `src/lib/net/bluetooth/`：

- `BluetoothDiscovery`：经典蓝牙设备扫描、按固定 UUID 发现 busymouse 服务、超时和取消。
- `BluetoothPairing`：Mac 使用 IOBluetoothDevicePair 原生配对；发出确认码、PIN 请求和配对结果信号，支持明确确认、取消和 60 秒超时。Linux 使用系统 BlueZ 配对代理；该分支未做目标机器验证。
- `RfcommBridge`：服务注册/监听、设备地址白名单、单连接限制、客户端拨号、连接超时、监听错误重试、适配器电源事件处理、显式停止、连接状态和 RTT 信号。
- `RfcommSession`：协议版本握手、分包/合包、双向转发、帧大小校验、有限写队列、回压、心跳/RTT、静默超时和旧会话销毁。

所有蓝牙对象在 Qt 主线程运行，已有核心继续在核心线程运行。未来 UI 应在主线程调用这些 API，先通过 `requestBluetoothPermission` 请求权限，再扫描、配对或启动桥接。
数字比对和 PIN 不会自动确认；UI 可连接 `confirmationRequired` / `pinRequired` / `passkeyDisplayed`，然后调用 `confirm` / `providePin`。探测工具提供等价的命令行交互。

## 数据路径与身份验证

```text
本机 busymouse 核心 ← TCP 回环 → 内嵌 RfcommBridge
                                ⇅ RFCOMM
远端 busymouse 核心 ← TCP 回环 → 内嵌 RfcommBridge
```

蓝牙承担两机之间的传输。回环桥接保留已有 TLS、证书指纹校验、键鼠处理及输入状态释放逻辑。
蓝牙模式强制要求现有配置启用 `security/tlsEnabled` 和 `security/checkPeerFingerprints`；关闭任一项会在启动核心前拒绝。

运行时覆盖连接地址，不改写已有网络主机、端口或网卡配置。服务端仅监听 `127.0.0.1`，停止发布网络 mDNS；客户端使用自动分配的本机回环端口，并跳过物理网卡绑定。
断线时立即 abort 两端流，销毁旧队列。客户端通过已有 busymouse 重试流程创建新的 RFCOMM/TLS 会话，避免重放旧输入。
Mac 的蓝牙安全参数由系统管理，应用层始终依赖已有 TLS 和证书验证。设备蓝牙地址白名单不替代证书身份验证。

首次使用前，两端应已配对，并完成现有 busymouse 证书信任。可沿用原有 GUI 的信任流程；无界面环境需要核对对端证书 SHA256 后，按现有 `FingerprintDatabase` 格式维护对应的 `trusted-clients` / `trusted-servers` 文件（`v2:sha256:` 加 64 位十六进制指纹）。不要关闭验证来跳过此步骤。

## 构建和运行

根项目新增可选开关，默认关闭，避免未安装 Qt Bluetooth 的环境受影响：

```sh
cmake -S . -B build -DBUILD_BLUETOOTH=ON -DCMAKE_PREFIX_PATH="Qt 安装目录"
cmake --build build --target busymouse-core
```

需要 Qt Core/Gui/Widgets/Network/Bluetooth 和项目原有依赖；Mac 构建系统启用 Objective-C 与 Objective-C++，自动链接原生配对框架，并在 app plist 声明蓝牙用途。
本机 Qt 6.8.3 使用 macOS 15.2 SDK 构建。新 SDK 若已移除 Qt 所需 AGL，请使用匹配的 SDK 或更新 Qt。

Mac 核心位于应用的 `Contents/MacOS/busymouse-core`。以下 `CORE` 需设置为其完整路径。

```sh
# 扫描设备，20 秒后结束；不会启动键鼠捕获。
"$CORE" --bluetooth-scan

# 在指定电脑上查找 busymouse RFCOMM 服务。
"$CORE" --bluetooth-services AA:BB:CC:DD:EE:FF

# Mac 服务端：地址是唯一允许接入的 Linux 设备；配置文件沿用现有布局/证书。
"$CORE" server --settings /完整路径/mac-settings.ini --bluetooth-peer AA:BB:CC:DD:EE:FF

# 后续 Linux 联调时：地址为 Mac 的蓝牙地址。
"$CORE" client --settings /完整路径/linux-settings.ini --bluetooth-peer 11:22:33:44:55:66
```

服务端内部 TCP 端口取现有 `core/port` 配置；若端口被占用，核心会按原有启动流程报错。客户端使用空闲回环端口。
不要同时启动 GUI 管理的核心和手动启动的核心；默认单实例检查仍有效。

## 独立诊断工具

`tools/rfcomm-prototype` 共享同一后端库，可单独构建；不会要求重编整个产品。

```sh
# 系统授权后，发起原生配对；按提示核对两端数字，再输入 yes/no，或输入 PIN。
"$BT" --pair AA:BB:CC:DD:EE:FF
# 已配对则直接报告成功。取消可 Ctrl+C；程序退出时会清理请求。

"$BT" --services AA:BB:CC:DD:EE:FF
"$BT" --scan --duration 20
"$BT" --server --peer AA:BB:CC:DD:EE:FF
"$BT" --peer 11:22:33:44:55:66 --probe --drop-after 10 --duration 45
```

独立工具的 bridge 模式不解析本机 TLS 字节流，需自行保持两端 busymouse TLS 设置；集成核心入口会强制检查。
RTT 每秒采样；使用 `analyze-log.py` 统计 P50/P95/最大值和桥接恢复时间。它不等于物理按键到画面响应的延迟。
详细两机步骤见 [原型操作说明](../../tools/rfcomm-prototype/README.zh-CN.md)。

## 目前的验证边界

已完成 Mac 编译、独立工具真实蓝牙扫描/服务注册、核心命令入口和自动化回归。
真实配对确认、跨机 RFCOMM 数据传输、键鼠延迟、卡键释放、关蓝牙/休眠后的恢复仍需后续两机实测。
监听器错误会每 2 秒重试注册；适配器电源事件会关闭旧会话并触发恢复，但不将实现逻辑等同于硬件验证结果。

当前不包含网络与蓝牙的无缝热切换、多蓝牙客户端或产品 UI。Mac 分发包使用本地 ad-hoc 签名，未做正式发布公证。
