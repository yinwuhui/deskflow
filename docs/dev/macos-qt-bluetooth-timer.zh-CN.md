# macOS Qt 6.8 蓝牙入站连接崩溃修复

2026-10-23，Mac arm64 服务端与 Ubuntu 26.04 客户端进行纯蓝牙联调。
系统配对成功后，Mac Qt 6.8.3 在首次接收 RFCOMM 连接时发生 SIGSEGV，
调用栈为 IOBluetooth RFCOMM 完成回调 → QtBluetooth → QTimer::stop()，访问地址 0x8。

## 原因与修复

Qt 的 DarwinBTRFCOMMChannel 仅在主动拨号时创建 channelOpenTimer，
入站连接没有计时器，但 rfcommChannelOpenComplete 仍无条件解引用。
Qt 官方 6.10 分支已对空计时器作保护：

- 原始代码：https://github.com/qt/qtconnectivity/blob/v6.8.3/src/bluetooth/darwin/btrfcommchannel.mm
- 官方修复后的代码：https://github.com/qt/qtconnectivity/blob/6.10/src/bluetooth/darwin/btrfcommchannel.mm

在 Qt 6.8.3 源码根目录应用项目内补丁：

```sh
patch -p1 < /path/to/deskflow/tools/patches/qt-6.8-rfcomm-server-timer.patch
```

补丁移除只适用于拨号连接的计时器断言，保留 delegate 检查，并在计时器存在时停止。
不改变 TLS、证书验证、配对确认或输入权限。

用与应用相同的 Qt 6.8.3 SDK 单独构建 qtconnectivity 的 Bluetooth 目标，
关闭模块测试和示例，使用匹配的 macOS SDK。模块配置和构建示例：

```sh
cmake -S /path/to/qtconnectivity -B /path/to/qtconnectivity-build -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/macos \
  -DCMAKE_BUILD_TYPE=Release \
  -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF
cmake --build /path/to/qtconnectivity-build --target Bluetooth --parallel 4
```

重新打包时，通过 tools/package-macos-preview.py 的
`--qt-bluetooth-library /path/to/qtconnectivity-build/lib/QtBluetooth.framework/Versions/A/QtBluetooth`
参数指定修复后的库。此覆盖发生在 macdeployqt 之后、原开发证书签名之前，
避免 macdeployqt 覆盖补丁。必须使用相同 Qt ABI、匹配应用架构的库。
普通打包不传这个参数会继续使用 SDK 原库；因此继续使用旧 SDK 时必须显式传入。
使用已经包含同等修复的新版 Qt 时不再需要此补丁。

## 本次环境与验证

- Ubuntu 远端源码为 32bd92aa，重新构建后 42 项 CTest 全部通过。
- Mac 使用 dist/home-refined/busymouse.app，应用源码版本 4cd17a0b；此次仅替换其 QtBluetooth arm64 运行库。
- 原框架为通用二进制，本次保留原 x86_64 切片，合入修复后的 arm64 切片；x86_64 切片未修复，不能据此声称 Intel Mac 已验证。
- 保留原应用标识及开发证书，重新签名后通过 codesign --verify --deep --strict。
- 两端应用证书指纹与互信记录匹配。系统旧配对记录失效，重新完成系统配对后恢复。
- Mac 仅开启蓝牙共享；Ubuntu 选择蓝牙首连并关闭自动备用。屏幕排列为 Ubuntu 在左、Mac 在右。
- 修复后 Ubuntu 记录 connected to secure socket，Mac 接受客户端并交换键盘布局；界面显示当前使用蓝牙、备用已关闭。
- 实体鼠标跨屏、点击、滚轮、键盘、剪贴板和重连验收另行记录，建立会话不等于这些功能已通过。

本次现场构建位置：`/tmp/bmouse-qtconnectivity-src`、`/tmp/bmouse-qtconnectivity-build`；
修复前框架备份：`/tmp/bmouse-QtBluetooth-before-fix`。临时目录不作为长期交付依赖。

本次修复后的 arm64 库、原 QtConnectivity 源码归档和补丁已额外保存在 Mac 的
`dist/qt-bluetooth-6.8.3-rfcomm-fix/`。更新后的预览 ZIP 包含本次运行库修复。

用户离开设备后暂停实体交互验收；上述待验项目尚无通过结论，后续从实体跨屏测试继续。

## 2026-10-26：监听器撤销后的入站回调崩溃

