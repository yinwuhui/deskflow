# M007：帧布局、跨 epoch 内存预算与进程内帧租约

## 完成范围及契约

对应设计第 14、15 节，完成纯 C++ 帧描述校验、**进程内**槽位所有权及统一分配准入；代码 `src/lib/display/frame/FrameModel.{h,cpp}`，测试 `src/unittests/display/FrameModelTests.cpp`。不含 OS 共享内存映射、跨进程 fence/句柄传递、真实 GPU import 或具体编码器队列，不能用本模块结果声称已实现跨进程零拷贝。

## 决策及安全约束

- `FrameDescriptor` 绑定 session/source resource/topology generation/epoch、真实单调捕获时间、帧序号、逻辑/像素尺寸、旋转、SDR BT.709 色彩、光标、memory domain、可见/损伤矩形及平面布局。支持 BGRA8、NV12、I420，后两者 V1 限偶数尺寸。
- 先检查尺寸/乘法溢出、stride×rows、offset+length、平面重叠、矩形和数组预算，再准入。默认 1920×1080，Profile 可限制更小；V1 SDR，不隐式接受 HDR。不得把帧数换算的伪时钟当捕获时间。
- 一个 Worker 的所有帧池及退役 epoch 共享 `FrameBudget`，最多 6 个真实分配、64 MiB；只有先成功 reserve 才允许平台分配。adopt 要求字节数和内存域相同且唯一 owner；提供者必须诚实报告真实分配，包括 GPU padding。
- 槽位 `Free→Writing→Published→Reading→Released→Free`，池实例、epoch、slot generation 全部匹配。发布内容不可修改；消费者持有强引用时不得释放其预算。只读取最新的完整原始帧，丢弃未被读取的旧原始帧；本模块不接收编码数据，故不会任意丢掉 H.264 依赖帧。
- GPU 发布要求提供者已确认生产 fence。消费者释放后仍等待匹配 fence 完成；不能凭应用引用消失或超时复用。fence/token 判断来自可信本地平台适配器，不能由网络 JSON 提交。
- retire 仅禁止新工作并丢弃未读取帧；正在写入者要主动 abortWrite，正在读取/GPU pending 者要完成原来的释放流程。
- 若帧池被提前销毁，将仍在写入/读取或等待 GPU 的分配转入 Worker 预算隔离区，保持强引用和预算，阻止新 epoch 绕过上限。隔离区通过自持引用保持存在，最坏为有界保留；只能由可信平台执行器在**所有导入上下文终止**后调用释放。仍有应用强引用时拒绝释放。此保守路径可能暂时耗尽预算，属于可见清理阻塞，不能盲目释放“恢复可用”。
- 资源最后一个 deleter 在核心锁外执行，释放真实对象之后才退还预算。需要异步回到原生线程销毁的提供者，必须将分配计数保留至实际完成，不得仅排队就归还本模块准入预算。具体提供者接入时需单独认证该条件。

## 实际验证

Mac arm64 / Qt 构建环境、Ubuntu x86_64 均通过新增 8/8 测试。本模块本身无 Qt 依赖。

```sh
cmake --build <build> --target DisplayFrameModelTests -j 4
ctest --test-dir <build>/src/unittests -R '^Display.Infrastructure.Frame' --output-on-failure
```

覆盖：3 种正常布局及溢出/重叠/超限/无效时间；跨池表面/字节预算；最新帧和旧帧丢弃；读取引用未释放时保持预算；旧 ticket/epoch/重复释放；生产/消费 fence；退役期间的写入/读取确认；提前析构后的隔离区及新 epoch 拒绝越界；资源 deleter 重入只读接口时不死锁。GPU 测试使用内存对象模拟 fence/归属，**没有真实 GPU 实验**。

## 接入、影响及审查

当前产品媒体路径尚未使用本模块；不改变既有帧传输，可独立回滚。后续需接入原生采集/呈现、编码反馈、OS 原生句柄及共享内存，测量实际分配和引用/fence 生命周期。审查重点是提供者的实际字节数、销毁线程/完成语义、隔离区解除凭据；这些不能仅靠通用核心证明。Windows 未编译/实测。

本备忘录、源码、测试同一提交；以 `git log --follow -- <本文件>` 定位。
