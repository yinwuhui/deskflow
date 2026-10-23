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
