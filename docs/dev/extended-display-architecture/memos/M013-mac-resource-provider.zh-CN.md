# M013：Mac 原生资源提供者与耐久恢复核查

2026-10-28。对应架构第 4、9、11、12、22、26 节。

## 完成边界

新增 `platform/macos/MacResourceProvider.{h,cpp}`，实现 PlatformResourceProvider 的 snapshot/create/configure/quiesce/inspect/release，接入真实 ResourceExecutor。一个实例固定 owner/session/attempt/plan 与一个平台 helper，最多一个输出；支持 worker→virtualOutput 的显式资源依赖。新增手动 `MacResourceProviderSmoke`，不注册为自动 CTest，避免常规回归改变桌面。

本阶段没有把 provider 装配进 DisplayService，没有实现采集、媒体、跨机协商、Linux/Windows 资源提供者，也没有完成整进程崩溃恢复服务。

## 实现与不变量

- 创建 worker 前由执行器耐久写入意图。完成 OS 身份/HMAC 握手后以原生出生身份记录 Observed、Active；输出意图必须依赖已 Active 的同 owner helper。
- 输出使用已耐久写入的资源 ID、creationIntent 与创建前拓扑摘要。收到 helper 的强引用对象身份后先由执行器耐久写入 Observed；只有 configure 阶段才能发 ConfigureOutput。重新枚举输出 UUID/ID、非镜像、尺寸和自身右侧布局，才可 Active。
- 可信本地绑定、provider/version/verificationPolicy、owner/session/plan/userSession、调用线程共同限定操作。创建还要求当前 boot 和交互图形会话一致。恢复只能检查原记录，不能通过变更计划收养新资源。
- quiesce 先关闭原 helper 的创建能力并等待其退出；正常停止最多等待 4 秒，随后经监督器终止自己持有的子进程，全部等待受传入绝对期限约束。未知/晚到创建必须先静止，不能先枚举一次为空就关闭 owner。
- 这个平台输出与持有它的 helper 生命周期耦合；停止本次输出也结束该 helper。当前没有输出无损迁移/重建，不能把这个模型宣称为可独立重启的平台 helper。
- 已知输出身份时，需系统枚举确认原 UUID 缺失，并证明 helper 原进程已退出，才给 systemAbsenceCheck。原始 Intent 没有 UUID 时，要求原 helper 的出生身份已不存在，再结合本应用确定性 hint 缺失；hint 只做保守枚举线索，不能授权删除。
- 重新构造 provider 后没有 live 注册表，拒绝终止或接管持久 PID，即使该进程仍活着。无法证明缺失时保留 CleanupRequired；worker 创建后尚未写回出生身份的窗口仍可能需要人工处理，不能猜测关闭。
- 没有按显示器名称/厂商/型号批量删除、没有全拓扑回滚、没有恢复网络密钥。输出删除始终通过原 helper 自己的强引用释放。

## 实际验证

Mac arm64 / Qt 6.8.3，真实系统输出与真实子进程的四个手动场景通过：

1. normal：执行器创建并激活 helper/输出，然后按依赖关闭，系统原生枚举确认输出消失，owner 闭合。
2. recover：输出 Active 后销毁执行器/provider 和其拥有的 helper，真正关闭并重新打开耐久账本，再构建 provider/executor，以原记录核实进程/输出缺失后闭合 owner。
3. intent：意图先落盘、原生输出创建成功，故意不写 Observed；销毁旧 provider 后重开账本，仅凭原 helper 出生身份缺失加系统受限枚举，回收原 Intent 并闭合 owner。
4. ambiguous：向测试账本写入当前仍存活但没有 helper 注册关系的真实进程身份。恢复保持 CleanupRequired，不终止该进程，不关闭 owner。

前两个恢复场景是受控析构和账本重开，**不是整个 agent 被 SIGKILL 后重启**；没有将二者视为同一证据。前三项创建后均由 1 个系统输出恢复为 1 个，结构化结果及源码 SHA256 见 [真实结果](M013-native-results.json)。失败账本保留，退出码与 stderr 同时保存。

本阶段重新构建并执行完整相关回归：Mac **115/115，13.79s**；Ubuntu **115/115，13.43s**。包括 77 项基础设施和 38 项既有服务测试。Ubuntu 只验证公共构建/回归，Mac provider 源码在 Linux 条件排除，不能称为 Ubuntu 原生资源后端通过。

```sh
cmake --build <build> --target MacResourceProviderSmoke busymouse-display-platform-helper -j 4
MacResourceProviderSmoke <absolute-helper-path> normal
MacResourceProviderSmoke <absolute-helper-path> recover
MacResourceProviderSmoke <absolute-helper-path> intent
MacResourceProviderSmoke <absolute-helper-path> ambiguous
ctest --test-dir <build>/src/unittests -R '^Display\.(Infrastructure\.|Service\.|UsageService\.)' --output-on-failure
```

Ubuntu 同步前比较两个已修改构建文件与阶段起点 SHA256，均一致。只同步五个明确文件，保留远端其他修改和不同 Git 历史。

## 接入限制与审查重点

- provider 同步等待只允许在专用资源线程，内部仅泵该线程 Qt 事件；测试进程没有 UI。产品装配必须实际建立该线程并保留执行器防重入，不能在 GUI 线程直接调用。
- 原生系统枚举目前仍在 provider 所属线程调用 CoreGraphics；这些系统调用无法被 C++ 截止时间打断。若 WindowServer 调用卡住，现有超时不能保证 provider 本身退出；后续需把探测隔离到受监督原生边界，完成这一点前不发布有界停止认证。
- 需要 agent 启动恢复装配、全进程硬崩溃/父 IPC EOF/系统重启故障注入、锁屏/热插拔/长时验证、当前拓扑代次管理，以及原生采集源授权和媒体装配。M011 未解释的间歇测试失败仍保留。
- 无 Windows 编译/实测证据，无 Linux 虚拟输出证据。当前产品入口仍为旧运行时。

代码、手动验证工具、结果与本备忘录同阶段提交；通过 `git log --follow -- <本文件>` 定位。回滚此模块不切换生产媒体协议。
