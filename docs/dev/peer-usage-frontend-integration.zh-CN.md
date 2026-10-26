# 设备用途前端接入与后端待办

更新：2026-10-26；依据产品需求 v1.3。

## 本次前端范围

首页及建立连接流程保持原样。设备列表的远端设备详情、屏幕排列的远端设备属性新增相同的「键鼠共享 / 扩展屏幕」控件，本机不显示此控件。两处使用应用级 `PeerUsageModel`，订阅同一 `PeerUsageService`。

键鼠共享仍包含键盘和鼠标，不增加独立键盘开关，不改变原输入角色。切换无需保存排列。选择期间不修改首页角色、服务器地址或核心配置；有效模式只取后端快照。显示请求采用主窗口内非模态卡片，允许仅本次或拒绝，携带完整 session/request/generation/epoch。

控件包含提交、等待对方、准备首帧、恢复、失败、资源待清理、取消和结束扩展状态。滑块表示操作目标，状态文字区分仍生效的旧用途。不用命令 accepted 代替真实首帧。修订号倒序事件及已失效连接的命令回复不会提交到当前设备。

没有用途能力时按钮禁用，并显示明确原因；不把旧输入连接虚构为 `InputActive`。当前系统画布仍为键鼠跨屏位置，不能代表系统副屏真实布局。扩展会话或切换期间禁止编辑/保存该画布，设备选择和结束扩展仍可用。

## 前端已绑定编程接口，生产运行链路待贯通

本节表示前端已绑定服务的命令/事件契约，不表示产品连接、双端授权和媒体运行链路已经贯通，也不表示已经可以实机切换。

启用 `BUILD_VIRTUAL_DISPLAY=ON` 后：

- `PeerUsageService::applicationService()` 的 `execute / commandFinished / snapshotChanged`。
- `subscribePeerUsageEvents`、`startPeerUsage`、`switchPeerUsageMode`、`cancelPeerUsageOperation`、`stopPeerUsage`。
- `DisplayService::snapshotChanged` 与 `respondToDisplayRequest`。

关闭构建选项时前端仍可编译，能力保持不可用。此编译开关不代表运行设备具备平台能力。

## 必须补齐的后端接入（P0）

下列 P0 链路已增加后端实现及产品装配，并在 Ubuntu 验证独立产品媒体流程；不能据此宣称 Mac ↔ Linux 双机键鼠与副屏切换已验收。未完成认证握手或实际平台探测时，界面继续显示“用途能力尚未就绪”。

1. **认证连接与 UI 设备映射**：现有设备列表只有名称/地址及日志推导的连接状态。由真实认证适配器提供 `deviceKey -> peerId`，其中 peerId 为已验证的 64 位小写 SHA-256 身份。前端入口 `PeerUsageModel::applicationModel()->bindAuthenticatedDevice(deviceKey, peerId)`。deviceKey 沿用 `computer:<配置名称>` 或 `server:<归一化地址:端口>`；它仅是 UI 索引，不能作为信任凭证。同一设备若存在多个 UI 索引，需要适配器显式绑定每一个索引。断线清除映射，重连重新认证绑定。不要按名称、IP 或界面传入指纹自行认领身份。
2. **注册与既存会话同步**：真实适配器调用用途服务、显示服务的 `authenticatedPeer`，提交实际探测能力，并将既存键鼠连接同步为正确的 effectiveMode、原控制方向、焦点和权限。现通过内部 `syncConnectedPeer` 导入既存输入，不向 GUI 开放该入口。连接生命周期应先更新已连接设备，再注册身份映射和订阅初始快照；断线还需通知用途服务，而不仅更新 UI。
3. **显示授权控制通道**：将请求投递到对方 `incomingAuthenticated`，允许/拒绝及准备/首帧/停止确认传回对应会话。前端卡片已调用 `respondToDisplayRequest`；后端应处理过期、断线、锁屏与权限撤销，并保留完整会话代次校验。
4. **平台探测与媒体装配**：真实平台适配器更新 send/receive/network/input/focus/isolation/release 等能力，运行媒体监督器和清理检查；蓝牙内部流传递认证身份。unknown 不应改成 supported 以启用按钮。
5. **双端事件与恢复**：输入角色保持、断线撤权、应用退出、信任撤销、失败回滚、资源残留恢复需产品级贯通。目前事务单元测试通过不能替代 Mac ↔ Ubuntu 双向实机验收。

## v1.3 新增后端契约

