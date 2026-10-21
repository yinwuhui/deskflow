# macOS → Linux RFCOMM 原型

本原型用于两台已配对的电脑，支持独立 RTT 探测、现有 Deskflow 数据转发、断线关闭和重新建连。
当前按需求仅完成并验证 Mac 端，暂不进行 Linux 联调。本工具与已接入核心的蓝牙后端共享实现，便于独立诊断；尚未改动产品 UI。核心直接使用蓝牙的方式见 [后端说明](../../docs/dev/bluetooth.md)。

数据路径：

```text
Mac Deskflow 服务端 ← 本机 TCP 127.0.0.1:24800 → Mac 桥接程序
                                               ⇅ 蓝牙 RFCOMM
Linux Deskflow 客户端 ← 本机 TCP 127.0.0.1:24800 → Linux 桥接程序
```

两台电脑之间使用蓝牙；TCP 仅用于每台电脑内部。桥接不解析键鼠消息或 TLS，现有 Deskflow 核心负责设备证书、键鼠处理和断线后的输入状态清理。实际按键释放仍需硬件实测。

## Mac 现成包

本机交付位于仓库 `dist/rfcomm-prototype/`，包含已部署依赖的 `deskflow-rfcomm.app` 和 `run-macos-server.command`。
这是命令行程序，直接双击 app 不会出现配置窗口。需要运行服务端时，双击 `.command` 并填写允许接入的 Linux 蓝牙地址；Ctrl+C 停止。

当前不联调时，可在仓库根目录仅验证 Mac 监听启动（测试地址不会连接任何远端）：

```sh
BT="$PWD/dist/rfcomm-prototype/deskflow-rfcomm.app/Contents/MacOS/deskflow-rfcomm"
"$BT" --server --peer 02:00:00:00:00:01 --duration 3
```

输出 `listening_rfcomm` 表示服务注册成功。开发和验证范围见 [VALIDATION.md](VALIDATION.md)。

## 构建

需要 CMake 3.24+、C++20、Qt 6.7+ 的 Core、Network、Bluetooth 模块；测试需要 Qt Test。
Mac 端还需要 Qt Gui 来显示系统蓝牙授权弹窗（原型本身没有窗口）。Qt 在线安装器中的模块名称为 Qt Connectivity。Linux 需要 BlueZ 和运行中的蓝牙服务。

从仓库根目录运行（路径含空格时加引号）：

```sh
cmake -S tools/rfcomm-prototype -B build/rfcomm -DCMAKE_PREFIX_PATH="你的 Qt 安装目录"
cmake --build build/rfcomm
ctest --test-dir build/rfcomm --output-on-failure
```

本机使用 Qt 6.8.3 + macOS 15.2 SDK 构建；较新的 SDK 如果已移除 AGL，可安装兼容 SDK 并在配置时添加 `-DCMAKE_OSX_SYSROOT=SDK完整路径`，或使用适配新 SDK 的 Qt。

macOS 二进制位于 `build/rfcomm/deskflow-rfcomm.app/Contents/MacOS/deskflow-rfcomm`；
Linux 位于 `build/rfcomm/deskflow-rfcomm`。以下命令用 `BT` 环境变量代表该程序的完整路径。
macOS 如需独立运行，先用 Qt 的 `macdeployqt` 部署整个 app，并进行本地签名：

```sh
macdeployqt build/rfcomm/deskflow-rfcomm.app
codesign --force --deep --sign - build/rfcomm/deskflow-rfcomm.app
```

首次运行 macOS 端会请求蓝牙权限。请在系统隐私设置允许该原型使用蓝牙。
Mac Deskflow 本身仍需要辅助功能/输入监控权限。Linux Wayland 的输入权限沿用当前 Deskflow 的要求。

## 1. 系统配对和地址

先在两端系统蓝牙设置中完成配对。Linux 可使用 `bluetoothctl` 的 `power on`、`scan on`、
`pair MAC地址`、`trust MAC地址`；按系统提示确认配对码。无需通过 `bluetoothctl connect` 打开音频等其他配置文件。

可以扫描经典蓝牙设备：

```sh
"$BT" --scan --duration 20
```

记录 Mac 与 Linux 的蓝牙地址，格式为 `AA:BB:CC:DD:EE:FF`。Mac 端使用 Linux 的地址作为允许接入的设备；Linux 端使用 Mac 的地址。
系统发现可见性与服务注册不是同一回事；扫描不到时先检查系统蓝牙可发现设置。

## 2. 先测链路，不启动 Deskflow

Mac（将示例地址替换成 Linux 的蓝牙地址）：

```sh
"$BT" --server --peer AA:BB:CC:DD:EE:FF 2>&1 | tee mac-probe.log
```

Linux（将示例地址替换成 Mac 的蓝牙地址）：

```sh
"$BT" --peer 11:22:33:44:55:66 --probe --duration 120 2>&1 | tee linux-probe.log
python3 tools/rfcomm-prototype/analyze-log.py linux-probe.log
```

`session_ready mode= probe` 表示 RFCOMM 应用握手完成；每秒输出一条 `rtt_ms`。
统计脚本输出样本数、P50/P95/最大 RTT、桥接恢复时间。探测流量不包含真实键鼠输入。
`--duration` 仅控制运行时间，进程退出成功不代表曾连接成功；必须检查 ready 和样本数。

服务端只接受指定蓝牙地址、一次一条连接，识别固定服务 UUID `7e349c68-6726-4b5c-a1d4-6c80f21cd980`。
地址白名单用于限制原型接入，不能替代 Deskflow 的证书身份验证。Qt 在 macOS 上不支持手动设置蓝牙 socket 安全标志，链路安全由系统管理；Deskflow 的 TLS 与证书检查仍需保持启用。

