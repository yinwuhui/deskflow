# E001：原生 WebRTC 软件管线与反馈实验

2026-10-28；对应设计 D01/D09 的部分工程验证，**不是后端/方向认证完成记录**。

## 可复现实验

工具为 `tools/display-infrastructure-lab/gstreamer_loopback.c` 和 `run_gstreamer_loopback.py`。源码由本项目编写，通过 GStreamer 的公开接口交互，没有实现私有视频网络协议或读取 Rust 私有 ABI。

- 发送：1920×1080、I420、30fps、BT.709 synthetic snow → GStreamer 管理的 x264/H.264 Constrained Baseline → 原生 WebRTC。
- 接收：原生 WebRTC → H.264 RTP depayload/parse → avdec_h264 → BGRA。只将原始像素 sink 的输出计为解码帧。
- 通过公开 signaller 的信号拦截 start/stop/send SDP/send ICE/end session，在同一实验进程内交换标准信令；实际视频仍通过 ICE/DTLS-SRTP/RTP/RTCP。没有应用设备授权，仅限本实验。
- 显式关闭默认 STUN/TURN、控制/导航数据通道以及存在该功能时的 UPnP；不依赖外部信令/中继服务。
- 通过实际 encoder 的 caps 事件核对 Constrained Baseline，观察真实 rtpgccbwe estimated-bitrate 通知和实际 x264 bitrate 通知，不用组件名称代替反馈证据。

公开组件接口：[webrtcsink/signaller](https://gstreamer.freedesktop.org/documentation/rswebrtc/webrtcsink.html)、[webrtcbin](https://gstreamer.freedesktop.org/documentation/webrtc/)。已审阅上游 0.15.1 的 [signaller 接口](https://github.com/GStreamer/gst-plugins-rs/blob/0.15.1/net/webrtc/src/signaller/iface.rs) 与 [发送实现](https://github.com/GStreamer/gst-plugins-rs/blob/0.15.1/net/webrtc/src/webrtcsink/imp.rs)。实验依赖具体版本的公开信号语义，需要随 SDK 升级重验，不直接触碰 Rust vtable。

## 两端结果

两份结果对应相同源码 SHA-256 `b2aea44b370c66976f8747dcf73e8062ff8063c6c66512977c7c5997550d89cb`。主开发 Mac；Ubuntu 在用户私有依赖目录解包开发包并构建 rs 插件，未修改系统安装。

| 证据 | Mac arm64 | Ubuntu x86_64 |
| --- | --- | --- |
| GStreamer | 1.28.2 官方 SDK | 1.28.2 Ubuntu 库 + 自建 rs 0.15.1 |
| 解码输出 | 1920×1080 BGRA、30/1 caps | 相同 |
| 实际编码 profile | Constrained Baseline | 相同 |
| 解码帧计数 | 246 | 219 |
| 流错误 | 0 | 0 |
| GCC 估计通知 | 53 次 | 21 次 |
| GCC 估计范围 | 2,038,696–3,839,956 bit/s | 2,033,368–3,541,875 bit/s |
| 编码器 bitrate 通知 | 54 次 | 22 次 |
| 编码器目标范围 | 2000–3329 kbit/s | 2000–3138 kbit/s |

GLib 秒级测试截止会对齐系统时刻，窗口长度不同；帧计数不能直接用作跨平台性能比较。caps 中的 30fps 是协商格式，本实验没有完成长时延迟/丢帧/资源预算认证。编码目标与估计不同包括协议/FEC 开销分配；本实验只证明反馈能实际作用到编码器。

原始结构化摘要：[Mac](results/E001-macos-loopback.json)、[Ubuntu](results/E001-ubuntu-loopback.json)。

## 失败发现与修正（保留审计）

1. 首次把 fakesink 接收缓冲数量称为“解码帧”不够严谨。后续明确加入 RTP depayload、parse、软件解码和 BGRA caps 约束，重新测量；此前 236/2598 等缓冲数量不纳入解码证据。
2. 当前 SDK 的 webrtcsink 对 VideoToolbox encoder 不支持完整码率处理；直接强制 Constrained Baseline 时还观察到输出 baseline 不匹配。没有把该硬件路径发布为可用。
3. 当前 Mac/SDK 组合的自动 VideoToolbox 解码路径出现逐帧错误，随后使用明确的 avdec_h264 软件路径获得有效 BGRA 输出。硬件编码/解码均需独立适配和实机认证。
4. Ubuntu 首次私有 SDK 缺少 libnice 的 gupnp-igd 运行依赖，无法组成链路；补齐依赖后通过。产品准入必须检查完整管线和依赖，不能只见到 webrtcsink 工厂就报告 available。
5. /tmp 中旧构建工具的部分资源文件消失，导致 CMake 找不到 Modules。已在持久依赖目录恢复 CMake 4.4.4、Ninja 1.13.2、clang-format 20.1.0，并重新配置原构建目录；后续复现需使用完整工具链。

## 当前决策与未完成条件

采用 GStreamer 原生 WebRTC 软件管线继续实现，暂为开发候选；暂不准入 SDK 默认的 Mac 硬件路径。该结论没有决定全部方向默认协议，RDP 仍在独立适配范围。

尚需：认证控制面与类型化 Worker IPC、媒体信令校验及绑定、平台帧源/真实副屏、可见呈现证据、有限队列/真实分配预算、断线/撤销/崩溃清理、Mac↔Ubuntu 双机、长时与故障实验、插件白名单/打包/SBOM、Windows 编译及驱动交付。不得把这份 loopback 结果改写成完整拓展屏已验收。

本实验工具与记录同一提交，使用 `git log --follow -- <本文件>` 定位。业务模块仍逐项通过 M 编号备忘录追踪。
