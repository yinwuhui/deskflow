# M024：生命周期 owner 有界 mailbox

2026-10-29。对应架构第 10.2/10.3、13、21.5、26.2 节。本阶段实现公共 core 内的类型化队列与 InfrastructureSession 分派；生产网络、平台和 Worker facade 的调用点尚未切换。

## 边界与设计对应

新增 `core/SessionMailbox.h/.cpp`，不依赖 Qt、媒体库或平台对象。构造及 drain 必须由同一 owner 线程执行；生产者线程只能 post。一个实例绑定一个已预留的 session/owner/attempt，不因 core 后来接受另一个会话而隐式换绑。

队列承载有类型的停止、撤权、控制断开、原实例退出、日志完成、进程挂接、准备、呈现、恢复、排空、销毁、核查与统计事件。不得携带任意 JSON、原生资源持有对象或帧像素。每个标识按固定长度校验，正文只有枚举、布尔、有限标识和整数。

- 普通完成最多 64 项，可缩小但不能扩张；默认每轮 drain 32 项，硬上限 64。
- 撤权、断线、Worker 退出、Stop 各有独立单槽，按此顺序先于普通完成分派。相同类别合并重复通知，保留已排队的原因；不同实例退出冲突明确返回 Full 并锁存停止。
- 高频统计独占一个合并槽，按完整 token 与统计序号检查；普通完成和终止事件不受统计洪泛挤占。
- 普通满队列返回 Full，表示**没有接收该完成**；同时锁存 OwnerMailboxOverflow，owner 下一轮进入受控停止。生产者必须保留操作所有权并重试完成，不能根据入队失败删除资源、丢掉原生登记或宣布清理。
- drain 在锁内取出有限批次，在锁外调用纯 core；不回调适配器、不执行原生 I/O、不持锁跨模块。每轮仍驱动本地绝对期限，但旧 mailbox 不驱动新 scope 的 tick。

## 竞态与不变量

撤销先使状态机 generation 失效；晚到普通呈现不能激活。日志完成、挂接和清理完成仍交给 core 的原 token/owner 核查，**不能统一丢弃所有旧 generation**，否则会漏回收取消期间产生的资源。

Worker 退出可能早于普通挂接事件到达。仅可信本地监督器在确认原生实例退出后，携带原启动 token 与实例 ID 提交退出；owner 先通过 attachWorker 验证该在途启动，再停止并提交该实例的 quiesced。晚到普通挂接成为无副作用的重复，不能复活退出进程。其他 Worker 的退出不影响当前实例。

统计不激活生命周期；Decoded/Submitted 和隐藏窗口证据仍不能满足首帧要求。呈现标识在完整 token 内只接受前进。退休 mailbox 的旧停止、清理和超时不能作用于新的 session/owner/attempt。

## 测试

`DisplaySessionMailboxTests` 七项行为测试：

1. MailboxPriorityFlood：10,000 次统计与 10,000 次重复撤权、64 项普通呈现并存，四个终止槽仍可用；终止优先，单轮普通批次 8，旧帧/统计不激活。
2. MailboxLateIntent：撤销先于已排队的在途日志完成，仍执行原 owner 的销毁和核查，不启动 backend。
3. MailboxExitBeforeAttach：退出超越挂接，即使普通 batch 为零也登记原实例并进入验证清理；晚到挂接无效。
4. MailboxOverflowRetry：单项容量耗尽，明确 Full、锁存停止，生产者重试晚到日志完成后仍能回收。
5. MailboxEvidenceStatistics：统计顺序/重放、解码与隐藏画面、合法呈现及帧序号回放。
6. MailboxThreadScope：四个真实 std::thread 共 4,000 次统计，错误 owner 线程 drain 被拒，外来 owner/Worker、超长 ID/未知枚举及时间倒退拒绝。
7. MailboxRetiredScope：原 owner 清理完再预留新会话，旧 mailbox 的撤权及 60 秒 tick 均不能改变新会话。

Mac 七项 7/7（3.32 秒），完整相关自动回归 128/128（20.15 秒）。Ubuntu 完整相关回归 128/128（18.47 秒）。源码摘要、原始日志和同步基线见 [M024-test-results.json](M024-test-results.json)。测试涉及真实 C++ 生产者线程和纯生命周期核心，**没有真实子进程、平台资源或媒体呈现证据**。

构建目标 DisplaySessionMailboxTests；CTest 过滤 `^Display.Infrastructure.Mailbox`。完整回归过滤 `^Display\.(Infrastructure\.|Service\.|UsageService\.)`。Ubuntu 同步两个 CMake 文件前与 M023 摘要逐项匹配，另三个新文件确认不存在后创建；不改远端其他文件和历史。

## 限制、未接入与提交定位

- 仅可信本地生产者可 post。它不认证远端消息、验证 IPC 身份或证明原生进程退出；Worker 自报退出不能转换为 MailboxWorkerExited。
- 撤权时立即撤销原生工作 permit 仍是宿主职责；mailbox 优先顺序不能替代生产线程上的许可失效。正在分派的有限普通批次不会被新事件同步重入中断。
- 未提供事件循环唤醒器、生产 facade／监督器接线、消息正文注册表、网络状态权限验证和效果 ID 去重；现有 core token 与 M009 命令缓存仍各自负责其边界。
- 不对系统全局 OOM、owner 线程停顿或失去调度承诺成功；队列容量保证不能替代进程监督的硬期限。事件不拥有 OS 资源，资源登记与释放权始终在原 owner。
- Windows 未编译实测。生产入口不变。源码、测试和备忘录同阶段提交，通过 `git log --follow -- 本文件` 定位。