## 3. 接入真实键鼠

结束 Linux 探测进程后，Mac 端可保持运行。

1. 在 Mac 现有 Deskflow GUI 中配置服务端，添加 Linux 的电脑名称并设置相对屏幕位置。
2. Mac 服务端的监听地址设为 `127.0.0.1`，端口 `24800`，启动服务端。可在配置文件中检查 `[core] interface=127.0.0.1` 和 `port=24800`。
3. 两端均保留 TLS 加密和设备证书指纹检查。首次连接时按现有 Deskflow GUI 流程核对并信任对端证书。
4. 初次键鼠测试先关闭剪贴板共享，排除大数据传输的影响。
5. Linux 启动桥接监听：

```sh
"$BT" --peer 11:22:33:44:55:66 --port 24800 2>&1 | tee linux-bridge.log
```

6. Linux Deskflow GUI 选择客户端，远端主机填写 `127.0.0.1`、端口 `24800`。电脑名称必须与 Mac 布局中一致，启用客户端自动重试连接。不要同时运行另一个占用该端口的服务端。
7. 移动鼠标跨越 Mac 屏幕边缘，在 Linux 编辑器输入、拖动窗口、滚动，确认输入正确、无重复字符和卡键。

若两端端口需不同，例如 Linux 24801，只需同时修改 Linux 桥接 `--port` 和 Linux Deskflow 客户端端口；Mac 设置保持不变。
桥接的 `--port` 在 Mac 端表示要连接的本机 Deskflow 服务端端口，在 Linux 端表示本机桥接的监听端口。

RTT 仅衡量传输往返时间，包含排队影响，不能直接当作单向键鼠延迟。实际输入到画面响应可用高帧率摄影记录物理按键与 Linux 画面变化，重复至少 20 次；记录键盘、显示器刷新率和系统版本。

## 4. 验证断线恢复

可以先做一次软件断线：

```sh
# Linux：连接成功后第 10 秒主动断开一次；探测模式会自动重试。
"$BT" --peer 11:22:33:44:55:66 --probe --drop-after 10 --duration 45 2>&1 | tee linux-recovery.log
python3 tools/rfcomm-prototype/analyze-log.py linux-recovery.log
```

真实键鼠模式同样支持 `--drop-after 10`。断线后两端关闭本机 TCP 连接，丢弃旧会话队列；Linux Deskflow 客户端下一次连接会触发新的蓝牙连接和 TLS 握手。桥接不会重放断线前的数据。

继续测试真实硬件故障：

- Linux 正在接收输入时，关闭任一端蓝牙 5 秒再开启，确认输入停止、Mac 本机恢复可用，并在恢复后重新跨屏操作。
- 在测试编辑器中按住 Shift 或鼠标按钮后断开，确认 Linux 没有卡键或持续拖动；测试时避免操作重要文档。
- 两端分别休眠/唤醒、退出重启桥接，重复至少 5 次。

Mac 端收到监听器错误后，每 2 秒尝试重新注册 RFCOMM 服务；蓝牙电源切换和休眠恢复的实际行为仍需后续硬件验证。

静默链路/握手默认在约 10 秒后超时；系统明确报告断开时立即关闭会话。探测模式重试间隔为 1、2、4、8 秒（上限）；真实键鼠模式的重试时机由 Deskflow 客户端决定。
统计脚本的恢复时间从桥接检测到断开开始，到下次桥接 ready 结束，不包含检测前的等待，也不代表 TLS/键鼠已恢复；最后一段需通过核心连接日志及实际操作确认。

## 验收记录

建议保存两端日志，并填入下表。目标值是本原型的测试建议，不是已测性能承诺。

| 项目 | 建议验收 | 当前状态 |
|---|---|---|
| macOS 构建 | 编译及打包成功 | 本机已验证，详见 VALIDATION.md |
| Linux 构建 | 在目标 Linux 上完成构建 | 待目标机器验证 |
| RFCOMM 实连 | 配对后双向握手成功，连续探测 120 秒 | 待两机验证 |
| 链路 RTT | P95 小于 30ms，可按实测调整 | 待两机验证 |
| 实际键鼠 | 无丢键、重复输入、卡键；主观操作流畅 | 待两机验证 |
| 断线恢复 | 检测断开后 15 秒内恢复，5 次均成功 | 待两机验证 |
| 剪贴板 | 基础键鼠通过后，再测试文本与大数据对延迟的影响 | 待两机验证 |

## 自动化覆盖范围与限制

测试用本机 TCP 字节流替代 RFCOMM，运行相同 RfcommSession 实现，覆盖碎片帧、合并帧、错误协议、超大帧、RTT 回显、超时、双向二进制传输和断线后全新会话。
它不模拟系统蓝牙驱动、配对、无线干扰、真实键盘注入或系统休眠。

诊断工具支持 `--pair 蓝牙地址` 发起原生配对并按提示确认，支持 `--services 蓝牙地址` 查找 Deskflow 服务。尚无连接方式 UI、多个蓝牙客户端、网络/蓝牙无缝切换或正式发布签名。仅支持 Qt 已支持的经典蓝牙 RFCOMM 硬件/系统组合；尚未在目标 Linux 发行版编译运行。

API 依据：[Qt 6.8 Bluetooth](https://doc.qt.io/qt-6.8/qtbluetooth-index.html)、[QBluetoothSocket](https://doc.qt.io/qt-6.8/qbluetoothsocket.html)、[QBluetoothServer](https://doc.qt.io/qt-6.8/qbluetoothserver.html)。
