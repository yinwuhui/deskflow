# M001：有向能力与管线规划核心

日期：2026-10-27。开发/构建：当前 Mac（Apple clang、Qt 6.8.3 工程环境；本模块本身不依赖 Qt）。对应设计第 5、6、7、25 节。

## 交付边界

新增纯 C++ 类型、校验与选择算法，按发送设备的 send 与接收设备的 receive 能力求兼容管线。支持 RDP 与 WebRTC，方向可以使用不同协议；不实现媒体库、网络或系统虚拟屏操作。

代码：`src/lib/display/core/InfrastructureTypes.{h,cpp}`。独立目标 `deskflow-display-infra-core`，由旧显示基础库引用；不依赖 common、Qt Widgets 或具体媒体引擎。

## 决策与约束

- 能力包含实现版本、环境签名、方向、协议、格式、安全、硬件、反馈、呈现与回收能力。
- 同协议名称不等于兼容：还校验图形模式、安全剖面、源格式及反馈闭环。
- 发行认证档案来自本地策略，不采信远端自报“已认证”。默认拒绝未认证候选；仅本地开发诊断策略允许实验组合。
- 未验证权限等条件标记为 conditional，不能直接提交为就绪计划。
- 版本或环境改变会使原认证条目不再匹配。候选最多保留两个，完整拒绝解释保留在有界集合中。
- V1 仅支持 1080p SDR/30fps 认证档位；不会因整数格式校验通过而隐式开放 4K/HDR。
- uint64 计数用严格十进制字符串解析，拒绝前导零、符号、浮点与溢出。

## 验证

测试文件：`src/unittests/display/InfrastructurePlannerTests.cpp`。

命令（在已配置 Mac 构建中）：

```sh
ctest --test-dir /private/tmp/deskflow-build/src/unittests \
  -R '^Display.Infrastructure.(DirectionMatrix|AsymmetricProtocols|SafetyGates|NoProtocolConversion|CertificationInvalidation|InvalidDeclarations|FeedbackAndConditional|PlanBinding|CounterBounds)$' \
  --output-on-failure
```

实际结果：9 项通过。覆盖三平台共 9 种有向组合的纯逻辑、两个方向使用不同协议、仅接收能力不得充当发送、无共同协议不得虚构网关、网络/清理/忙碌/身份/输入策略门槛、认证失效、非法声明、反馈与权限条件、计划绑定及计数边界。

矩阵输入是合成能力，不能作为对应硬件/后端已支持的证据。

## 接入和审查

尚需接入真实后端探测、认证清单、网络能力交换及产品运行时。`validatePlan` 要求精确匹配本地当前候选；控制编码器还需验证计划摘要，不能只调用字符串格式校验。

审查重点：能力组合约束、认证来源、候选排序及默认诊断开关。代码与本备忘录同阶段提交，按 `git log --follow -- <本文件>` 定位。


## 后续跨平台验证补记

M006 阶段在 Ubuntu x86_64 / Qt 6.10.2 构建并执行前五个模块的 37 项测试，全部通过。Mac 仍为主开发环境；此补记不代表产品链路或 Windows 认证。
