# M006：原生 Worker 进程监督与本地连接

## 完成边界

实现设计第 4.2、13.4、18 节的本地进程创建、启动身份、父子连接、心跳租约和有界停止。代码：`src/lib/display/supervision/WorkerProcess.{h,cpp}`；真实子进程夹具：`src/unittests/display/WorkerProcessTests.cpp`。产品媒体可执行程序和资源执行器尚未接入，故本模块不能作为拓展屏已可用的证明。

## 接口与不变量

- `WorkerRegistration` 由可信装配器提供：绝对规范路径、完整文件 SHA-256、backend/profile 白名单。拒绝符号链接、散列不符、不匹配管线、非普通文件、过大文件；POSIX 还检查 owner 及组/其他用户不可写。单实例监督器只能 launch 一次。
- 启动参数只有 `--display-worker`。随机挑战与完整绑定通过 QProcess 的私有 stdin pipe 一次交付，随后关闭写端；不放入 argv/环境。启动 JSON 8 KiB 上限，worker 校验 schema 和父进程 PID。解析器内 Qt 临时字符串可能留下内存副本，清除 QByteArray 不等于经过认证的全内存秘密擦除。
- POSIX 使用私有临时目录和用户级 Unix socket，Mac 使用 `getpeereid + LOCAL_PEERPID`，Linux 使用 `SO_PEERCRED`；双向校验实际连接对端 UID/PID。Worker 另验 `getppid()`，但它不替代 socket 凭据和挑战证明。
- Windows 实现 Qt `UserAccessOption` 限制 named pipe 访问，双向查询 named pipe 对端 PID；子进程在交付挑战之前加入带 `KILL_ON_JOB_CLOSE` 的 Job Object。**Windows 分支未编译/实测，不能发布为认证能力。**
- POSIX 子进程创建独立进程组，监督器仅终止自身 QProcess 对应的进程组/句柄；不按进程名清理。Windows 使用 Job Object。不把进程退出当作输出已消失。
- socket 读写和解析队列均有预算；stdout/stderr 定向空设备，避免不受控输出堆积或秘密写入日志。业务诊断走类型化错误事件。移除继承的 DYLD/LD/GST 环境覆盖，正式打包仍需插件白名单与签名校验。
- 心跳约 250ms，协议接收租约 1.5s，握手 3s；EOF/租约失败调用 worker 的自停回调。监督器 Stop 绝对目标 5s 后终止该进程；Qt 事件循环调度可能导致少量超时偏差，测试容忍总耗时 <6s，不能把其描述为硬实时保证。
- 正常停止需 Quiesced 加正常进程退出；崩溃/非零退出保留异常。进程无法启动也交付一次终态通知，避免调用者永久等待。平台资源存在性仍须独立验证。

## 实际验证与发现

Mac arm64 / Qt 6.8.3 和 Ubuntu x86_64 / Qt 6.10.2 均构建、运行真实子进程。新增 5 项：正常握手→Ready→Start→呈现→Stop→Quiesced→退出；注册散列/管线拒绝；崩溃、握手超时、伪造父 PID；拒绝 Stop 的有界强制终止；断开父 socket 后 worker 自停退出。配合 M005 协议测试，共 12 项均通过。

```sh
cmake --build <build> --target DisplayWorkerProcessTests DisplayWorkerProtocolTests -j 4
ctest --test-dir <build>/src/unittests -R '^Display.Infrastructure.Worker' --output-on-failure
```

本阶段修复 M005 停止竞态：Stop 发出时已经在管道中的合法 Ready/帧仍经过 MAC/序号验证，但不再作为可执行/可呈现事件派发；否则会误杀正常停止。新增该竞态回归。Ubuntu 默认构建 umask 生成 775 可执行文件，注册器正确拒绝；测试改为复制到私有目录并设 755，未放宽产品安全门槛。

Mac 全部新基础模块加既有显示/用途服务回归为 80/80，通过；Ubuntu 同范围完整回归亦为 80/80，通过（12.00s）。本备忘录随后可追加更大范围回归结果，不能覆盖历史结果。

## 剩余接入与审查

- WorkerConnection 的 onShutdown 回调必须由实际媒体实现完成原生线程停机和资源释放；本模块只保证及时通知/有界终止。
- 原生显示/GPU 对象需要平台枚举确认，不能用 `running()==false` 自动关闭资源账本。
- 当前本地用户会话字符串是可信装配器绑定，仍须由各平台 session provider 校验实际交互会话和权限。发行打包需签名/不可写目录、依赖插件清单和许可证审查；开发文件散列不能替代发布签名政策。
- 当前未替换旧生产运行时；移除这些独立目标即可回滚本阶段。Windows Job/pipe、Windows 进程组外资源和非 Mac/Linux 平台仍未验证。

提交定位：本备忘录与源码、测试及 M005 竞态修复同一提交，通过 `git log --follow -- <本文件>` 查询。
