# M011：Mac 原生输出 helper 与持久化配置屏障

2026-10-28。对应架构第 4、9、11、12、22、26 节，尤其 INV-05/INV-11。

## 模块边界

新增 `src/lib/display/platform/macos/MacVirtualOutput.{h,mm}`、`MacPlatformHelper.cpp`、`supervision/PlatformWorkerContracts.{h,cpp}`；构建 `busymouse-display-platform-helper`。提供真正当前交互桌面的附加输出、唯一强引用持有、受认证本地命令、正常/失联释放和原生枚举验证。没有实现采集、网络媒体、接收呈现、持久恢复提供者或产品装配，不能把本模块标为完整拓展屏已完成。

## 接口与关键不变量

- 资源 helper 使用独立 purpose、`display-platform/1` 与 `display-platform-bootstrap/1`；媒体仍为版本 2。purpose 参与认证上下文；资源 helper 不需要伪造远端身份/控制连接，拒绝媒体命令/帧/信令，媒体 worker 也不能报告平台资源归属。
- 完整链为 `PrepareOutput → Ready → Start → OutputObserved → ConfigureOutput → OutputReady`。绑定 owner/session/attempt/generation/epoch/worker/build/profile/plan/local identity。仅单个资源、固定 1920×1080 SDR 30Hz，10 秒绝对准备期限；禁止未经配置创建或重复创建。
- `ResourceExecutor` 新增 provider `configure()` 屏障：原生创建返回后先耐久写入 Observed，再允许配置，再独立 inspect 并写 Active。这是 M008 SPI 的补全；不能把先前模拟平台测试当作已覆盖真实布局顺序。取消、日志失败、配置失败均进入补偿。
- 原生对象只由 helper 主线程的 CGVirtualDisplay 强引用持有，移除只能释放自己持有的对象。厂商/型号/确定性 serial 只用于保守冲突/缺失线索，不能授权按 ID/名称删除其他屏幕。UUID、系统 ID 和强引用一起限定当前对象。
- 创建核对当前 console、用户图形会话、boot 实例、可证明未锁屏、创建前完整拓扑摘要；未知锁状态拒绝。模式初始化属于创建；对布局的操作需等 agent 已持久记录的配置指令。布局只设置自身输出 origin，不恢复整张历史拓扑，不调用镜像设置。
- 配置后重新枚举，核对自身 UUID/ID/serial、独立非镜像、物理及逻辑 1920×1080 和预期坐标。250ms 健康检查发现锁屏/失去交互会话/拓扑失效则停止。
- Qt IPC 与租约运行于独立 I/O 线程，主线程 WindowServer 操作阻塞不阻止 EOF/租约失效触发进程退出。原生释放和系统缺失验证仍分别进行；正常本地释放验证限 3 秒，监督器停止期限 5 秒。进程退出本身不证明输出已消失。
- Stop 后晚到的 OutputObserved 仍交付原 owner 清理；晚到 OutputReady 被认证消费后抑制，不能重新准备或配置。
- 修复监督器退出竞态：QProcess finished 可能先于 socket 可读通知；先零等待轮询内核可读、读取受限缓冲中的最终认证消息，再关闭 socket。新增即时断连真实子进程回归，Mac 另做 12 次有限重复均通过。仅排空 Qt 缓冲不足以解决该竞态，最初新增测试确实失败后才补齐。

复用已有 `tools/virtual-display-prototype/macos/third_party/DeskPad/CGVirtualDisplayPrivate.h` 的 MIT 声明及其 LICENSE/SOURCE 归属。CGVirtualDisplay 属私有接口，当前机器通过不构成整个 macOS/芯片矩阵或分发认证。

## 实际验证与证据

Mac arm64 / Qt 6.8.3：实际创建附加输出、独立枚举确认 Active；随后分别正常停止和监督器强制终止自己启动的 helper。最终连续四次复测（各模式两次）均通过，系统枚举均恢复为创建前 1 个输出，原输出 UUID 不再存在，资源依赖按输出先、helper 后关闭，owner 闭合。正常停止收到 Quiesced，强制停止没有该消息，依靠独立系统枚举证明输出缺失。结构化结果和源 SHA256 见 [真实资源结果](M011-mac-native-results.json)。这不是合成帧、真实桌面传输或跨机呈现测试。

手动工具 `MacPlatformHelperSmoke <helper> graceful|terminate` 明确不注册 CTest。工具使用临时账本验证持久转换顺序；失败后的临时测试账本不作为产品恢复机制，正式提供者仍待实现。失败路径监督器销毁只终止其自身进程；不会猜测删除系统输出。

新增单元/子进程测试 4 项：平台命令/源归属与取消竞态、purpose 隔离、配置失败/配置期间撤权补偿、最后一条 IPC 消息排空。既有日志故障测试另断言未持久 Observed 时不能 configure。

最终相关回归：Mac **113/113，17.19s**；Ubuntu x86_64 / Qt 6.10.2 **113/113，13.33s**。含累计 75 项基础设施及 38 项既有服务测试。Ubuntu 仅验证公共代码和真实通用子进程，没有运行 Mac 原生输出。Windows 尚未编译/实测。

```sh
cmake --build <build> --target DisplayWorkerProtocolTests DisplayWorkerProcessTests DisplayResourceExecutorTests -j 4
ctest --test-dir <build>/src/unittests -R '^Display\.(Infrastructure\.|Service\.|UsageService\.)' --output-on-failure
MacPlatformHelperSmoke <absolute-helper-path> graceful
MacPlatformHelperSmoke <absolute-helper-path> terminate
```

Mac 构建/CTest 继续在正常宿主权限下运行，未绕过 Qt CPU/SIMD 检查。Ubuntu 同步前逐个 SHA256 比较本阶段修改文件，均与 Mac 阶段起点一致；只同步 17 个明确文件及最后一次监督器补丁，保留远端原有修改，不覆盖 Git 历史。

最终有界排空修正后，两端另运行受影响的 19 项 Worker 测试均通过。

## 待接入、限制与审查

- 暂停前一次最终真实输出复测返回失败；当时外层脚本 capture_output 后直接 check=True，未保存 stderr，因此不能确定属于平台创建、环境状态还是清理消息问题。恢复后一次正常停止和连续四次两模式复测通过，但不将这些通过结果当成已解释该次失败。后续真实验收需持久保留每次退出码与 stderr，继续追查；本模块不发布为已认证能力。

- 需要正式 NativeProcessRegistry/原生进程出生身份、持久恢复 PlatformResourceProvider、核心 effect 装配。当前手动工具的 live QProcess 所有权不是重启后可复用的 PID 删除授权。
- 需要捕获同一 source ResourceId、拓扑代次与实际 ScreenCaptureKit 权限/帧证据。当前 topologyGeneration=1 只表示一次性 helper 的首次对象，尚未建立产品拓扑版本管理。
- helper 的应用包布局、签名和完整依赖清单还未接入；当前从明确绝对路径+摘要注册启动，未发布 available 能力。
- 私有接口选择器与单机真实结果不能代替 OS 矩阵、睡眠/热插拔/锁屏、helper 父进程整体崩溃及长时验收。

生产入口仍使用原媒体运行时。回滚本阶段不会切换生产视频协议。通过 `git log --follow -- <本文件>` 定位与代码/测试同阶段提交。
