# 拓展屏基础设施实验

这些工具只验证单项媒体工程条件，不接入产品、不创建虚拟显示器，也不证明授权、原生呈现或完整拓展屏链路成立。

## GStreamer WebRTC 软件管线

```sh
python3 tools/display-infrastructure-lab/run_gstreamer_loopback.py \
  --sdk-prefix <GStreamer-SDK-prefix> \
  --output <私有实验结果目录>
```

SDK prefix 必须包含 `lib/pkgconfig` 或 `lib/<triplet>/pkgconfig`、相应动态库和插件。Mac 官方 Framework 对应 `GStreamer.framework/Versions/1.0`；本次 Ubuntu 私有 SDK 对应解包树的 `usr/`。需 C 编译器、pkg-config 和 Python 3。实验使用 GStreamer 1.28.2 的公开但标记 unstable 的 WebRTC C API，依赖固定版本后需重新验证。

发送为 `videotestsrc → x264enc（由 webrtcsink 管理）→ H.264 Constrained Baseline → WebRTC`；接收明确 depayload、parse、avdec_h264 解码并输出 BGRA，避免把 RTP 缓冲数错当解码帧。发送输入锁定 1920×1080、30/1、I420、BT.709，运行约 8 秒。测试只用软件编解码，不自动宣称 VideoToolbox 已支持。

信令只在同进程内经公开 GObject 信号转交，视频仍走原生 ICE/DTLS-SRTP/RTP/RTCP。显式关闭 STUN/TURN 默认服务、数据输入通道及可用时的 UPnP；不连接示例 WebSocket 信令服务器。编解码器、webrtcbin/libnice、SRTP、RTP 和 GCC 组件须全部存在，缺失即失败。

成功至少要求真实 BGRA 解码输出、编码器实际输出 Constrained Baseline、实际选择 x264、GCC 估计与编码器 bitrate 通知都出现。输出 JSON 保存源码 SHA-256、SDK 版本、实际 caps、计数和失败状态。此实验没有包办发布依赖清单、插件白名单、帧内存预算、进程 IPC 和既有设备授权，不能发布为 available。

实验记录：[E001](../../docs/dev/extended-display-architecture/experiments/E001-native-webrtc.zh-CN.md)。

## 隔离发送与 Mac→Ubuntu 跨机诊断（M034/M035）

显式私有 SDK 构建新增 `busymouse-display-media-sender`、`busymouse-display-media-receiver` 和 `NativeRemoteReceiverAgent`。默认产品入口未切换，工具不安装为生产后端。

Mac（先构建 helper、两个 Worker、NativeMacWebRtcLoopback）：

```
NativeMacWebRtcLoopback <absolute-helper> <private-runtime> --sender-worker=<absolute-sender>
NativeMacWebRtcLoopback <absolute-helper> <private-runtime> --sender-worker=<absolute-sender> --ubuntu-receiver
NativeMacWebRtcLoopback <absolute-helper> <private-runtime> --sender-worker=<absolute-sender> --ubuntu-disconnect
```

后两项固定连接已配置的 `ywh@10.20.1.71` 和 `/home/ywh/code/github/deskflow/build/bin/NativeRemoteReceiverAgent`，Ubuntu 需先编译 IPC v5、接收 Worker 和 Agent，并有活动 `/run/user/1000/wayland-0` 图形会话。保持其他显示器布局调整暂停；工具只创建/回收自己的 Mac 副屏，不调整其他输出。

媒体走真实 LAN WebRTC，控制使用 SSH 宿主认证和显式诊断身份，不能替代产品设备证书、用户同意、输入隔离或 PlanCommit 验收。Ubuntu 诊断 profile 仅在实际解码像素通过绿色色块检查并完成 native paint 后报告帧；Mac 的远端模式不回传原始像素，JSON 的本机 decodedFrames/markerFrames 因而为0。跨机 nativePaintFrames 是远端带色块校验的绘制数。

`--ubuntu-disconnect` 在至少30帧后关闭控制输入，检查双方停止与Mac输出独立清理。正常/断线后的窗口排空、进程退出、系统副屏不存在和账本关闭是分开的证据。任何失败保留临时账本，不删除现场。

已知 ledger 可用 `--recover-ledger=<absolute-ledger>` 保守核对：已有活动owner时要求原helper出生身份已不存在、本owner输出hint不存在、用户会话/boot一致，再由现有provider验证关闭。不能凭此终止持久化PID或删除发现的任意显示器。无活动owner时仅报告幂等账本状态，不重新认证系统资源。
