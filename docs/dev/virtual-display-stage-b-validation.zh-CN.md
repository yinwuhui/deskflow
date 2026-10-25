# 虚拟副屏阶段 B：安全实时传输实验

日期：2026-10-25。范围：当前 Ubuntu GNOME 与 Mac 两台实机；Windows 尚无设备。
本阶段在阶段 A 的真实虚拟显示器、目标屏幕采集和 H.264 编码之上增加实时 TLS 会话。
这是原设计阶段 B 的先行实验，Windows MVP、性能和产品集成门槛尚未完成。
这些程序位于 `tools/virtual-display-prototype/`，属于可运行的实验适配层，尚未接入 Deskflow 产品界面。

## 实现范围

- `display_transport.py`：与 C++ DFVD/1 金向量一致的 40 字节大端帧头、严格有界解析、TLS 1.3 双向认证和证书固定。
- `display_session.py`：控制会话、视频与输入连接绑定、心跳、实时源进程管理、接收解码、结束序号确认与资源回收。
- `h264_access_units.py`：有界 Annex B access unit 分割及源进程输出封装。
- `macos/DisplayReceiverProbe.mm`：AppKit/CoreAnimation 原生实验窗口；本地 FFmpeg 解码成固定 1080p BGRA，经有界管道送入窗口。
- Linux helper 使用 Mutter 虚拟显示器与 PipeWire；Mac helper 使用私有 CGVirtualDisplay、ScreenCaptureKit 和 VideoToolbox。
- `test_display_transport.py` 与 `validate_live_session.py`：传输单元测试、真实 TLS 回环、连续解码、接收端中断验证。

主工程 `BUILD_VIRTUAL_DISPLAY` 默认关闭。启用后构建公共 C++ 库并注册可用的传输单元测试，
不会自动监听网络或启动显示器。Python CLI 需要显式运行；实时回环测试还需显式启用 `DISPLAY_LIVE_TESTS`。

## 会话与身份

当前没有自动发现、配对码或信任管理界面。实验使用每台设备独立生成的自签设备证书；
通过已可信的 SSH 通道交换公钥证书，`--peer-cert` 明确指定唯一被信任的对端。
私钥不传输、不进入仓库。生成目录权限 0700，私钥 0600，已有身份文件拒绝覆盖。
这只是实验引导流程，产品仍需设备撤销、轮换和持久信任管理。

TLS 上下限都固定为 1.3，必须验证对端证书，并比较证书 SHA-256 指纹。
控制连接获得非零随机 session ID 和 epoch；视频、输入各使用一个随机单次绑定令牌，
令牌绑定证书、会话、epoch 与通道，十秒失效，控制连接关闭后撤销。
每条连接独立检查单调序号和消息类型，解析失败后不能复用。

流程为 Hello → Capabilities → Start → Accept → 两个子连接 Bind/确认 → StreamConfig → Ready → 视频。
输入连接仅验证绑定与隔离；`inputEnabled=false`，没有键鼠注入或焦点控制。
结束时 Stop 带最终视频序号，接收端跨连接等齐该序号，关闭解码输入并排空，再回复 Stopped。
此确认是接收/解码结束确认，不是屏幕呈现确认。

## 视频与流控

视频为 H.264 Annex B access unit。每条 VIDEO 负载前九字节是八字节大端时间戳与一字节 IDR 标志。
当前时间戳为零，明确表示尚未接入有效采集时间；不能据此计算端到端延迟。
首个 access unit 必须有 SPS、PPS 与 IDR，IDR 标志须与 NAL 内容一致。
源进程到 Python 的本地管道使用四字节大端长度前缀，这与网络帧头是两层不同封装。

