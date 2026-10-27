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