本次 Mac 使用 macOS 27.0.1（26A434）、Qt 6.8.3，Ubuntu Qt 为 6.10.2。
Linux 客户端选择蓝牙首连，Mac 同时开启网络与蓝牙共享。

### 现场证据

- 两机 Paired/Bonded/Trusted 均正常；应用管理层最初没有已认证 Peer。
- Linux 抓包显示链路 AES-CCM 加密成功，随后向 RFCOMM 通道 1 发送 SABM，业务连接未完成。
- Mac bluetoothd 收到入站请求，却记录 `No sessions found`；重新注册时出现
  `RFCOMM_Accept failed with status 916`。这定位到系统入站连接交付，尚不能单凭该日志归因 Qt。
- 停止 Mac 核心后，远端仍能查询 11 条同 UUID 的历史服务记录；Mac 蓝牙关闭再打开后只剩 1 条。
  原生临时服务的 removeServiceRecord 在调用端返回成功，但系统日志出现移除失败状态。
  残留记录与系统监听状态有关，其最初形成原因尚未完全确认。
- 恢复蓝牙后，Mac 确实接受连接并收到数据，随后核心 SIGSEGV。
  `busymouse-core-2026-10-26-145748.ips` 的故障帧为
  `-[DarwinBTSocketListener rfcommOpenNotification:channel:]+12`，由 IOBluetooth 主队列回调触发。
  此问题与上面的空计时器崩溃不同。

### 本次修复

`tools/patches/qt-6.8-bluetooth-listener-lifetime.patch` 修复 Qt 私有监听器生命周期：
显式停止时先清除 C++ delegate，再注销通知；保留 Objective-C 接收对象到主队列已排队的回调排空。
RFCOMM/L2CAP 回调均校验 delegate 与当前通知对象，忽略旧通知。
服务端关闭及析构均执行该停止逻辑。
该策略参考 Chromium 对 Apple FB13705522 的公开规避实现：
https://github.com/chromium/chromium/blob/main/device/bluetooth/bluetooth_socket_mac.mm
这不是 Qt 官方补丁，仍需持续实机回归；单次主队列延迟不能替代长期稳定性验证。

业务层 ConnectionManager 在旧端口仍可查找时先注销服务，随后断开旧 server 信号并关闭；
入站处理绑定具体 server，并处理 nextPendingConnection 返回空指针的情况。

继续使用 Qt 6.8.3 时须同时应用空计时器与生命周期两个补丁，然后重新编译 QtBluetooth。
打包时显式传入更新后的框架，不能仅使用原 SDK 或只含计时器修复的旧备份。
本次 arm64 运行库及两个补丁同步保存到 `dist/qt-bluetooth-6.8.3-rfcomm-fix/`；
Intel Mac 未验证。本次运行库是 arm64，不能据此宣称通用二进制已覆盖。

### 验证与边界

- Ubuntu 编译应用及 ConnectionManagerTests 成功；ConnectionManagerTests、TransportIpcTests、
  TransportNotificationsTests 三个相关 CTest 套件通过。
- Mac 编译、原开发证书签名、严格签名验证及预览 ZIP 校验成功。
- 修复版 Mac 与 Linux 建立蓝牙 TLS 会话：Linux 出现 `connected to secure socket` 并交换键盘布局；
  Mac IPC 显示蓝牙通道 active，应用界面显示“已连接 / 当前使用蓝牙”。
- 对已连接的修复版再关闭并开启 Mac 蓝牙，核心未复现该崩溃，随后蓝牙会话恢复。
  期间有连接重试及一次只读 SDP 查询，未测精确恢复耗时；不能据此承诺快速、完全自主恢复。
- 默认构建触发全套 131 项测试，其中 126 项通过；MainWindowHomeTests 与 4 项
  FixedWindowScale 测试异常退出，离屏模式复核仍失败，尚未归因，不能宣称全套通过。
- 启动时蓝牙关闭后的自动启用、Mac 本机蓝牙名称暂不可获取、引导连接失败未暴露到界面的问题
  仍需后续处理。本次未修改引导连接的状态协议。
- 本次没有验收实体跨屏、点击、键盘、剪贴板、视频或端到端延迟。

抓包保留在 Ubuntu `/tmp/bmouse-bluetooth-debug.btsnoop`，包含蓝牙链路信息，
未提交到仓库。两端时钟不同，不能直接用绝对时间相减计算延迟。
