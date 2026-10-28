# M012：原生进程出生身份与监督器绑定

2026-10-28。对应架构第 4.2、5、11.3、12 节与 INV-11。

## 边界与实现

新增 `supervision/NativeProcessIdentity.{h,cpp}` 和 `NativeProcessIdentityTests.cpp`。这是只读系统证据模块，已经接入 WorkerProcess 的真实启动及 POSIX 进程组终止检查。尚未实现持久恢复 PlatformResourceProvider；它不提供按持久 PID 终止进程的接口。

Mac 使用 `proc_pidinfo(PROC_PIDTBSDINFO)` 的内核启动秒/微秒，Linux 使用 `/proc/<pid>/stat` 第 22 字段 starttime；Windows 源码使用 GetProcessTimes creation FILETIME 和原生进程句柄的零等待退出检查。Windows 分支未编译/实测，不能发布为认证实现。

`process1:<platform>:<pid>:<birth>:<fraction>` 为有界、规范编码；拒绝零/越界 PID、未知平台、非法分量、额外字段、前导零等非规范表示。该定位证据必须与资源记录中 bootInstance、userSession、owner/workerInstance 一起使用。仅有该字符串不构成资源所有权或终止授权。

只读核验返回 Exact、OriginalAbsent、Unknown：系统可证明原 PID 已不存在或同 PID 的出生身份不同，才证明原进程缺失；权限不足、读取/解析失败、跨平台或非法输入都为 Unknown。Linux 按最后一个右括号拆分 comm，避免含空格/括号的进程名改变 starttime 字段位置。没有使用 `kill(pid,0)` 的模糊结果代替原进程身份。

WorkerProcess 在 QProcess started 后、发送启动秘密之前取得原生身份；无法取得则拒绝启动后续操作。POSIX 组终止同时要求当前 live QProcess、原 PID 和出生身份一致；未知证据不发送进程组信号。子进程回收与状态转换仍在 QProcess 所属线程串行执行。保留原生 QProcess/Windows Job 的自身子进程清理，不增加恢复任意 PID 的接口。

## 验证

Mac arm64 / Qt 6.8.3 与 Ubuntu x86_64 / Qt 6.10.2：新增 2 项测试分别通过，包含当前进程身份往返、同 PID 不同出生值、非法身份不能证明缺失，以及真正启动/回收子进程后的系统缺失。出生值变化属于受控模拟，不宣称实际触发了内核 PID 复用。

接入监督器后，两端分别执行 8 项原生身份/真实子进程测试：Mac **8/8，11.54s**，Ubuntu **8/8，11.43s**。本阶段未重复声称整体 115 项回归已执行；累计基础设施注册测试为 77 项，上一阶段完整相关回归为 113 项。

手动 Mac 工具改用 helper 的原生出生身份作为进程资源定位。正常停止与强制终止各执行一次，独立原生枚举均证明输出缺失并闭合资源 owner，见 [最终原生结果](M012-native-results.json)。仍没有采集、传输或呈现认证。

```sh
cmake --build <build> --target DisplayNativeProcessIdentityTests DisplayWorkerProcessTests -j 4
ctest --test-dir <build>/src/unittests -R '^Display.Infrastructure.(ProcessBirth|WorkerProcess|WorkerRegistration|WorkerStop|WorkerParent|WorkerFinal)' --output-on-failure
```

Ubuntu 同步前逐文件比较修改文件与 Mac 阶段起点的 SHA256，未发现差异；只同步本阶段明确文件，保留其他远端修改。

## 诊断、恢复限制与后续

MacPlatformHelperSmoke 在失败时保留临时耐久账本，并立即向 stderr 输出其位置；仅完整成功后恢复自动删除，且锁/账本先于目录析构。修正开发中一次主动删除目录早于账本锁析构的测试工具警告。真实实验的外层执行同时保留退出码和 stderr，不能再只留下 Python CalledProcessError。

M011 中未保存 stderr 的间歇失败仍未得到根因解释。本阶段真实通过不能抹去该限制，能力保持未认证。还需要实现原生进程注册表、跨重启读取的保守恢复、真实资源 provider 与核心 effect 装配。原进程已退出不能单独证明其输出或共享表面已不存在，仍必须调用对应系统枚举。

生产媒体入口尚未迁移。代码、测试、备忘录同阶段提交，通过 `git log --follow -- <本文件>` 定位。
