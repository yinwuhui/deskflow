# M003：控制消息公共封装与计划编码

日期：2026-10-27。环境：Mac，Qt 6.8.3。对应设计第 5、7、13、21 节及附录 A。

## 交付边界

`src/lib/display/control/InfrastructureControl.{h,cpp}` 提供严格有界 JSON 读取、双端 nonce 控制绑定、版本化信封、序号防重放、计划编码/摘要及当前能力校验。构建在独立 `deskflow-display-infra-runtime` 中，仅依赖新纯核心及 QtCore/QtNetwork，不依赖业务 common 或 Widgets。

未完成范围：各消息正文处理器、后端标准信令校验、业务幂等结果缓存、网络控制适配装配。这些不能被本模块的公共封装通过替代。

## 决策

- Qt JSON 会保留重复键的最后值，因此在构造 QJsonObject 前验证原始 token，拒绝重复键（包括 Unicode 转义后等价键）、非法 UTF-8/孤立 surrogate、深度/数量/长度超限。
- 普通控制消息硬上限 64 KiB；本地持久化解析可采用单独固定预算，不由远端指定。
- 每条实际认证连接有随机 nonce 绑定，两端本地连接引用可以不同，共同摘要保持相同；本地真实连接参数不能从 peer JSON 获得。
- 同连接重复 nonce 可幂等，改变 nonce 拒绝；旧连接/旧摘要与旧序号不得重放。
- SessionPlan 从当前本地候选重建并校验，拒绝增加输入/音频/重定向或替换尺寸/身份/版本。
- 计划编码实现本文 schema 的 ASCII 标识＋有界整数 RFC8785 子集，不宣称通用任意 JSON 的 JCS 实现。
- 加密、nonce 和摘要使用成熟 Qt/系统设施；CRC 等非安全校验不承担身份认证。

## 验证

`src/unittests/display/InfrastructureControlTests.cpp`，4 项通过：StrictJson、BindingReplay、EnvelopeGates、PlanCanonicalization。

```sh
ctest --test-dir /private/tmp/deskflow-build/src/unittests \
  -R '^Display.Infrastructure.(StrictJson|BindingReplay|EnvelopeGates|PlanCanonicalization)$' \
  --output-on-failure
```

覆盖非法编码、重复键、深度/大小边界、两端不同本地 connection ID、nonce 替换、旧信封、权限/尺寸/摘要替换与过期能力。非规范空白可解析，但摘要依据规范计划。

## 审查与后续

审查信封注册范围、字节预算、序号消费时机、schema 限制及计划摘要绑定。收到一个合法 Envelope 后，仍必须验证当前状态、授权、正文注册表和标准后端消息；不能直接执行系统操作。

尚未切换现有 usage-display/1 通道，避免将未完成的正文处理器对外发布。提交与本备忘录同阶段，使用 git history 定位。
