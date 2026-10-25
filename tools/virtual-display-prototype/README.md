# 虚拟扩展屏平台实验与公共基础

本目录是设计文档阶段 A 和公共协议基础，不是已交付的跨设备副屏应用。
程序不会打开视频网络监听，也没有把不加密的画面流接入现有键鼠协议。

## 当前能力

- Linux：只读能力诊断；显式运行 GNOME 实验时，创建临时虚拟屏，确认独立逻辑输出、采集正确尺寸的帧并清理；可选通过有界帧队列生成软件 H.264 样本。
- macOS：只读私有接口与权限诊断；显式创建实验副屏，使用 ScreenCaptureKit 仅采集该副屏并清理；可选使用 VideoToolbox 生成 H.264 样本并报告实际硬件加速状态。
- 公共 C++20 库：DFVD/1 的 40 字节头、有界分帧、通道和会话/epoch 隔离、会话取消恢复代次及输入租约。
- 13 项自动化测试；Linux 和 Mac 的重复运行/异常退出验证工具。

尚未实现 TLS 身份与三通道绑定、JSON 消息语义、产品媒体管线、跨设备实时视频传输、输入注入和产品 GUI。
FrameReader 只是信封解析器，不负责认证。InputLease 的释放列表必须由未来的平台适配器实际执行，协调器也必须定时调用 expire。
这些基础能力不能单独开启远程控制。

## Ubuntu：能力与真实虚拟屏实验

在桌面用户的会话中运行，使用系统 Python。需要 python3-gi、python3-dbus、GStreamer Python introspection、PipeWire source 插件及 GNOME Mutter。
只读报告不要求导入 GStreamer 或 dbus-python；实验才需要这些模块。

```sh
python3 tools/virtual-display-prototype/virtual_display_probe.py
python3 tools/virtual-display-prototype/virtual_display_probe.py --exercise-gnome --duration 3
python3 tools/virtual-display-prototype/validate-linux-probe.py
```

SSH 的 XDG_SESSION_TYPE 可能是 tty；工具通过当前用户总线访问实际桌面。
只有明确确认总线对应预期用户桌面后，才运行 `--exercise-gnome`。
实验会临时改变真实桌面拓扑，窗口可能由系统重新排列。
不在独立 X server 上制造“通过”，不采集主屏代替副屏。

GNOME 后端使用私有 Mutter D-Bus API，与 Portal 虚拟源能力报告分开。
在 GNOME 50.1 的实测中，源是 `framerate=0/1`，通过 `max-framerate` 限定上界；固定 `30/1` 会协商失败。
静止桌面按损伤更新，报告中的缓冲到达率不是持续运动帧率。

## Mac：独立探测程序

需要 Xcode Command Line Tools 和 macOS 14+ SDK；实际认证范围仅见实测报告。

```sh
sh tools/virtual-display-prototype/build-macos-probe.sh
/tmp/deskflow-macos-display-probe/VirtualDisplayProbe.app/Contents/MacOS/macos-display-probe
/tmp/deskflow-macos-display-probe/VirtualDisplayProbe.app/Contents/MacOS/macos-display-probe --create-only
/tmp/deskflow-macos-display-probe/VirtualDisplayProbe.app/Contents/MacOS/macos-display-probe --exercise
```

`--exercise` 需要系统屏幕录制权限；没有权限时返回 PermissionRequired，不创建显示器。
需要时使用 `--request-capture-permission` 请求系统授权，然后按系统提示完成并重新运行。
无需为了创建副屏申请辅助功能权限。

私有接口声明来自 DeskPad 的固定提交；原始头文件、MIT 许可证和来源保存在 `macos/third_party/DeskPad/`。
运行时检查类和 selector 存在性不能代替对新 macOS 版本的实测。
此程序不面向 App Store 分发，也不安装驱动或系统扩展。

## 公共基础测试

独立构建不需要 Qt 或 FFmpeg：

```sh
cmake -S tools/virtual-display-prototype -B /tmp/deskflow-display-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/deskflow-display-build --parallel 4
ctest --test-dir /tmp/deskflow-display-build --output-on-failure
```

主工程使用 `-DBUILD_VIRTUAL_DISPLAY=ON` 启用库与测试，默认 OFF。
当前选项不增加用户界面或网络监听器，关闭时不构建这些目标。

分帧入口每次接收不超过 64 KiB，单条控制/输入/视频消息分别限制为 64 KiB、4 KiB、4 MiB。
解析错误使连接永久失败；上层必须关闭连接，不尝试扫描 magic 恢复。
SessionState 和 InputLease 由单一协调线程拥有，不能从多个线程并发修改。

## 离线 H.264 编码实验

Linux 需要可运行的 FFmpeg 和 libx264 编码器。仅诊断列出编码器不表示硬编码已经可用。
Mac 使用原生 VideoToolbox，回调把 AVCC access unit 转为 Annex B，保留参数集供独立解码器验证。

