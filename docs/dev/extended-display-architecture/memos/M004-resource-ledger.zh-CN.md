# M004：耐久资源账本

日期：2026-10-27。实际验证环境：Mac/APFS 临时测试目录。对应设计第 11、12、21、24 节及附录 A.5。

## 交付边界

`src/lib/display/resource/DurableResourceLedger.{h,cpp}` 实现资源意图、资源状态迁移、所有权/依赖约束、校验日志、检查点和启动重放。只管理资源记录及证据，不代替平台资源创建/销毁执行器。

旧 ResourceJournal 仍在现有媒体运行时使用；迁移与原生所有权核查需在后续装配完成。本模块单独验证成功不代表旧产品已经采用新账本。

## 持久化与安全决策

- BMDL 32 字节大端头、版本、严格序号和 IEEE CRC-32；正文为受限 JSON。CRC 仅用于损坏检测。
- 意图必须完成文件刷新/同步后才返回成功。Mac 使用 fsync 与 F_FULLFSYNC，POSIX 检查点还同步目录。
- 原子检查点为独立 generation 的 snapshot/events.log，再提交 CURRENT；旧代次只在新指针持久确认后回收，保留当前及前一代。
- CURRENT 丢失且已有 generation 时拒绝初始化空账本，防止遗失原资源所有权。
- 不完整尾部可恢复至有效前缀；校验错误、中间损坏、未知版本不扫描猜测继续。
- 单 owner 最多 64 个活动资源、192 个记录硬上限，保留有界关闭记录；关闭 owner 不可直接复用。
- 回收有依赖顺序，父资源有未关闭子引用时不能确认释放。资源定位符不可在观察后替换。
- 私有目录、独占锁、符号链接拒绝；只回收结构已识别的私有旧检查点目录，不递归删除有其他条目的目录。
- Windows 实现了用户 DACL、FlushFileBuffers 和 MOVEFILE_WRITE_THROUGH 路径，但尚未在 Windows 编译或实测。

## 验证

`src/unittests/display/DurableResourceLedgerTests.cpp`，8 项通过：DurableIntentReplay、ExclusiveLedger、TornLedgerTail、LedgerCorruption、MissingCurrent、LedgerCompaction、DependencyOrder、LedgerSymlink。

```sh
ctest --test-dir /private/tmp/deskflow-build/src/unittests \
  -R '^Display.Infrastructure.(DurableIntentReplay|ExclusiveLedger|TornLedgerTail|LedgerCorruption|MissingCurrent|LedgerCompaction|DependencyOrder|LedgerSymlink)$' \
  --output-on-failure
```

测试实际写入/重新打开临时账本、截断/修改字节及检查原文件内容；没有创建真实系统副屏。电源故障、Windows 文件系统和真实平台资源崩溃恢复仍需系统测试。

## 审查与后续

重点审查耐久顺序、损坏边界、快照/日志一致性、所有权证据来源和资源依赖。ResourceEvidence 的 verified 只能由受信平台提供者产生，不能直接从网络正文传入；账本验证结构和合法状态迁移，不自行证明平台 API 已执行。

后续需接资源管理者、真实平台证据、旧日志导入和 Worker/平台在途创建清理流程。提交与本备忘录同阶段，可按 git history 定位源码。
