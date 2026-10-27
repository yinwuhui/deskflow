# M009：控制面限频与有界命令结果缓存

## 模块边界

对应设计第 13.2–13.3 节，代码 `src/lib/display/control/ControlAdmission.{h,cpp}`，测试 `src/unittests/display/ControlAdmissionTests.cpp`。本模块提供准入/缓存机制，尚未装配到真实基础连接；消息正文注册表、会话/权限检查、保留容量 mailbox 及适配器信令检查仍需后续模块。它不替代 M002 生命周期或 M004/M008 持久资源终态判断。

## 实现和约束

- 每个已认证 peer 维持一个 token bucket 组合，跨重连保留：协商/命令 20 条/s、burst 40；信令 100 条/s、burst 128；Stop/撤销独立保留 5 条/s、burst 8。普通/信令流量耗尽不挤占停止，但重复 Stop 也不能无限洪泛。速率只使用本地 steady_clock；检测时钟倒退。
- 命令结果缓存每 peer 最多 256 项，跨 peer 共享全局最多 1024 项。全局计数加锁，单 peer 缓存由单 owner executor 访问。满时拒绝新动作，不为腾空间悄然丢掉刚完成的结果。
- 操作 ID 与参数 SHA-256 绑定：新项只执行一次；同参数 pending 不重执行；完成后返回相同结果；同 ID 不同参数是冲突。缓存结果仅固定类型状态/错误码/公开引用/清理证明标志，字段长度受限，不缓存任意 JSON、SDP 或 bearer 凭据。
- 参数摘要包括共同 control binding、type/session/attempt/plan hash 和已由消息注册表验证的正文；不包括传输 sequence/message ID。相同语义消息换传输序号仍能去重。使用 Qt 的确定性本地对象编码，不宣称是任意 Unicode/数值 JSON 的 RFC8785 实现。
- 操作 deadline 最多 15s，重复 begin 不续期。pending 到期变成 Expired 并保留十分钟固定 tombstone；晚到的 Active/成功不能复活操作。晚到的已验证 Stopped 可以补充 cleanupVerified，但保留原 OperationDeadline 根因。
- 缓存保留十分钟，查询不刷新保留期。保留期届满后可能再次返回 Execute，调用者仍须先用当前会话、owner 和授权终态拒绝旧请求；缓存不是永久重放数据库。
- 缓存销毁只退还缓存预算，不能代替会话停止/资源清理。基础连接重建仍按设计停止旧媒体；必须保留 peer 的限频与生命周期屏障，不能每次重连重置所有保护。

## 验证记录

Mac arm64 / Qt 6.8.3：6/6，通过（3.24s，包含首次可执行加载）；Ubuntu x86_64 / Qt 6.10.2：6/6，通过（0.06s）。

```sh
cmake --build <build> --target DisplayControlAdmissionTests -j 4
ctest --test-dir <build>/src/unittests -R '^Display.Infrastructure.(ControlRate|Command)' --output-on-failure
```

测试覆盖三个独立配额及真实速率补充/时钟倒退、pending/结果幂等/参数冲突、重复消息不延长期限、晚成功拒绝与晚清理保留根因、peer/global 容量及析构回收、十分钟过期、参数/连接绑定摘要及传输元数据无关性。CI 的 clang-format 20.1.0 格式已应用。

M008 的最近一次完整相关回归为两端各 99/99，本阶段另新增 6 项单独通过；不把未重新执行的整组测试标成 105 项已执行。

## 影响、剩余工作和审查

没有改变现有生产控制消息。后续装配须在解析长度/认证/消息正文与会话状态检查后分类限频，再对确有副作用的命令调用 begin；仅 Execute 可执行动作。Adapter 或 peer 自报字段不能决定它是否属于 Stop 保留额度。原始网络队列还需要独立硬上限与停止优先通道，避免在进入本模块之前积压。Windows 未编译/实测。

模块源码、测试及本备忘录同一提交，`git log --follow -- <本文件>` 可定位。