```sh
python3 tools/virtual-display-prototype/virtual_display_probe.py --exercise-gnome --h264-output /tmp/deskflow-linux-new.h264
/tmp/deskflow-macos-display-probe/VirtualDisplayProbe.app/Contents/MacOS/macos-display-probe --exercise --h264-output /tmp/deskflow-mac-new.h264
ffprobe -v error -count_frames -show_entries stream=codec_name,width,height,nb_read_frames -of json /tmp/deskflow-linux-new.h264
ffmpeg -v error -xerror -err_detect explode -i /tmp/deskflow-linux-new.h264 -f null -
```

只执行当前平台对应的编码命令；输出文件必须不存在。Linux 为软件基线，Mac 是否硬编码以 hardwareEncoder 字段为准。
这类样本使用实验性固定帧时间戳，不代表实时 30 fps，不能据此测端到端延迟或判定色彩质量。
样本通过 SSH 复制到另一平台后可验证交叉解码，SSH 文件传输不计为产品实时网络协议。

参考：[设计文档](../../docs/dev/cross-platform-virtual-display-design.zh-CN.md)、[阶段 A 实测](../../docs/dev/virtual-display-stage-a-validation.zh-CN.md)。


## 实时 TLS 会话（实验）

在 Ubuntu 开发与测试；Mac 仅运行已授权的原生验证和接收程序。
需要支持 TLS 1.3 的 Python、OpenSSL CLI、FFmpeg/libx264；可视接收还需 ffplay。
默认不会监听局域网。双方分别生成身份，并通过可信通道仅交换 cert.pem：

```sh
python3 tools/virtual-display-prototype/display_session.py init-identity --directory /tmp/display-identity
```

私钥 key.pem 留在生成设备上。确认对端证书来源后，在发送端运行：

```sh
python3 tools/virtual-display-prototype/display_session.py serve \
  --identity /tmp/display-identity --peer-cert /tmp/receiver-cert.pem \
  --source gnome --bind SENDER_LAN_IP --duration 10
```

Mac 发送端使用 `--source macos --macos-probe /绝对路径/macos-display-probe`。
接收端运行（以各自身份和发送端公钥替换路径）：

```sh
python3 tools/virtual-display-prototype/display_session.py receive \
  --identity /tmp/display-identity --peer-cert /tmp/sender-cert.pem --host SENDER_LAN_IP
```

加入 `--view` 打开 ffplay 窗口；默认严格解码至空输出并报告真实解码帧数。
窗口模式没有呈现确认，不能把进程成功当作可见画面证明。两种模式都禁用远程输入。
默认 TCP 24810，三个独立 mTLS 连接复用监听端口；只在明确需要时指定局域网 bind 地址。
实验每次只允许一个可信会话，最长 3600 秒，不提供自动重连。

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tools/virtual-display-prototype -p test_display_transport.py -v
PYTHONDONTWRITEBYTECODE=1 python3 tools/virtual-display-prototype/validate_live_session.py
```

后一命令明确使用合成 test 源，验证实时传输和中断，不能作为虚拟屏证明。
独立或主工程 CMake 可加 `-DDISPLAY_LIVE_TESTS=ON` 把该回环测试加入 CTest。
真实副屏、身份引导、流控与尚未完成的产品工作详见[阶段 B 报告](../../docs/dev/virtual-display-stage-b-validation.zh-CN.md)。


真实 GNOME 发送进程崩溃验证（会临时创建副屏并 SIGKILL 本次发送进程）：

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/virtual-display-prototype/validate_gnome_host_death.py
```

该测试独立观察 Mutter 显示拓扑与子进程退出，不把接收端报错本身当作清理证明。
helper 的父进程监测由会话程序自动传入；手动运行探测器通常无需设置 `--parent-pid`。
Mac 探测器在锁屏、非活动控制台或无活动显示器时返回 SessionUnavailable，等待解锁后重新验证。


### Mac 原生实验接收窗口

```sh
sh tools/virtual-display-prototype/build-macos-receiver.sh
python3 tools/virtual-display-prototype/display_session.py receive \
  --identity /tmp/display-identity --peer-cert /tmp/sender-cert.pem --host SENDER_LAN_IP \
  --macos-viewer /tmp/deskflow-macos-display-receiver/DisplayReceiverProbe.app/Contents/MacOS/macos-display-receiver
```

该选项隐含窗口模式；本地 FFmpeg 把 H.264 解码为固定 1920×1080 BGRA，AppKit/CoreAnimation 显示。
网络仍为 H.264/TLS，像素管道有界。nativeSubmittedFrames 表示提交数量，presentationVerified 仍为 false。
窗口不转发键鼠输入。当前 Mac 原生编码实验使用 4 Mbit/s 目标及 500000 字节/秒硬上限，
最多两个在途编码请求，未编码原始帧可丢弃；不丢弃已编码的依赖帧。

```sh
python3 tools/virtual-display-prototype/validate_macos_host_death.py \
  --macos-probe /tmp/deskflow-macos-display-probe/VirtualDisplayProbe.app/Contents/MacOS/macos-display-probe
```

以上实机测试要求解锁且处于活动控制台会话。断线集成测试现在等待真实首帧后注入 SIGTERM。