单条控制/输入/视频负载上限分别为 64 KiB、4 KiB、4 MiB，单次解析输入不超过 64 KiB。
JSON 限制深度、节点、字符串长度，拒绝重复键和非有限数字；每个 access unit 最多 256 个 NAL。
编码源队列和解码队列分别最多两个待处理 access unit，总在途数据分别不超过 4 MiB。
启动宽限为队列创建起两秒，不随进度无限延长；之后拥塞等待最多 150 ms。
编码后的依赖帧不丢弃，超限中止会话；Linux 可丢弃尚未编码的旧原始帧；Mac 最多两个在途编码请求，超出时丢弃尚未编码的原始帧。
网络整条消息发送使用绝对一秒期限。当前尚无码率自适应、请求关键帧恢复或自动重连。
真实采集 helper 通过 `--parent-pid` 监测拥有会话的父进程；父进程消失时退出采集并清理，
不依赖控制连接 finally 或实验时长结束。Linux 已做真实 SIGKILL 拓扑验证；Mac 也已完成解锁状态下的真实 SIGKILL 拓扑验证。

默认接收端用 FFmpeg 严格解码并报告帧数；`--view` 改用 ffplay 实验窗口。
窗口模式未接入呈现遥测，因此 decodedFrames 为 0、没有 PresentAck，不能据此认定画面已经显示。
`--macos-viewer` 选择新建的原生窗口，`nativeSubmittedFrames` 仅表示已将解码图像提交给显示层，
也不能等同于实际呈现。原生窗口保留一帧显示内容、最多一帧待主线程处理和一帧读取中的固定尺寸像素；
网络不传原始 BGRA。该路径仍依赖 FFmpeg 解码，尚非完整原生硬件解码后端。

## 实测记录

环境沿用[阶段 A 报告](virtual-display-stage-a-validation.zh-CN.md)。以下是单次样本，不是吞吐或持续帧率认证。

| 场景 | 结果 | 边界 |
| --- | --- | --- |
| Python 传输单元测试 | 22 项通过 | 包含真实 mTLS 1.3、错误客户端证书、一次性令牌、重放/通道隔离、JSON、NAL 和队列上限 |
| 合成源实时回环 | 120 帧发送、接收、解码一致，结束前开始解码 | 显式 test 源，只验证传输链路 |
| GNOME 真实副屏回环，4 秒 | 47 帧发送、接收、解码一致，结束前开始解码 | 正常结束与接收端 SIGTERM 均确认虚拟屏清理 |
| GNOME 发送进程 SIGKILL | 拓扑从 eDP-1 + Meta-0 恢复为 eDP-1，helper 退出 | 单次观测清理 705.7 ms；接收端检测断开 |
| Linux → Mac，LAN 直连，6 秒 | 64 帧发送、接收、解码一致，TLS 1.3 | 真实 GNOME 虚拟屏；清理确认 |
| Mac → Linux，新码率配置，6 秒 | 106 帧发送、接收、严格解码一致，结束前已解码 104 帧 | 硬编码 true；最大单帧发送约 254 ms；源队列高水位 179014 字节；清理确认 |
| Mac 本机回环，新码率配置，4 秒 | 31 帧发送、接收、解码一致 | 正常结束与首帧后接收端 SIGTERM 均确认清理 |
| Mac 发送进程 SIGKILL | 显示器列表从 [3,76] 恢复为 [3]，helper 退出 | 单次观测确认时间 1085.1 ms |
| Linux → Mac 原生窗口，45 秒 | 399 帧接收并提交给原生显示层，正常退出，源清理确认 | 界面工具连接应用超时，未取得截图或完成可见画面验收 |
| Mac → Linux，LAN 直连，4 秒 | 30 帧发送、接收、解码一致，TLS 1.3 | 真实 Mac 虚拟屏；硬编码 true；清理确认 |
| Linux → Mac，ffplay 模式，20 秒 | 172 帧接收，播放器正常退出，源清理确认 | Mac 锁屏使界面检查受阻，未作可见画面认证 |

早期窗口测试触发冷启动拥塞，并未取得清理报告；随后修复固定启动宽限，并将 GNOME 显示器释放前移到编码器等待之前。
这次失败不计为成功样本。
解锁后的 Mac → Linux 重测仍在关键帧突发时触发队列上限，随后修复 Mac 的 SIGPIPE 退出路径，
改为记录管道错误并执行清理；同时限制原始在途编码请求，并将原生实验编码目标改为 4 Mbit/s，
通过 VideoToolbox DataRateLimits 设置 500000 字节/秒硬上限。上表 106 帧样本使用这一新配置。
这是固定实验档位，不是自适应码率策略，也未完成文字质量验收。未扩大两帧/4 MiB 队列。
断线 fixture 改为等待首个真实 VIDEO 到达再注入 SIGTERM，避免固定 1.5 秒等待只测到启动取消。
保留严格队列上限，后续仍需运动画面和受限网络稳定性测试。
静止桌面的损伤更新、keepalive 和临时固定编码时序不能证明 1080p30 持续运动表现。

