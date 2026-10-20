# macOS 与 Linux 双机联调

2026-10-20：用户确认第二台电脑为 Linux。已通过用户授权的SSH连接Ubuntu 26.04.1 LTS（x86_64），GNOME 50.1／Wayland。已完成环境采集，正在独立构建；其余用例仍待执行。凭据不写入仓库。

## 环境与构建

在 Linux 桌面终端、当前源码根目录运行 `bash tools/testing/linux-preflight.sh`。脚本只读取系统／会话／开发库版本及 Portal 接口，不安装软件、不启动共享、不申请控制权限。SSH shell 可能缺少桌面会话环境；Portal 信息须以图形桌面内的结果为准。

必须使用本次修改后的源码。上游已发布的 Linux 包可作兼容性对照，不能代替本轮新版 GUI／核心验收。依赖以仓库 CMake 为准：Qt6.7+、OpenSSL3+、libei1.3+、libportal0.9.1+、xkbcommon／xkbregistry，以及默认开启的 X11 开发库。发行版确定后再给出对应安装命令；构建依赖版本不等于桌面 Portal 已实现全部能力。

依赖就绪后的独立构建目录：

```sh
cmake -S . -B build-linux-review -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_INSTALLER=OFF \
  -DBUILD_TESTS=ON -DSKIP_BUILD_TESTS=ON
cmake --build build-linux-review --parallel 4
build-linux-review/bin/deskflow-core --version
```

构建结果和 Linux 可执行文件名以生成输出为准。先保留构建日志及源码摘要，再启动 GUI；不要以 root 启动桌面共享，也不要强制设置 `QT_QPA_PLATFORM=xcb` 来掩盖 Wayland 后端问题。

## 实测顺序

| 项目 | 操作与通过条件 | 当前结果 |
|---|---|---|
| 环境 | 记录 Linux 发行版／架构／桌面／X11或Wayland／Portal版本；两端源码与版本可追溯 | 待执行 |
| 首次连接 | 先 Mac 提供键鼠、Linux 接收；发现失败可用手动地址，核对实际监听端口（默认24800）和双方完整指纹 | 待执行 |
| 身份保护 | 拒绝核对不建立可输入连接；重新连接、身份变化和撤销均显示实际结果，不自动放宽安全要求 | 待执行 |
| 输入 | 跨屏移动、左右键、滚轮、拖动、中文／英文、组合键；两端仅在本地空白编辑器中测试；人工确认后才显示输入已验证 | 待执行 |
| 剪贴板 | 双向唯一测试短文本、换行、Unicode；关闭共享后不再同步，重新开启验证；不使用隐私内容 | 待执行 |
| 暂停 | 按住普通键／修饰键时暂停后无卡键，远端输入与剪贴板停止；恢复状态和实际行为一致 | 待执行 |
| 布局 | 多行位置、保存／取消、别名及快捷键；新增设备应用不虚报已连接 | 待执行 |
| 换来源 | Mac改为接收Linux；连接成功后，候选地址不可达／拒绝／取消分别验证旧来源恢复及失败报告；切换允许短暂中断 | 待执行 |
| 生命周期 | 暂停后睡眠／唤醒、网络中断重连、关闭窗口与明确退出；托盘不可用时可返回窗口 | 待执行 |
| Wayland专项 | 分别验证接收RemoteDesktop和提供InputCapture的实际授权、拒绝、撤销、再次尝试；剪贴板／全局快捷键按实际Portal反馈记录 | 待执行 |

来源替换涉及“旧来源”和“新来源”时，只有两台电脑不能覆盖两台真实来源的成功切换；可以覆盖候选不可达和旧来源恢复，完整成功切换场景还需要第三台来源设备。不要为了凑齐用例把同机模拟会话记为真实双来源验收。

## 记录规则

每项记录两端角色、时间、操作、预期、实际、日志／截图与是否复现；失败保留现场配置和日志，经诊断报告预览去除敏感内容再共享。证书私钥不加入报告。系统授权弹窗和真实键鼠感受需用户在两台物理电脑前配合；SSH构建成功无法证明这些项目通过。

## 本次实测环境与处理

- GCC15.2、CMake4.2.3、Qt6.10.2、OpenSSL3.5.5、libei1.5.0；xdg-desktop-portal1.21.1、GNOME Portal50.0。读取桌面DBus得到RemoteDesktop v2、InputCapture v1、Clipboard v1；接口存在不等于授权或实际传输通过。
- 原Deskflow GUI和客户端进程保留，构建在用户目录 `deskflow-review-20261020`，未覆盖安装包或真实配置。
- 通过apt新增libportal-dev、libei-dev、libxkbregistry-dev、meson及依赖gir1.2-xdp-1.0；未做系统整体升级。
- 系统libportal0.9.1缺少剪贴板API；从上游0.10.0发行包在 `deps/prefix` 独立构建，不替换系统库。归档SHA-256：`1c74a3f7becbd4ff745d7f565afd06e97e50eb080a0e268cb14bb55289a745bf`。重新配置确认剪贴板API与InputCapture恢复API可用，全局快捷键API仍不可用。上游[0.10.0发布说明](https://github.com/flatpak/libportal/releases)说明新增剪贴板支持。
- 首次Linux编译发现Xlib宏与App.h中Qt JSON头冲突；已将相关Qt头置于Xlib之前继续构建。同时修正Linux设置助手将未知授权写成“权限检查完成”的文案，改为等待后端授权及输入验证。

## 用户自行联机测试前的状态

用户已要求停止联机测试：没有替换已有应用、没有切换现有共享连接。仅在独立目录同步和编译最终代码。较早快照的Linux39个隔离CTest目标已通过（17.43秒），不等于双机输入／剪贴板通过，也不等于后续全部收尾代码已在Linux回归。

Ubuntu已有构建目录：`/home/ywh/deskflow-review-20261020/build-linux-review`。用户稍后运行新版前，应先正常退出旧Deskflow，保存需要的草稿；旧应用和正常配置保持原状。需要隔离试用时可在桌面终端为新进程设置 `XDG_CONFIG_HOME` 和 `XDG_STATE_HOME` 到独立测试目录；不要将旧的信任确认或测试fixture当作新对端授权。

最终源码已于本轮在Ubuntu成功编译（GUI、核心及测试可执行文件）；最终编译日志为Linux构建证据/build-linux-final.log。未启动新版共享，也未再运行Linux回归测试。
