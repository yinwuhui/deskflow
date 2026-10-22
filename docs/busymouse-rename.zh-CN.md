# busymouse 改名说明

本分支的应用名、GUI 可执行文件和安装包前缀为 `busymouse`，核心进程为
`busymouse-core`，Windows 服务进程为 `busymouse-daemon.exe`。
应用标识为 `org.busymouse.busymouse`。原有应用图标继续使用；安装器采用不含上游名称的默认背景。资源文件和 C++
命名空间中的 `deskflow` 是内部名称。

## 构建

使用新的构建目录，避免旧 CMake 缓存和 Deskflow 程序混在一起：

```sh
cmake -S . -B build/busymouse -DCMAKE_BUILD_TYPE=Release
cmake --build build/busymouse --parallel
```

macOS 的本地预览版本可通过 `-DBUSYMOUSE_GUI_BUNDLE_ID=org.busymouse.busymouse.preview`
使用独立标识。macOS 产物为 `busymouse.app`。修改应用路径、标识和签名后，
需要在目标电脑上检查辅助功能、输入监控和本地网络授权。默认本地包使用临时签名，
不启用 Hardened Runtime；配置签名证书时仍启用该功能。

## 旧配置迁移

真正的 GUI、核心和 Windows 服务首次初始化设置时，会尝试把同级 `Deskflow`
配置目录复制到 `busymouse`。支持用户目录、系统目录和 `XDG_CONFIG_HOME`。
旧文件保留；目标目录已经存在时不会覆盖或合并。迁移先在临时目录完成，成功后再改名，
避免留下半份配置。含符号链接的旧目录需要手动迁移。

迁移包括配置、TLS 私钥和证书、信任指纹数据库，以及同目录的服务端配置。
配置文件改为 `busymouse.conf`，配置中的目录内绝对路径会同步调整；自定义外部路径保留。
旧的证书文件名 `deskflow.pem` 可以继续使用，以保持设备身份。
状态文件复制为 `busymouse.state`，已有状态文件不会覆盖。
Windows 便携模式额外支持从同目录 `settings/Deskflow.conf` 复制设置。

旧版自动启动项若符合应用生成的格式，会迁移到新可执行文件，并删除旧登记；
已有 busymouse 启动项不覆盖。自定义启动项和不同预览标识的启动项需要自行检查。
Windows 使用独立的 MSI Upgrade GUID，不会把上游 Deskflow 当成同一产品卸载升级。

## 兼容与更新

Synergy / Barrier 握手协议、mDNS 服务类型和蓝牙 UUID 保持原值，继续与现有设备互通。
上游版权、贡献者记录和帮助链接保留。`busymouse.org` 仅作为 Qt 组织标识使用，
不会据此生成网络请求；没有假设该域名提供服务。

本分支尚无更新服务。新安装默认关闭更新检查，迁移时清空原 Deskflow 更新接口。
将来配置自己的 `gui/updateCheckUrl` 并启用检查即可使用独立更新服务。
CI 的包名、应用路径、Flatpak 标识和测试命令已同步；上游 Homebrew 更新通知已移除。

## 本次验证

- macOS arm64：Qt 6.8.3、macOS 15.2 SDK 编译成功；蓝牙功能开启。
- 配置迁移、设置、翻译、设备列表、主窗口和启动项的六组相关测试全部通过。
- 全量 39 组测试中，38 组已通过；`OSXKeyStateTests` 的模拟按键检查在改名前的
  已编译版本也失败，未在本次改名中调整键盘逻辑。
- 完整应用的 GUI 和核心 `--version` 命令通过，使用临时配置验证了首次启动迁移，
  包括原证书和信任记录的保留、状态文件复制和上游更新检查关闭。
- Windows / Linux 的构建与打包配置已同步，尚未在对应系统实机验证。