这些要求属于新增后端实现，不只是既有接口接线。

- `queryConnectedPeerUsage(peerId)`：查询真实认证的既存连接、原输入角色/方向、当前用途和能力；不建立新连接，不改变原角色。
- 用途快照增加 `baseConnectionId`、`baseConnectionGeneration`（十进制字符串）、`baseConnected`、`originalInputRole`（source/receiver/unknown）、`originalInputDirection`（localToPeer/peerToLocal/unknown）。设备身份可跨连接保持，连接 ID 和代次用来隔离断线重连后的旧事件。
- 认证适配器提供内部既存会话同步入口，提交完整输入权限、真实路由是否有效、统一焦点及连接代次。GUI 不得设置 effectiveMode 或调用成功回调导入状态。
- `retryPeerUsageCleanup(peerId, operationId)`：统一重试用途清理，自动选择输入撤权/释放确认或显示资源清理；不建立新用途、不重新使用旧显示授权、不触发自动回滚激活。
- 清理后的状态、原方向、可用动作和错误原因均通过统一快照发布。输入客户端和服务端两种原角色都应覆盖；不支持的方向在切换前返回明确原因。
- `startPeerUsage`、`switchPeerUsageMode`、`cancelPeerUsageOperation`、`stopPeerUsage`、`retryPeerUsageCleanup` 均必须携带基础连接 ID/代次；缺失或过期即拒绝。运行期状态事件关联基础连接 ID/代次，旧连接断线和晚到命令不能改变新连接。切换时基础认证关系保留，输入路由与附加视频会话分别管理。

## 显示布局、接收窗口与流配置接口

以下接口已增加后端运行实现，均通过 `DisplayService::execute / commandFinished / snapshotChanged` 使用。GUI 不能提交原生显示器 ID、私有控制路径、授权令牌或伪造适配器完成事件。

除无参数的能力/快照查询外，以下 `args` 必须包含 `sessionId`、`requestId`（32 位小写十六进制）、`generation`（十进制字符串）及 `displayEpoch`（正整数）。迟到的系统回调只能完成原事务，不能更新新会话。

| operation | 额外 args | 适用条件与确认 |
| --- | --- | --- |
| `queryDisplayLayout` | 无 | Host、Streaming、layout 能力有效；读取当前真实拓扑 |
| `setDisplayLayout` | `layout: {x, y, scale?}` | 同上；x/y 为 ±32768 内的整数逻辑坐标；scale 首版只允许与系统现值相同 |
| `presentReceiverWindow` | 无 | Receiver、Streaming；显示并恢复窗口，等待实际状态确认 |
| `setReceiverPresentation` | 必填 `presentationMode: window/fullscreen`；可选 `receiverScreenId`、`windowState: normal/minimized`、`scaleMode: fit/actual` | Receiver、Streaming；仅允许枚举到且唯一的屏幕，最小化需 minimize 能力 |

布局成功回复的 `data` 和快照 `confirmedLayout` 为同一对象：

```json
{
  "coordinateSpace": "primary-relative-logical",
  "layout": {"displayId":"实际虚拟屏 ID","x":1920,"y":0,"width":1920,"height":1080,"pixelWidth":1920,"pixelHeight":1080,"scale":1,"primary":false,"owned":true},
  "displays": [],
  "capabilities": {"position":true,"scale":false}
}
```

`displays` 包含所有系统确认的显示器，字段与 `layout` 一致。原点是当前主屏左上角，左侧/上方可为负坐标。Linux 适配器在 Mutter 的非负坐标空间中整体平移原点，保留其他显示器的相对几何位置；只修改当前持有的虚拟屏位置，不修改键鼠排列配置。系统拒绝重叠、断开的拓扑或其他无效请求时返回 `LayoutRejected`，前端保留最近确认布局。布局写操作串行，重复提交返回 `PeerBusy`。

实现路径：GNOME/Mutter `ApplyMonitorsConfig` 与实际状态回读；macOS 使用当前持有的 `CGVirtualDisplay` 和 CoreGraphics 配置回读。命令由持有资源的原生进程处理，清理后不能凭旧显示器名称继续修改布局。macOS 已编译验证，当前 Mac 无可用图形会话，尚未完成原生布局运行验收。

