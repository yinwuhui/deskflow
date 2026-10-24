# 项目图标素材包

2026-10-24 从 iconfont 获取 57 个 Remix 线性图标，另制作 1 个明确标注的蓝牙关闭派生图标。所有 SVG 都使用 currentColor，统一 24×24 输出尺寸；保留来源 viewBox，避免破坏图形比例。应用品牌 Logo 不在替换范围。

## 文件

- `svg/`：58 个 SVG 素材。
- `preview.png` / `preview.html`：深浅背景预览。
- `manifest.json`：每个素材的 iconfont ID、检索链接及与 Remix 4.5.0 官方图形的像素对比结果。
- `mapping.json`：现有 26 个 HomeIcon、通用主题图标及蓝牙状态的用途映射。
- `LICENSE`：Remix 4.5.0 官方 npm 包的 Apache-2.0 许可。

## 来源与风格核对

主要来源：https://www.iconfont.cn/collections/detail?cid=50344 （页面标注开源、Apache-2.0）。该集未包含的系统操作图标，从 iconfont 同名结果中选择与 Remix 4.5.0 官方形状相符的图标，未采用仅名字相同而图形不同的结果。

图标数据来自 iconfont 公开检索及图标集接口。上游 npm 包 https://registry.npmjs.org/remixicon/-/remixicon-4.5.0.tgz 仅用于形状核对和获取许可文本；素材 SVG 来自 iconfont。下载后移除网页展示样式并统一颜色属性。

## 蓝牙状态

- 已连接：bluetooth-connect-line（两侧连接标记）。
- 未连接/已断开：bluetooth-line（通用蓝牙符号），必须同时显示状态文字。
- 适配器关闭：bluetooth-disabled-line（在下载的蓝牙图标上添加斜线，属于本地修改）。
- 正在连接：蓝牙图标配合 loader-4-line 或状态文字；静态资源本身不含动画。

## 接入状态

素材已接入本地项目。运行时文件位于 `src/apps/res/icons/remix/`，主题别名已注册到 `deskflow.qrc`，首页图标通过 HomeIcons.h 读取 PNG 遮罩着色，蓝牙状态根据实际核心进程通道和适配器状态切换。应用品牌资源保留。本目录继续作为素材盘点与预览，运行时不依赖 output 目录。

导航增加 computer-fill、layout-left-fill、layout-grid-fill、settings-3-fill 四个同系列实心图标，与原线性版本组成未选中/选中状态。
