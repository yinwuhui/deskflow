# Ubuntu 远程开发

本项目后续默认使用以下 Ubuntu 环境开发和验证。

| 项目 | 配置 |
| --- | --- |
| SSH | `ywh@10.20.1.71` |
| 仓库 | `/home/ywh/code/github/deskflow` |
| 系统 | Ubuntu 26.04.1 LTS，x86_64 |
| 编译器 | GCC 15.2.0 |
| 构建工具 | CMake 4.2.3、Ninja 1.13.2 |
| Qt | 6.10.2，含 Bluetooth |
| 系统库 | OpenSSL 3.5.5、libei 1.5.0、libportal 0.9.1 |

Mac 上的 Windows App 已连接该主机，可以用于观察桌面程序。命令行开发通过 SSH 进行。

## 连接与构建

```sh
ssh ywh@10.20.1.71
cd /home/ywh/code/github/deskflow
git status --short

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DBUILD_BLUETOOTH=ON
cmake --build build --parallel 4
```

保留调试符号，启用 X11、Wayland 和蓝牙后端。默认构建包含单元测试，并在构建末尾自动运行。
根据主机剩余内存调整并发；初次验证使用 4 路，内存紧张时降至 2 路。

单独运行测试：

```sh
QT_QPA_PLATFORM=minimal ctest --test-dir build/src/unittests --output-on-failure
```

GUI 与核心程序位于 `build/bin/busymouse` 和 `build/bin/busymouse-core`。
在 Ubuntu 桌面的终端中运行 `./build/bin/busymouse` 可查看界面。
系统已有 `/usr/bin/deskflow` 运行，验证本项目时应确认启动的是 `build/bin/busymouse`。
编译数据库位于 `build/compile_commands.json`。
首次构建日志保存在 `build/configure.log` 和 `build/build.log`。

## 系统依赖

```sh
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  qt6-base-dev qt6-tools-dev qt6-connectivity-dev \
  libssl-dev libx11-dev libxtst-dev libxkbfile-dev libxinerama-dev \
  libxrandr-dev libxi-dev libxkbcommon-dev libxkbregistry-dev \
  libei-dev libportal-dev help2man
```

此次补装了 `qt6-connectivity-dev`（及其运行库）和 `help2man`，其余构建依赖原已安装。

## 首次验证结果

2026-10-23，提交 `e59626e66a21ba2b4ad025255e994d8b2791847e`：

- 完整构建成功，42 个 CTest 测试全部通过。
- GUI 与核心程序版本均为 `1.27.0.9999 (e59626e6)`，动态库无缺失。
- SSH 会话没有桌面显示环境，版本验证也需要为核心程序设置 Qt 后端：

```sh
QT_QPA_PLATFORM=minimal ./build/bin/busymouse --version
QT_QPA_PLATFORM=minimal ./build/bin/busymouse-core --version
```

上述验证覆盖编译、单元测试和程序加载；桌面交互、跨机器键鼠共享及蓝牙硬件连接仍需实际场景验证。

## 修改与同步

以远端工作区为后续开发目标，开始前检查远端 `git status`。不要直接将 Mac 整个目录覆盖到 Ubuntu，
避免丢失远端未提交改动；不要同步 Mac 的构建产物到 Ubuntu。需要回传时仅同步明确修改过的源码或文档。
认证信息不写入仓库、脚本或本文档。
