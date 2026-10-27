# 设备屏幕信息同步

## 行为与边界

屏幕信息属于已认证设备元数据，独立于键鼠 source/receiver 角色及视频会话。认证连接后双方自动交换；局域网、仅蓝牙连接都可传输这些小消息。不创建虚拟屏、不捕获画面、不申请录屏或辅助功能权限、不建立媒体连接。网络视频仍遵守原有仅蓝牙禁用策略。

设备详情默认展示显示器列表、主屏、像素尺寸、逻辑尺寸、缩放比例、方向、刷新率和系统报告的物理毫米尺寸。macOS 优先按逻辑几何匹配 CoreGraphics 当前显示像素模式；GNOME 优先异步读取 DisplayConfig 的当前模式和逻辑缩放；不可用时像素尺寸来自 Qt geometry × devicePixelRatio，明确标为系统换算值，不能冒充所有平台的精确硬件原生模式；物理尺寸未经校准，缺失则显示未知。不会传输原始 EDID 或显示器序列号。

对端 4K 显示器和当前媒体只支持 1080p30 分开呈现。本次未引入动态分辨率、HiDPI 或物理坐标跨屏校准，不改变现有系统布局写入接口。

用户可在设备详情保存下次接收画面使用的屏幕，以及窗口/全屏偏好；默认自动选择主屏、窗口显示。偏好不启动会话，也不改变正在进行的会话。指定屏幕缺失时明确显示不可用，不静默选择其他屏幕；用户可改回自动。屏幕排列仍通过现有排列入口完成。

## 同步协议

复用 CoreInputChannel 的已认证控制流；独立协议 `device-screens/1`，类型 `request` / `inventory`。每次新认证基础连接发布本机清单并请求对端清单，避免首次交换时序丢失；不向未认证发现广播发布屏幕清单。

inventory 字段：schemaVersion=1、instanceId（32 位随机十六进制，每次进程启动更换）、revision（正整数十进制字符串）、screens 数组。每块屏幕包含：

- screenId：名称、制造商、型号、序列号的本机 SHA-256 标识，不发送原始序列号；标识在系统提供的信息稳定时可跨重启保持，无法保证硬件级永久唯一。
- name、primary、orientation；logicalWidth/Height、x/y 为对端自身桌面的逻辑坐标，不代表两台电脑在桌面上的实际摆放关系。
- pixelWidth/Height、pixelSizeSource=coreGraphicsDisplayMode、gnomeDisplayConfig 或 qtGeometryTimesDpr；scale 是系统报告的像素比例。
- refreshRate：Hz，可为 null；physicalWidthMm/physicalHeightMm 可为 null；physicalSizeSource=systemReportedUnverified。

清单最多 16 块、14 KB，名称最多 128 字符，尺寸/比例/坐标均有界；拒绝重复 ID、异常数值、控制字符及额外描述字段。消息绑定 peerId 和当前 baseConnectionId，旧连接不能刷新信息。同一进程同一连接内忽略不递增的清单修订号。

订阅 QGuiApplication/QScreen 的插拔、几何、方向、DPI、刷新率等变化，200ms 合并通知，仅在清单实际变化时递增 revision 并推送，不依赖视频会话或高频轮询。

## 缓存与接口

ScreenInventoryService 是同进程应用接口，源文件位于 src/lib/display/ScreenInventoryService.*：

- localInventory()：本机清单。
- peerInventory(peerId) / forDevice(deviceKey)：inventory、peerId、fresh、lastSeenAt、preference、supportedStreamMode。
- setPreference(peerId,screenId,presentationMode)：仅允许在线且清单新鲜；空 screenId 表示自动主屏，presentationMode 为 window/fullscreen。
- selectionForPeer(peerId)：当前清单及偏好对应的会话候选；清单过期、指定屏幕缺失返回空。
- changed()：驱动详情自动更新。

信息按认证身份保存在用户配置目录 screen-inventory.ini。lastSeenAt 使用接收端本机 UTC 时间；断线或应用重启后的磁盘缓存始终 fresh=false，界面明确显示“上次连接”和时间，缓存不能授权启动。设备名称只用于查找缓存展示，不能替代身份或连接校验。

## 会话确认

发起拓展时，usage-display/1 的 request 携带 screenSelection：instanceId、revision、screenId、presentationMode、streamMode。streamMode 仍固定 1920×1080、30fps、H.264、SDR。

接收端在展示授权请求前检查选择与当前清单一致；用户允许后、创建媒体接收资源前再次读取屏幕并验证修订号。过期返回 CapabilityCheckRequired。清单不会替代既有权限、桌面锁定、网络、对端同意和资源检查。

