# M021：Worker 提升权限位与 Mac 路径/ACL 策略

2026-10-28。对应架构第 4、13、21、24、26 节，继续收紧 M016/M018 启动边界；不代表 Mac 原子执行或签名供应链认证完成。

## 实现与不变量

POSIX 共同验证拒绝 setuid/setgid 可执行文件，保留 owner/root、普通文件、大小、哈希、组/其他用户不可写等既有条件。原生显示 Worker 没有通过文件权限位提升权限的需求。

Mac 按规范绝对路径从根目录逐级 openat(O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)，校验每个父目录由 root/当前用户持有；拒绝普通组/其他用户可写父目录。sticky 目录允许作为共享临时根，但下一层目录及最终文件仍必须由可信 owner 持有。最终 fstatat(AT_SYMLINK_NOFOLLOW) 的 dev/inode/mode/uid 必须与已打开验证对象一致。

文件及所有祖先目录还检查扩展 ACL：先要求 fpathconf(_PC_EXTENDED_SECURITY_NP) 明确支持，再用同一描述符的 fstatx_np + filesec_query_property(FILESEC_ACL) 确认存在性。明确无 ACL 才视为空；存在时取得 ACL 并拒绝任何 allow 类型的写数据、追加、删除/删除子项、写属性/扩展属性、安全策略或 owner 变更授权，即使 qualifier 看似可信也保守拒绝。deny/read/execute ACL 不因存在而被拒绝。获取失败、未知状态、过多条目均失败关闭。

实际开发中 acl_get_fd_np 在无 ACL 的有效对象上返回 NULL/ENOENT；没有最终采用“ENOENT 就安全”的推断，改用上述成功的安全属性快照和存在性查询。相关接口来自本机 SDK sys/acl.h、sys/fcntl.h、sys/stat.h；参考 [Apple Libc 的描述符 ACL 实现](https://github.com/apple-oss-distributions/Libc/blob/main/posix1e/acl_file.c)。本项目逻辑自行实现，没有复制该源文件。

## 测试

扩展现有 WorkerRegistration 的真实文件负例：setuid、setgid、Mac 组可写目录、everyone 写文件 ACL、everyone add_file 目录 ACL；移除 ACL 后安全私有路径恢复可接受。允许安全私有目录位于系统 sticky 临时根。Mac ACL 定向测试通过（0.20 秒）。

最终 Mac 重新链接完整相关回归 118/118（18.59 秒）。Ubuntu 本阶段 POSIX 权限位变动完整回归 118/118（18.49 秒）；最终 Mac 条件 ACL 源码同步后重新构建，WorkerRegistration/WorkerExecutableBinding 2/2（1.28 秒），Mac 专属代码不在 Linux 编译。同步前比对阶段起点/阶段内上次同步版本，仅更新两个明确文件，保留远端其他修改。

Mac 在最终策略下另执行 capture、capture-cancel、capture-revoke 三项真实输出/采集场景，均退出 0，输出消失与 owner 闭合、CPU 预算归零仍成立；原始摘要、stderr 和实现 SHA256 见 [原生结果](M021-native-results.json)。这些结果不是网络媒体或可见接收呈现证据。Windows 未编译实测。

## 接入、限制与回滚

- 该策略减少其他本地用户通过可写目录/ACL 替换路径的风险；Mac 仍按路径 exec，不提供验证对象到内核执行的原子绑定。可信同用户 updater 的替换竞态、完整同用户账户攻陷、动态依赖/插件/安装签名均不由本模块解决。
- 当前开发卷根目录以及本机 /Applications 是组可写目录，会被保守策略拒绝。当前 helper 使用安全 /private/tmp/deskflow-build 路径，实际通过；后续必须为标准安装包实现明确的签名/安装路径准入策略，不能把开发验证当作默认安装目录已可用。
- 不支持/无法确认扩展安全能力的文件系统不会获得 Mac Worker 准入；没有删除 ACL、chmod 用户目录或改变系统安装权限以绕过策略。
- 文件/元数据查询和启动的硬截止风险仍在。生产媒体入口未切换；回滚本模块仅恢复先前验证策略。代码、测试和备忘录同阶段提交，git log --follow -- 本文件可定位。
