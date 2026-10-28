# M022：编码器回调可持有的只读 CPU 帧租约

2026-10-28。对应架构第 9、14、15、19、26 节，为原生编码器/appsrc 输入所有权准备，不代表已接入编码或网络。

## 边界与实现

FrameAllocation 新增可选的本地 cpuData，ReadFrame 显式携带只读视图及真实 allocationBytes。opaque owner 是持有对象，不能被解释为像素首地址；可信导入方保证视图指向 owner 保活的连续分配，且 CPU 所有者可在消费者线程安全销毁。字段不进入 IPC，不传裸进程地址到另一进程。

FramePool 新增 readNewestCpu：只在最新 Published 帧有 CPU 内存域及明确 CPU 视图时借用；拒绝 opaque CPU owner/GPU 时不改变槽状态，不伪造 GPU fence。原 readNewest 行为保留。

CpuFrameLease 在借用槽之前建立回调持有对象，避免持有对象分配失败后遗留 Reading 槽。它只暴露只读 descriptor/data/allocationBytes，隐藏 owner 与 ticket；共享持有者的最后一次析构先释放 ReadFrame owner，再 releaseRead 归还预算。只持有线程安全 FramePool，不捕获 MacCaptureSource 原生 facade，因此可在编码器消费者线程最后释放，不触发原生 UI 对象析构。Reading 租约仍受原有跨 epoch 预算约束，retire 不强制回收。

MacCaptureSource 将自己的连续 BGRA CPU 分配显式导入 cpuData，新增 takeCpuLease，在读取前后检查源许可仍 live；已经借出的旧帧仍须由媒体停止过程丢弃/排空，不能靠撤销标记强行释放。

## 测试

新增两项纯核心行为测试：

- FrameCpuCallbackLifetime：vector owner 对象地址与像素 data 地址不同；lease 必须保持真实像素视图和字节数。retire 后销毁生产端 pool/lease 引用，预算仍占用；另一个 std::thread 消费最后一个共享持有者后数据仍正确、预算归零。
- FrameCpuViewGates：拒绝空 pool、没有显式视图的 CPU owner 及模拟 IOSurface；拒绝不借出槽，不需要伪造 fence，retire 可正常排空。

Mac 全部十项帧模型测试通过（3.14 秒）。新增手动 capture-callback 使用真实 1920×1080 副屏采集数据、借用期间的停止屏障，并在另一个 std::thread 最后释放；预算归零且资源 owner 闭合。另重跑 capture/capture-cancel/capture-revoke，四项退出 0，输出均消失。原始日志、退出码及源码摘要见 [原生结果](M022-native-results.json)。该消费者线程模拟编码器完成回调，**没有实际调用编码器**。

重新链接全部相关目标后，Mac 120/120（25.78 秒）、Ubuntu 120/120（18.88 秒）。Ubuntu 同步前四个公共源码/测试文件摘要全部匹配本阶段起点，只同步这四个文件；Mac 条件源码没有在 Ubuntu 编译。Windows 未编译实测。

## 限制、接入与提交

- cpuData 是可信平台导入的进程内视图，不能通过校验元数据证明任意指针有效。只有带正确所有权的可信分配可导入；它不是共享内存或跨进程 GPU/句柄方案。
- GPU 测试仍为模拟对象，未增加任何真实 GPU 认证；没有编码器内部内存、网络或呈现证据。
- 尚未接入 GStreamer appsrc、原生媒体 Worker、产品装配及跨机测试；生产媒体路径未改变。调用者必须在最后一个 lease 释放前保持使用的像素指针不逃逸，停止必须等待租约排空。
- 内部 C++ 帧结构有新增字段，使用者需要重新编译链接；IPC schema 不变。

代码、测试、手动结果与本备忘录同阶段提交，git log --follow -- 本文件可定位。
