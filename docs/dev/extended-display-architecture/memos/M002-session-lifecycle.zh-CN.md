# M002：基础设施生命周期纯核心

日期：2026-10-27。环境：Mac。对应设计第 2、5、7、9、10、11、22 节。

## 交付边界

`src/lib/display/core/InfrastructureSession.{h,cpp}` 实现无 I/O、单所有者状态机和有界效果队列。涵盖预留、协商、屏障、准备、首帧、传输、受限恢复、停止、清理验证及 CleanupRequired。

不在本次边界：执行进程/平台副作用、真实授权代理、GPU 操作、自动备用协议事务和产品入口装配。

## 核心决策

- 计划须符合当前双端能力与本地选择策略；当前身份、方向、连接绑定、session 和 attempt 不得替换。
- 授权、权限、输入释放、路由隔离与对端提交全部确认后，只先产生 PersistIntent；意图耐久确认后才产生 PrepareBackend。
- 取消使当前 generation 失效，但不会撤销旧资源 owner 的回收权限。
- 已派发的持久写或进程启动必须返回完成；停止不能越过在途副作用提前宣布清理成功。取消后晚到的进程实例只登记并停止，不启动媒体。
- 接收帧与 submitted/decoded 不等于呈现；需要暴露的原生呈现证据。
- startup 和恢复使用绝对期限，不因阶段进度/重复刷新而续期；恢复最多两次刷新请求。
- 资源销毁请求与独立验证分离；Worker quiescence 也必须确认。原始失败和清理失败分别保留。
- owner/session 短期重放有界拒绝；跨重启的约束由耐久账本承担。

## 验证

`src/unittests/display/InfrastructureSessionTests.cpp`，9 项通过：

AuthorizationDurability、PresentationEvidence、CancelDuringPersistence、CancelDuringSpawn、StaleFramesCleanup、AbsoluteDeadlines、RecoveryBudget、GrantRevocationCleanup、QueuedCreateCancelled。

```sh
ctest --test-dir /private/tmp/deskflow-build/src/unittests \
  -R '^Display.Infrastructure.(AuthorizationDurability|PresentationEvidence|CancelDuringPersistence|CancelDuringSpawn|StaleFramesCleanup|AbsoluteDeadlines|RecoveryBudget|GrantRevocationCleanup|QueuedCreateCancelled)$' \
  --output-on-failure
```

测试使用虚拟单调时钟和受控完成事件。它证明状态/效果约束，不代替真实 Worker 被杀、系统显示移除和 GPU fence 的平台测试。

## 接入和审查

后续 Resource Manager/Supervisor 必须报告所有在途操作的最终完成，即使已取消；不能简单丢弃旧回调。执行器必须在每个副作用前验证 token，并对晚到资源执行原 owner 清理。

当前尚未取代原 Coordinator/UsageCoordinator 的产品装配。审查重点是取消与在途持久化/进程启动、停止期限以及 worker/owner/generation 的区别。提交与备忘录同阶段，按 git history 定位。


## 后续跨平台验证补记

M006 阶段在 Ubuntu x86_64 / Qt 6.10.2 构建并执行前五个模块的 37 项测试，全部通过。Mac 仍为主开发环境；此补记不代表产品链路或 Windows 认证。