目标屏幕只解析为本机窗口目标，不能指定可执行文件或任意路径；窗口启动时再次检查目标存在及名称唯一。失败返回不可用，不静默落到其他显示器。

## 验证

ScreenInventoryTests 覆盖：仅蓝牙元数据双向交换、初次握手、设备映射、默认主屏、偏好保存、旧修订号拒绝、插拔变化推送、重复/超长描述拒绝、错误连接隔离、离线缓存不能启动及重启后缓存过期。

DeviceScreenInfoTests 覆盖：未开启媒体即可展示 4K 屏幕信息、同时标示 1080p 媒体上限、文本按纯文本渲染、偏好回显、断线后缓存标签及禁用配置。

接收窗口和只读能力探测沿用 GUI 的 Qt 平台后端，避免 Wayland 清单与 X11/XWayland 窗口的屏幕名称和坐标不一致。窗口能力按实际后端探测；Wayland 不保证的最小化能力不会冒充已支持。

## 本次验证结果（2026-10-27）

- Ubuntu GUI、核心和接收窗口构建成功，91 项 Display/用途/设备详情相关 CTest 全部通过。
- Mac GUI、核心和接收窗口编译成功；签名预览包沿用原开发证书和三项 QtBluetooth 修补。解锁后已更新原路径的运行预览包，辅助功能授权继续有效。
- 在没有开启拓展会话的情况下，经真实已认证控制连接请求并收到 Ubuntu 清单：eDP-1、2560×1600、120 Hz、逻辑尺寸1536×960、缩放约166.7%。GNOME GetCurrentState 的原生模式查询与清单一致。
- Qt 在该分数缩放环境下的 geometry × DPR 会得到3072×1920；已验证 GNOME 原生模式优先修正，未将估算冒充真实分辨率。
- 元数据仅蓝牙传输、缓存、旧连接/修订号、拔屏拒绝及偏好在自动化测试中通过。本次未新增真实屏幕热插拔、接收目标切换后的媒体首帧或跨屏鼠标体验验收结论。

### 解锁后的实机检查

Mac 设备详情已在未开启拓展时显示 Ubuntu 的2560×1600、166.7%缩放、120Hz和物理尺寸；Ubuntu 也收到并缓存 Mac 的 T2752U（3840×2160）及 ToDesk 屏幕信息。一次检查期间只有蓝牙活动，信息同步正常，证明不依赖视频链路。

全屏复选框的辅助功能操作发现仅更新勾选、不保存的问题；已改为响应 toggled，并在实机看到“已保存，将在下次会话生效”。验证后恢复窗口显示偏好，没有发起拓展会话。

目标屏幕选择过程中发现同步刷新重置下拉框模型的重入问题并发生崩溃；已修复为仅在选项实际改变时更新，弹出列表可见时延后更新。新增回归断言：选择目标和修改全屏偏好均不触发模型 reset。修复版已更新，最新91项定向测试全部通过。

随后 Mac 再次锁屏，目标下拉框修复后的最终实机操作仍待再次解锁验收，不能将自动化通过解释为该崩溃已完成实机复验。

### 最终菜单控件实机复验

再次解锁后，原 QComboBox 在辅助功能键盘操作时仍发生 Cocoa 访问异常，说明仅避免模型 reset 不足以修复该系统环境的问题。最终将目标屏幕选择替换为 QToolButton + QMenu，保存流程不重建未变化的菜单项，并保持控件对象稳定。

Mac 实机已反复打开菜单，分别选择自动主屏和 eDP-1，确认配置文件中的 screenId 在空值与指定标识间正确变化，界面显示已保存；原操作路径不再导致崩溃。验证后恢复自动主屏、窗口显示。期间扩展会话始终未启用，设备连接保持正常。已完成本功能的界面实机复验，但不扩大为媒体首帧或真实热插拔已验收。

新增单测验证选择菜单操作确实保存目标、辅助功能式复选框状态变化确实保存全屏偏好，以及保存过程中现有菜单项对象不会被重建。

## 设备详情展示优化（2026-10-27）

使用模式优先于连接信息和屏幕信息展示。屏幕采用与连接信息一致的浅色圆角卡片，默认只展示友好名称、主屏标记、分辨率和刷新率；Qt 换算的分辨率保留“系统估算”提示。系统名称、缩放、逻辑尺寸、方向及可用物理尺寸在详情中展开。物理尺寸校准说明放在提示中。

接收屏幕、窗口/全屏和简短画面规格归入扩展模式设置，仅在当前或正在请求扩展模式时显示；设置仍仅影响下次会话，不自动启动、停止或改动现有会话。H.264 SDR 和画面规格上限放在信息提示中。菜单与卡片对象在无清单变化的刷新中保持稳定，离线保留卡片、显示“上次连接”和缓存时间，偏好控件禁用。
