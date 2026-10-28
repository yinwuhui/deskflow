# M019：Mac 活跃资源绑定与原生 CPU 采集源

2026-10-28。对应架构第 9、11、14、19、22、26 节。完成独立采集模块及手动原生验证；不是产品媒体 Worker 或后端方向认证。

## 边界与实现

- 新增 MacCaptureSource.{h,mm}；MacResourceProvider::captureBinding 只为自己 live helper 持有、账本 Active、原平台引用一致的 virtualOutput 生成不可公开构造的本地绑定。再次通过隔离探测核对桌面、显示 ID、UUID、活动/非镜像状态和 1920×1080 像素及逻辑尺寸，附带拓扑摘要、session/resource/epoch/generation。
- 绑定共享 live 撤销标记及一次性 claim。helper 停止、退出或 provider 析构时失效；复制绑定不能启动第二条采集。它是本地资源证据，不是网络授权，也不跨进程序列化。
- 采集必须在主线程、QGuiApplication/native event loop 中启动；只检查既有 Screen Recording 权限，不主动请求或弹窗。准备上限十秒；start 返回准备已接受，不代表帧已产生。
- ScreenCaptureKit 按准确 SCDisplay ID 创建 filter，没有选择其他屏幕的分支；注册拓扑回调后重新核对 UUID/拓扑摘要。任意拓扑变化使交付失效，blank/suspended/原生流错误也停止接受样本。
- 配置 1920×1080、30fps、BGRA、BT.709、嵌入光标、无音频，原生 queueDepth=3；只接受 Complete、合法时间及像素格式/尺寸/stride。应用 CPU 拷贝预先向 FramePool 预约，最多三张、32 MiB，一张等待帧；只有 Published 帧可被新帧替代，不覆盖 Reading。CPU 缓冲的唯一所有权移交帧池后再 publish。
- stop 先撤销交付并 retire 帧池，再等待 native stop completion 或明确 didStopWithError。准备中取消不能被晚到回调重启。stopped 还要求所有帧租约排空；借用方必须先释放 owner 再 releaseRead。
- 原始捕获时间来自 SCK 样本 PTS，以纳秒标记；没有把回调到达时间伪装为采集时间。跨进程统一时钟映射与性能校准仍待后续接入。

## 探测监督修正

实际采集绑定验证暴露了既有 probe 同步等待暂停 helper 心跳的问题。probe 轮询现在处理所属资源线程的 Qt 事件，维持监督器 IPC/租约；ResourceExecutor 继续负责拒绝重入。captureBinding 在探测后重新检查 live/停止状态。同步系统启动和原生枚举仍不具备完整硬截止保证。

## 实际测试与故障审计

手动 MacResourceProviderSmoke 新增 capture、capture-cancel、capture-revoke，使用真实附加输出和真实 ScreenCaptureKit：

1. capture：实际 1920×1080 BGRA 帧的资源/session/时间绑定通过；重复使用绑定拒绝；持有 ReadFrame 时不能宣告 stopped，释放后停止确认、CPU bytes/surfaces 归零。
2. capture-cancel：准备后立即停止，泵晚到回调；没有发布帧，预算归零。
3. capture-revoke：真实帧借用期间关闭 owned helper；撤销后不再读取新帧，原生停止及借用排空后预算归零。
4. 上述三项及 probe-timeout、probe-invalid、normal、recover、intent、ambiguous 九项最终串行全部退出 0，原有输出均从 1 恢复为 1，outputAbsenceVerified/ownerClosed 为 true。完整 stdout/stderr、退出码与源码摘要在 [原生结果](M019-native-results.json)。

开发中失败没有删除：首次绑定失败发现心跳饥饿；QCoreApplication 宿主无法完成原生异步准备，现明确要求 native GUI 事件循环；采集缓冲最初没有按唯一所有权移交，已修正；测试 until 对消费型条件重复求值，已修正。一次停止崩溃由系统符号栈定位为 stopNative 的 shared_ptr 引用被异步 block 持有，改为按值持有；撤销场景最初未承认原生 didStop 通知，导致停止未确认，已修正。JSON 保留崩溃及撤销失败尝试和解释，最终通过不覆盖这些历史结果。

Mac 当前完整相关自动回归 116/116（17.07 秒）。原生测试不注册为常规 CTest，避免回归意外改变桌面。Ubuntu 公共条件构建及相关回归 116/116（18.20 秒），不验证 Mac 原生模块。同步前两个 CMake 文件摘要与阶段起点一致，仅同步这两文件；远端 Mac provider/smoke 仍为 M013 版本，未覆盖。Windows 未编译实测。

```sh
cmake --build <build> --target MacResourceProviderSmoke busymouse-display-platform-helper -j 4
MacResourceProviderSmoke <absolute-helper> capture
MacResourceProviderSmoke <absolute-helper> capture-cancel
MacResourceProviderSmoke <absolute-helper> capture-revoke
ctest --test-dir <build>/src/unittests -R '^Display\.(Infrastructure\.|Service\.|UsageService\.)' --output-on-failure
```

## 未接入与限制

- 仍是本地手动测试进程内的 provider→capture 绑定；没有把本模块装入隔离媒体 Worker，没有替换 ProductDisplayRuntime，没有网络或编码、可见接收呈现、跨机认证。
- 真实帧不是解码帧或呈现证据。没有媒体预算/拥塞/30fps 吞吐或长时认证；SCK/WindowServer 的内部表面和隐藏分配不属于已证明的 CPU 预算。不得把 queueDepth 配置当作总原生内存实测。
- 原生 API 阻塞仍需外部 Worker 监督；停止错误/未回调不得转为成功。析构可以取消交付，但不能代替停止和排空确认。所有 start/poll/stop/析构必须由主线程执行，消费者释放租约可使用线程安全帧池接口。
- 锁屏、睡眠、会话切换、热插拔/拓扑真实故障、进程硬崩溃、TCC/签名包责任链、色彩校准、统一拓扑代次和时钟映射仍需独立验证。当前一次性输出 generation=1，拓扑变化即失败，不自动重建。
- 绑定不替代 AuthorizationContext；产品装配必须先授权并在撤销时停止采集，不能把本地资源存在当作网络发送许可。
- 本阶段回滚不切换生产媒体协议。代码、手动测试、结果和本备忘录同阶段提交，git log --follow -- 本文件可定位。