接收窗口快照 `receiverWindow` 回传实际 `visible`、`minimized`、`fullscreen`、`receiverScreenId`、`availableScreens`、`scaleMode`；`presentationMode` 和 `receiverScreenId` 同步更新。默认 fit 保持画面比例；actual 按接收屏物理像素显示，超出窗口时可滚动查看，不重建虚拟屏。窗口操作异步确认，失败返回 `PresentationFailed`。Ubuntu 默认使用 XWayland 接收窗口，以获得可确认的最小化/恢复；显式选择原生 Wayland 时遵循实际平台能力，未确认支持的最小化返回不可用。

`streamConfig` 来自实际媒体 CONFIG 协商，首版固定 H.264、1920×1080、目标 30fps、SDR、无输入回传。`fps` 是协商目标，不是测得呈现帧率；`statistics` 尚无可靠测量，保持 null。动态分辨率、缩放配置、接收端输入和自动重连仍为 P1，不向 UI 声称可用。

能力准入和真实系统排列画布是两层工作：现有键鼠排列画布仍不能直接代表系统显示拓扑。前端需消费上述确认结果后才能开放扩展布局编辑。

## 联调验收

先用一条已认证的现有键鼠连接完成注册及 UI 索引绑定，确认设备列表和排列页指向同一用途快照。依次检查拒绝保留原输入、取消、允许后释放输入、首帧确认、两页同步、扩展结束不自动控制对方桌面、切回键鼠保留原方向，以及断线/迟到消息/清理失败。Windows 仍需另行实机验证。

## 本次后端交付范围

- 已认证 TLS/蓝牙隧道将实际认证身份绑定到本进程的回环套接字；核心查询真实连接，不使用日志中的名称/IP 认领身份。
- 核心通过 `peerUsageSnapshot` 向 GUI 提供连接、原角色、输入权限和实际路由状态；源端路由状态通过认证输入流同步至原接收端。
- 显示请求、允许/拒绝、准备及结束消息通过既存认证流传输；接收方完成输入释放后才确认显示允许，发送方获得系统捕获权限并停止输入后才创建副屏。
- `ProductDisplayRuntime` 装配平台探测、证书绑定的 LAN 媒体端点和 `MediaSupervisor`。媒体首帧确认来自真实原生窗口及认证媒体对端；没有以请求 accepted 代替首帧。
- 正常断线带有核心本地输入撤权结果；异常核心退出缺少验证时继续保留待清理状态，不把进程消失视为资源清理成功。
- 启动时发现持久化显示资源记录，会恢复统一用途清理状态，并在主窗口提供清理重试入口；不要求设备在线。该记录只用于清理，不能恢复显示授权。

布局、接收屏选择、全屏、最小化/恢复已增加异步编程接口和实际状态回传；统计继续为空，不伪造帧率或延迟。完整验证范围见 [v1.3 后端验证记录](peer-usage-v13-backend-validation.zh-CN.md)。

## 仅蓝牙连接策略限制（2026-10-26）

仅蓝牙是后端强制策略，LAN 实际可达也不能绕过。共享端只开放蓝牙时禁止扩展；接收键鼠端以蓝牙为初始连接且关闭网络备用时也禁止扩展。明确允许网络备用、当前输入恰好走蓝牙时，仍可进行显示网络能力探测。

- 连接快照 `displayNetworkPolicy`：`networkAllowed` / `bluetoothOnly`；内部未知策略保持不可用。
- `inputCapabilities.displayNetworkAllowed`：双方是否已明确允许网络媒体，与实际 `networkAvailable` 分开。
- `inputCapabilities.displayUnavailableReason`：策略禁止时为 `BluetoothOnlyMode`，策略未就绪时为 `CapabilityCheckRequired`。
- 禁止时 `startPeerUsage`、`switchPeerUsageMode`、内部既存连接上的接收请求及下层显示服务都拒绝；拒绝发生在授权和输入释放前，不影响既存键鼠。
- 前端禁用扩展按钮，并显示“仅蓝牙模式无法使用扩展屏幕，请先启用网络连接”。直接调用 API 同样不能绕过。
- 策略在等待授权阶段被撤销时，取消请求并保留原输入；已进入资源准备/扩展时执行停止与清理，禁止自动恢复输入。重新启用网络仅重新探测能力，不自动开始扩展。

策略来自认证核心观察和对端认证 hello，不接受 GUI 在开始命令中覆盖。hello 在仅蓝牙时不发布视频网络地址，媒体不会借用键鼠备用切换策略自动降级为蓝牙。

本次修改解决连接策略准入。原有 LAN 地址匹配尚不能替代真实媒体端点/TLS 连通性预检，该独立限制没有因本次策略校验而消失。
