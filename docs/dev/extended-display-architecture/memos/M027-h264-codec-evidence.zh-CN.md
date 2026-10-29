# M027：从实际 AVC 头核查 H.264 Profile

2026-10-29。对应架构第 14、17、24、26 节，是 M023 开发引擎的格式证据修正；不新增生产可用声明。

## 触发与证据

独立资源线程的 Mac 桌面复测曾出现笼统 RTP mapping rejected。增加诊断后，确认接收端先失败于 `EncodedProfileMismatch: h264-profile/baseline`，关联被拒只是后果，**没有发现 RTP timestamp 冲突的证据**。

GStreamer 1.28.2 的 [h264parse 源码](https://raw.githubusercontent.com/GStreamer/gstreamer/1.28.2/subprojects/gst-plugins-bad/gst/videoparsers/gsth264parse.c) 包含向下游发布兼容 profile 标签的逻辑；因此不能仅以 caps 标签作为实际码流约束证据。旧失败轮次未保存 AVC 头，不能追溯断言当时一定是合格码流，也不能用后续通过抹掉该缺口。

## 实现、不变量与边界

新增内部 H264CodecEvidence，编码和接收解析器均使用它：要求单一 fixed video/x-h264、AVC/AU，取得真实 encoder/parser 产生的 codec_data；7–65536 字节范围内调用公开 `gst_codec_utils_h264_get_profile_flags_level` 和 `gst_codec_utils_h264_get_profile/get_level`。只有实际头为 Constrained Baseline 且 level 可识别才设置 profile 已验证。无 codec_data 为 Pending；格式、版本、长度或实际 profile 不符为 Rejected；不把标签 baseline 自动当成功。

API 依据为 [GStreamer codec-utils 1.28.2](https://raw.githubusercontent.com/GStreamer/gstreamer/1.28.2/subprojects/gst-plugins-base/gst-libs/gst/pbutils/codec-utils.c)。完整 SPS/码流解析仍交给成熟 h264parse；本函数只是核查其输出的 AVC profile 头，不自写视频解析器，也不把七字节测试夹具说成完整合法视频。

发送 video-caps 固定 AVC/AU，接收在 h264parse 与 avdec_h264 之间约束 AVC/AU，使检查有明确的 codec_data 证据。没有检查通过，不允许发送 RTP/发布解码帧。统计新增 compatibleProfileLabels：仅当实际头合格且标签不同才计数，用于区别元数据标签和真实头。当前最终成功轮次该计数为 0，故不能据此声称已复原旧轮次原因。

显式 SDK 目标增加 gstreamer-pbutils-1.0。初始化除 GStreamer/libnice 外，再用 dladdr 核查实际 codec-utils 所在模块位于显式私有运行时；没有扫描/加载任意新插件，没有放宽录屏来源或媒体授权。

## 测试

原九项真实引擎政策夹具保留，新加八项 header 证据检查：实际 CB 头配兼容 baseline 标签仍可识别；宣称 CB 但实际 baseline/high、错误版本、零 level、短头、超限头均拒绝；缺头只 Pending。合计每端 17 项。它们验证元数据边界，不替代真实码流。

最终两端真实媒体与原始源码/二进制摘要见 [M027-native-results.json](M027-native-results.json)：Mac 政策 17 项通过，8 秒窗口读取/色块 213/213，GCC 50、encoder 51；Ubuntu 政策 17 项通过，读取/色块 222/222，GCC 26、encoder 27。验证包含实际解码色块、GCC→编码通知、持有接收租约阻止停止、释放后排空及预算归零。Mac 资源线程桌面样机结果归 M026，不能把同进程样机当隔离媒体 Worker 或双机认证。

## 影响、限制与提交

- NativeWebRtcStatistics 追加诊断字段，使用者须重新编译；Worker IPC/公共网络控制协议无改变。
- 开发管线现在固定 AVC/AU；尚未准入 byte-stream 分支，没有自动回退。SDK 内部内存、恶意 AU、完整硬期限、长期和弱网仍未认证。
- Windows 未编译实测。CPU 软件路径仍为显式开发目标，产品入口未切换。
- 代码、头部政策测试、两端媒体结果和本备忘录同阶段提交，通过 git log --follow -- 本文件定位。