## 验证命令

在 Ubuntu 仓库执行：

```sh
cmake -S tools/virtual-display-prototype -B /tmp/deskflow-display-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DDISPLAY_LIVE_TESTS=ON
cmake --build /tmp/deskflow-display-build --parallel 4
ctest --test-dir /tmp/deskflow-display-build --output-on-failure
PYTHONDONTWRITEBYTECODE=1 python3 tools/virtual-display-prototype/validate_live_session.py --source gnome --duration 4
PYTHONDONTWRITEBYTECODE=1 python3 tools/virtual-display-prototype/validate_gnome_host_death.py
```

独立 CTest 含 13 个 C++ 用例、一组 22 项 Python 单元测试、一个合成源正常结束/中断集成测试。
真实 GNOME 测试会改变桌面拓扑，故不自动加入常规 CTest；Mac 实机测试也需显式执行。
需要 Python 的 OpenSSL 支持 TLS 1.3，以及 openssl、FFmpeg/libx264；窗口模式另需 ffplay。
Mac 系统 Python 若使用旧 LibreSSL 则不可用，应显式选择支持 TLS 1.3 的 Python。

本轮 Ubuntu 主工程构建成功，62 个 CTest 全部通过（远端基础提交 `2532816e1` 加本轮工作区改动）；
这不等于验证了本地较新 GUI 基础提交。最新 GNOME 实机回归正常接收/解码 46 帧，异常中断时已发送 5 帧，
两个场景都确认清理。Mac helper 重新编译通过 `-Wall -Wextra -Werror`，锁屏预检返回
`SessionUnavailable`、`created=false`，前后显示器列表一致；没有继续创建新显示器。
之前锁屏下的发送重测未采到帧且未确认当次清理，明确记为失败，不能替代解锁后的复测。
源管道 EOF 可能早于 helper 清理完成，现将 EOF 后退出等待从五秒延长至十五秒。
接收报告显式给出 `presentationVerified=false`，窗口正常退出另标记 `viewerExitedCleanly`。

## 后续产品工作

1. 将传输与平台后端接入 C++ 产品服务，提供会话、权限和可信设备界面。
2. 把当前实验原生窗口升级为产品媒体后端，接入原生硬解码、有效时间戳、呈现确认、色彩与尺寸协商。
3. 完成输入租约到系统注入的链路、焦点/断线释放、权限和多屏坐标映射。
4. 完成分辨率切换、关键帧恢复、重连、码率调节、受限网络与长时运行验证。
5. Windows IDD、捕获/编码/接收后端，以及三端实际系统版本矩阵验证。
6. 完成依赖子模块核验、许可证清单、打包分发与 macOS 私有接口兼容性策略。

上述功能尚未实现或认证，当前不能称为三端完整可用产品。


## 解锁后的 Mac 复现补充

```sh
sh tools/virtual-display-prototype/build-macos-receiver.sh
python3 tools/virtual-display-prototype/display_session.py receive \
  --identity /tmp/display-identity --peer-cert /tmp/sender-cert.pem --host SENDER_LAN_IP \
  --macos-viewer /tmp/deskflow-macos-display-receiver/DisplayReceiverProbe.app/Contents/MacOS/macos-display-receiver
python3 tools/virtual-display-prototype/validate_macos_host_death.py \
  --macos-probe /tmp/deskflow-macos-display-probe/VirtualDisplayProbe.app/Contents/MacOS/macos-display-probe
```

Mac 命令必须选择支持 TLS 1.3 的 Python。崩溃测试仅结束它自身创建的发送进程，
独立枚举显示器列表并检查对应采集子进程退出；不终止已有 Deskflow 或其他显示器进程。
