# M008：持久资源执行、补偿与恢复顺序

## 完成边界

设计第 10–12 节的通用资源执行器与平台 SPI，代码 `src/lib/display/resource/ResourceExecutor.{h,cpp}`。连同 M004 耐久账本完成“先记意图再调用平台、观测后才激活、先停生产者再按依赖释放、独立确认消失后才关闭”的通用执行闭环。**具体 Mac/Linux/Windows 平台提供者与产品线程装配尚未实现/接入本 SPI。** 测试用平台对象是受控模拟，不能当作真实显示器的系统枚举证明。

## 状态及安全决策

- Provider 静态注册上限 16，固定 id/version/kinds/verification policy；恢复兼容版本须显式白名单。未知 provider/version 不执行猜测清理。
- 授权绑定 owner/session/plan hash、本地用户会话/boot instance、绝对有效期及可从其他线程原子撤销的标志；创建前、上下文读取后、原生返回后和激活回写后重检。公共 API 只能在同一 resource owner 线程执行，拒绝可重入调用。产品应把该对象与账本装配到专用资源线程；该装配尚待完成。
- 只读获取创建前上下文摘要，再耐久写 Intent，随后调用原生 create；没有耐久 Intent 不创建。Observed 必须包含实际定位符和平台归属证据；只有重新 inspect 验证归属与目标拓扑正确后才写 Active 并交付。
- 创建调用返回失败/未知或抛出异常后保留原 Intent，通过 provider.quiesce 确认没有尚可晚到的创建，再做补偿。无法确认创建已停止时不能凭一次“枚举不存在”关闭 owner。
- 创建成功但 Observed/Active 写入失败时仍执行精确归属补偿；即使系统对象已被删除，关闭日志未耐久仍保留 CleanupRequired/owner 屏障，不冒充已清理。
- 同 intent 的 Active 重试不重新创建；参数不一致、未完成 intent 或终态 ID 不复用。依赖资源须已经 Active。此幂等结果不代替后续平台健康监测。
- 全 owner 清理必须由可信生命周期提供生产者已停止的判断；先处理没有活跃依赖者的叶子，子资源关闭后才释放父资源。每项资源每轮只尝试一次，最多 64 轮；创建绝对期限上限 10s、清理上限 15s，适配器不得通过重试续期。
- 每次删除前重新 inspect 原生归属。旧 boot/其他登录会话、平台 ID 被复用、模糊/不可用证据都禁止删除；仍允许用独立可信 absence 证明关闭旧记录。不会按名称、型号或拓扑差集猜测删除。
- release() 成功只表示调用返回，不能证明消失；之后必须重新 inspect 并提供 `systemAbsenceCheck/verified`，再耐久写 ReleaseVerified、Closed。保留 primary/cleanup 错误。平台异常转成失败/未知，不让异常路径丢失已创建对象。

SPI 调用虽同步返回，但限定在资源执行线程；真正不可中断的 OS 调用必须由受监督 helper 执行并以 deadline 终止/确认。通用库不能抢占任意阻塞 C++ 提供者，也没有把这种保证伪装成已完成的平台实现。

## 本地日志格式变化

内部 ResourceRecord schema 从 1 升到 **2**，新增不可变 `preCreateContextDigest` 和 `verificationPolicy`。外层日志帧/快照/CURRENT 格式仍为 1，与公共控制协议版本独立。

- 新 Intent 必须有创建前摘要和明确验证政策。
- 可读取已有内部 schema 1，转换成空上下文摘要与 `legacy-unproven`，仅用于恢复；不补造历史拓扑或归属证据，不允许新建 legacy 记录。
- 后续合法状态更新编码为 schema 2，仍保留 legacy 的“未知”属性；关闭需要原生独立证据。
- 未知记录版本进入 UnsupportedVersion，保留原文件并拒绝新建/猜测删除。
- 既有产品 `ResourceJournal` 仍独立，未做隐式导入或丢弃；正式迁移需平台恢复器和明确证据。

## 实际验证

新增 11/11 在 Mac arm64 / Qt 6.8.3、Ubuntu x86_64 / Qt 6.10.2 通过：耐久顺序/幂等；创建期间撤销；归属不明禁止删除及重试；独立 absence；依赖顺序；Intent-only 恢复/旧 boot 拒删；生产者及在途创建屏障/重入；Observed 回写损坏后的原生补偿；schema 1 读取不造证据；未知 schema 拒绝；原生 create 抛异常后的归属清理。

```sh
cmake --build <build> --target DisplayResourceExecutorTests DurableResourceLedgerTests -j 4
ctest --test-dir <build>/src/unittests -R '^Display.Infrastructure.Resource' --output-on-failure
```

首轮修正了新 Intent 未填标准 unknown evidence reference 导致合法意图被账本拒绝的问题。此前 8 项真实文件账本测试继续通过。阶段完整回归初次为 Mac/Ubuntu 各 97/97；补充版本/异常两项后，Mac 最新完整回归 99/99（12.73s）。Ubuntu 最新完整回归 99/99（11.82s），同样通过。

本阶段同时用 CI 指定的 clang-format 20.1.0 格式化此前新增基础设施源码/测试，改善审计可读性；除本备忘录描述的资源执行/日志改动外，旧模块变动仅格式调整。

## 迁移、限制及审查重点

生产入口仍使用旧媒体运行时，当前模块不改变现有会话；独立静态库可分阶段接入/回滚。原生平台证据、helper 注册表、拓扑/布局比较后修改、跨进程用户会话验证、资源线程调度、日志迁移和真实端到端故障演练仍待实现。Windows 未编译/实测。

审查重点：quiesce 必须排除创建晚到；任何“absence verified”只能由平台可信枚举产生；context digest 是线索而非删除授权；回写失败的补偿不得绕过归属检查；资源有依赖时不可先移除父输出。

本备忘录、实现及测试同一提交，以 `git log --follow -- <本文件>` 定位。
