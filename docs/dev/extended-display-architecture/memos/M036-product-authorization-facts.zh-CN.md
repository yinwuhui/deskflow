# M036：现有产品桥的认证、用户同意与输入隔离事实

## 目标、边界与设计对应

对应架构第7、10、13、21、22、26节及交接中的生产授权缺口。直接扩展现有 InfrastructureControlChannel、InputRouteAdapter、Coordinator 和 UsageDisplayBridge，组合实际产品适配器观测，不另建认证/输入网络栈。事实只供后续生产 owner 使用；本阶段没有签发生产 MediaWorkPermit、运行 PlanCommit、启动新媒体入口或改变 available 声明。

## 实现与不变量

- 认证连接 Lease 只能由当前核心观测且完成 nonce transcript 的 InfrastructureControlChannel 构造，带本地证书身份、对端身份、连接及控制绑定。未完成绑定不返回；连接替换、断线、适配器销毁使共享副本不可逆失效。
- InputIsolationProof 只能由 InputRouteAdapter 在既有 StopInput 流程取得完全匹配的本机核心确认后产生，要求实际按键释放/路由挂起、local focus、otherRoutesIsolated、ack/ackReceived 和准确 lastSequence；绑定 peer、连接代次、用途操作代次和独立引用。不接受对端 JSON 自称的屏障。
- 事实使用本机单调时钟，1.5秒未刷新即不可逆过期。verifyIsolation 仅向现有核心通道发 status，不恢复路由、不生成新输入授权；准确旧序列仍挂起/隔离时才能刷新，过期后的晚到回复不能复活。冲突输入、操作/连接代次变化、核心丢失及析构使全部相应副本失效。
- Coordinator 提供按完整 Key 查询的实际授权状态。接收端 adoptIncoming 不能代替 respond(true)，发送端还须有系统采集权限。纯协调器接收显式 now，保持现有等待系统权限/发送端许可的暂停语义；准备期限过后拒绝新授权查询。
- UsageDisplayBridge 的 NativeDisplayAuthorization 仅组合当前会话/角色、用途操作、网络策略、认证连接 Lease 与 InputIsolationProof。私有构造，不允许从序列化布尔值构造；停止/代次失效同步撤销。所有副本共享撤销，活跃性继续依赖连接和隔离事实。
- 新组合测试发现既有 CoreInputChannel 析构中的内存错误：成员 socket 在后声明容器销毁后触发 disconnected，failed 访问已销毁的 m_records。现在先断开 socket 回调并 abort，再在成员尚存时撤销；用途/输入 owner 使用 QPointer，父对象已销毁时不解引用。核心适配器消失同样撤销认证租约。增加活跃连接时直接销毁核心适配器的回归。

## 实际测试与失败记录

Mac 构建 InfrastructureChannelTests、DisplayCoordinatorTests、DisplayServiceTests、PeerUsageServiceTests、ProductRuntimeSmoke；Ubuntu 先核对远端工作区及所有15个目标文件SHA256匹配上一明确同步版本，再同步相同阶段文件并编译这些目标。远端既有修改保留，未reset历史。ProductRuntimeSmoke仅编译，本阶段没有把它运行结果计为产品验收。

```
ctest --test-dir <build>/src/unittests -R '^Display\.(Infrastructure\.|Service\.|UsageService\.|Coordinator\.)' --output-on-failure
```

Mac145/145通过（20.41s）；最终夹具关闭持久化后，两项核心通道测试再次通过（3.66s）。Ubuntu最终145/145通过（23.32s）。包含新增隔离事实/到期/断线三项、已有协调器的授权时限断言、通过真实本地QLocalSocket双角色授权/用户同意门槛/停止/断线测试，以及新增核心活跃连接析构场景。本地核心回复和对端控制身份来自明确夹具，不能将该测试称为真实设备TLS、对端用户操作或跨机媒体验收。

开发中的exit139及系统崩溃栈摘要已保留在M036-test-results.json。修复后显露的接收夹具错误也分别记录：等待上一轮有效模式none不足以证明事务结束；核心观测不自动赋予显示接收能力，须使用既有能力探测入口补足。调试器一次启动停滞未形成证据，已结束本轮自启进程。不以最终通过抹掉这些记录。新组合夹具最终使用无持久化用途服务，合成输入身份不写入产品偏好。

## 影响、未接入与下一阶段

旧产品媒体入口继续使用既有路径。本事实不是完整生产许可：还缺生产 owner 持有并周期刷新隔离事实、无既有输入的初次启动隔离获取、绑定本机用户会话/原生权限/接收目标、双端授权摘要交换及PlanCommit、资源/Worker/mailbox接线和撤销硬期限。已有输入→扩展显示路径已有事实验证；不能将仅支持该获取路径说成所有启动方式已完成。

下一步应在真实生产会话 owner 中消费这些事实并贯通计划提交和资源启动，避免继续增加与闭环无关的独立模块。生产入口迁移后另做双机真实认证及用户撤销/控制断线/Worker故障验收。Windows未编译/实测。

代码、测试、原始结果和本备忘录同阶段提交，用git log --follow --本文件定位。回滚须成套回滚桥的事实接口及其使用者；核心析构修复可以独立保留，不能回退为假清理确认。
