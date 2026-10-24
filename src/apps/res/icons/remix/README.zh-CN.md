# 应用界面图标

使用 Remix Icon 线性系列（Apache-2.0），原始素材从 iconfont 获取。`manifest.json` 保存图标 ID、来源及与 Remix 4.5.0 官方图形的核对结果；`LICENSE` 为该版本上游许可文本。图标颜色属性经过统一，蓝牙关闭图标为加斜线的本地派生版本。

- `svg/`：58 个可编辑源图标。
- `light/`、`dark/`：预生成 32、64、128 像素 PNG 及主题索引。
- `mapping.json`：HomeIcon 和 Qt 主题名称到 Remix 图标的映射。

`deskflow.qrc` 将主题名称注册为 `deskflow-light`、`deskflow-dark`，保留调用方使用的标准 Qt 图标名称。HomeIcons.h 使用 128 像素 PNG 的透明度遮罩着色，提供 1x/2x 图标，不依赖 SVG 插件。应用 Logo 和托盘品牌图标保持原资源。

重新生成 PNG：安装开发依赖 `sharp` 后，在项目根目录执行 `node tools/generate-ui-icons.cjs`。开发环境也可通过 NODE_PATH 指定已有 sharp 包目录。运行或编译应用无需 Node.js 或 sharp。

新增图标时同时更新 SVG、映射、deskflow.qrc（主题别名及遮罩），执行生成脚本，并运行 IconTests。仅修改 mapping.json 不会自动修改 C++ 或 Qt 资源清单。

蓝牙状态：通用蓝牙符号表示未连接或未知；两侧圆点表示应用蓝牙通道 active/ready；斜线仅表示适配器关闭。连接状态来自核心进程快照，不由图标推测，原有文字提示保留。

左侧四个导航项使用成对的 Remix line/fill 图标：未选中为灰色线性，选中为蓝色实心。由按钮 checked 状态选择 QIcon::Off/On，鼠标点击和程序导航行为一致。
