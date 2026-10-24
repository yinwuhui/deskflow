# Linux Wayland 锁屏输入

Linux 接收端可选择 `client/linuxUinput=true`，通过内核虚拟键盘、鼠标接收输入。
设置界面入口：输入 → 接收：键盘和滚动 → 允许锁屏输入（英文为 Allow input on the lock screen）。
仅在 Linux Wayland 显示，默认关闭；修改后在桌面未锁屏时重新启动接收端。

## 数据路径与兼容性

网络 TCP 和蓝牙 RFCOMM 继续使用现有连接、认证和协议：

```text
发送端 → TCP / RFCOMM → 接收端键鼠事件 → uinput → 内核输入系统 → 桌面或锁屏界面
```

启用后，键盘、鼠标按键、绝对移动、相对移动和滚轮统一走 uinput，避免和 libei 重复注入。
仍保留 Portal/EIS，用于屏幕区域、键盘映射和剪贴板；停止 EIS 输入模拟不会停止 uinput 输入。
剪贴板是否能在锁屏时使用仍取决于桌面，锁屏输入模式不改变这一点。
若 Portal/EIS 报错，uinput 输入保持运行并保留最后已知的屏幕区域、键盘映射；
需要重新启动接收端以恢复失效的 Portal 功能。第一次连接仍应在未锁屏时完成桌面授权和布局初始化。

Windows、macOS 和 Linux X11 的输入后端保持原样，不编译 Linux uinput 代码。
Windows/macOS 发送端无需修改协议或启用此设置。关闭设置后，Linux Wayland 恢复原来的 libei 输入路径。

## 权限与安装

`busymouse-uinput-helper` 是独立的小型设备创建程序。核心通过 `pkexec` 请求管理员授权，
辅助程序创建三个固定能力的虚拟设备，通过 Unix socket 的 `SCM_RIGHTS` 传递设备句柄，随后退出。
连接使用随机名称的抽象 Unix socket；核心校验辅助进程的 root 身份和 PID，辅助程序校验接收方 UID。
辅助程序不读取网络、配置、键鼠事件、密码或调用方指定的文件路径。
GUI 和核心仍以普通用户运行，只持有这些新建虚拟设备的句柄，不需要读取真实键鼠设备。

Linux 构建会生成辅助程序，正常 CMake 安装会将其安装至配置的 `libexec` 目录。
默认 `/usr/local` 前缀的开发构建可只安装辅助程序：

```sh
cmake --build build --parallel 4
sudo install -D -o root -g root -m 0755 \
  build/bin/busymouse-uinput-helper /usr/local/libexec/busymouse-uinput-helper
```

运行环境需要 `/usr/bin/pkexec` 和 `/dev/uinput`；若设备节点不存在，可运行 `sudo modprobe uinput`。
不要把整个 GUI/核心改为 root 运行，也不必将 `/dev/uinput` 设置为所有用户可写。
辅助程序应由 root 拥有并安装在普通用户不可修改的系统目录。

首次启用时，从 Ubuntu 本机桌面启动程序，并完成管理员授权。SSH 启动的进程可能属于另一会话，
找不到桌面授权代理；设置 DISPLAY/DBUS 环境变量不一定能改变这种会话归属。
授权取消、超时或辅助程序缺失时，核心会报告启动错误，不会悄悄退回不支持锁屏的输入方式。

## 输入状态

- 独立的键盘、绝对坐标鼠标和相对移动鼠标，避免混合坐标轴被 libinput 误判。
- 绝对坐标按桌面区域归一化；重新进入时强制刷新位置，避免内核忽略相同坐标。
- 滚轮保留 1/120 格精度，并同时提供传统滚轮事件；水平滚动转换为 evdev 方向。
- 切回本机、断线清理和退出时释放按键/鼠标按钮并清空滚轮余量。
- 核心崩溃或退出后，设备句柄关闭，内核销毁虚拟设备。

## 验证

```sh
QT_QPA_PLATFORM=minimal ctest --test-dir build/src/unittests --output-on-failure
```

`UinputDeviceTests` 不需要管理员权限，使用管道验证按键释放、非法键码、坐标边界、
相对移动设备选择及滚轮余量。真实设备识别、Polkit 授权、锁屏和跨机输入仍需实机测试：

1. 关闭该设置，确认原有网络/蓝牙控制仍正常。
2. 开启设置，在未锁屏时重启接收端并授权，确认普通桌面的键盘、鼠标、滚轮正常。
3. 锁定 Ubuntu，用发送端按键唤出密码框，手动输入密码并按 Enter，确认 Ubuntu 正常验证后解锁。
4. 分别验证网络、蓝牙，以及切换连接或断线时不会留下按住的修饰键/鼠标按钮。
5. 检查熄屏后输入能否亮屏；此项不等于整机睡眠唤醒。

本功能以“已登录、系统仍运行、接收进程仍运行”的锁屏为目标。
不包含整机挂起唤醒、注销后的登录界面或磁盘加密开机密码。
软件转发按键，由 Ubuntu 进行密码校验，不保存密码，也不跳过系统认证。

## 本次验证记录（2026-10-24）

- Ubuntu `/data/deskflow` 完整构建与 43 项 CTest 测试通过；随后输入设备测试增加了重新进入时的坐标刷新用例。
- 实机 udev/libinput 将三个设备分别识别为键盘、绝对鼠标和相对鼠标。
- 通过独立测试程序验证了 `pkexec` → 辅助程序 → `SCM_RIGHTS` 设备句柄传递与销毁流程。
- Windows GUI 和核心编译、链接成功，使用独立测试文件名避免覆盖运行中的程序；设置、设置界面和输入释放测试通过。
- Windows 完整测试还遇到 ScreenLayoutTests 链接失败及 ConnectionManagerTests 运行失败，尚未定位；不将其计为全部通过。
- macOS 已检查条件编译隔离，没有可用的 macOS 构建环境，因此未进行实际编译。
- 本机桌面管理员授权成功，普通用户核心取得虚拟设备句柄，辅助程序随即退出。
- 蓝牙模式实测通过：锁定 Ubuntu 后，用户仅用共享键鼠手动输入密码并解锁；服务器确认 `LockedHint=yes` → `no`。
- 锁屏时 Portal 创建会话失败，但 uinput 与蓝牙传输持续运行，没有导致核心退出。
- 网络模式的锁屏输入及熄屏亮屏仍未单独实测；不包含整机挂起唤醒。
