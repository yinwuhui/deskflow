# M018：Linux 已验证描述符执行与公共原生启动入口

2026-10-28。对应架构第 4、13、24、26 节，推进 M016 的验证到执行绑定。

新增 prepareWorkerProcess：验证登记的文件、配置固定参数并清除 LD_/DYLD_/GST_ 环境覆盖。WorkerProcess 与 Mac provider 的隔离探测共同使用它；探测不再继承动态加载器或 GStreamer 覆盖。参数及环境只由可信本地调用者提供，配置后不允许再变更环境、参数或 child modifier。

Linux 对验证过的同一打开文件描述符执行 F_DUPFD_CLOEXEC，保持其对象引用。QProcess 子进程 modifier 直接 fexecve 该描述符，不再次按原路径解析。argv/envp 在父进程中预建，子进程不分配内存；执行失败由 Qt 6.7+ 的 failChildProcessModifier 关闭启动，旧 Qt 退出 127，不回退路径执行。描述符随准备进程对象销毁而关闭，并在成功 exec 时不泄露给 Worker。CLOEXEC 描述符执行脚本会失败；这里只支持原生 Worker，没有脚本解释器回退。

新增 WorkerExecutableBinding 真实进程测试：Linux 在 prepare 后、start 前把已校验文件 rename，再用退出 91 的脚本替换原路径；必须仍执行原生文件并返回标记 73。Mac 同项只验证公共入口执行原生标记，不模拟路径替换，不作为 Mac 竞态已解决的证据。

实际测试：Mac/Ubuntu 各 7/7 监督器测试通过（13.33/12.13 秒）；重新链接受影响测试后完整相关回归各 116/116（23.83/18.30 秒）。Mac 另运行真实 provider normal 场景，显示器 1→1，outputAbsenceVerified 与 ownerClosed 为 true。该原生验证不涉及采集、媒体及跨机呈现。Ubuntu 同步前四文件 SHA256 与阶段起点均相同，仅同步这四文件；Mac 条件源码不需同步到 Linux 本次编译。

限制：Mac 和 Windows 仍按路径执行，替换竞态未解决；Windows 未编译实测。fexecve 解决路径对象替换，不防同 inode 内容被有写权限者修改，不认证动态依赖和安装包；既有文件所有者/权限/哈希检查继续保留，完整同用户账户攻陷不在架构保证内。文件读取和启动的阻塞问题仍未解决。尚未迁移生产媒体入口，不能发布完整供应链或期限认证。

参考：[Qt 6.8 QProcess](https://doc.qt.io/qt-6.8/qprocess.html)、[Linux man-pages fexecve](https://man7.org/linux/man-pages/man3/fexecve.3.html)。代码、测试和本备忘录同阶段提交，git log --follow -- 本文件可定位。
