# 开发环境与审计

按用户最新要求（2026-10-27，覆盖此前 Ubuntu 主开发安排）：

- 当前 Mac 是主开发、编译与测试环境。
- Mac 仓库：`/Volumes/KINGSTON 250G/Code/deskflow`。
- Ubuntu 用于集成、双机与恢复验证：SSH `ywh@10.20.1.71`，仓库 `/home/ywh/code/github/deskflow`。
- 同步前检查远端工作区，仅同步明确的本阶段文件/补丁，保留远端已有修改。两端基线提交可能不同，不通过 reset 覆盖彼此历史。
- 不在仓库或脚本中保存登录密码。
- Ubuntu 的既有构建命令见 [Ubuntu 远程开发](docs/dev/ubuntu-remote-development.zh-CN.md)。

# 拓展屏底层重构

- 严格按 `docs/dev/extended-display-architecture/architecture.zh-CN.md` 实施。用户允许先只在 Mac、Ubuntu 测试；不能把 Windows 未编译/实测代码标为已认证。
- 用户要求每完成一个子模块编写备忘录，记录边界、设计对应、实现、不变量、实际测试、已知限制、未接入项与提交定位。索引：`docs/dev/extended-display-architecture/memos/README.md`。
- 备忘录和该模块代码/测试应在同一阶段提交。基础单元测试、真实子进程、媒体样机、真实桌面呈现和跨机验收必须分别记录，不相互替代。
- 当前基础设施仍在分阶段开发；以备忘录索引记录的生产接入边界为准，不把独立库或实验工具当成全部重构已完成。
