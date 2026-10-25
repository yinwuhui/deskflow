# 设备用途前端接入与后端待办

更新：2026-10-25；依据产品需求 v1.3。

## 本次前端范围

首页及建立连接流程保持原样。设备列表的远端设备详情、屏幕排列的远端设备属性新增相同的「键鼠共享 / 扩展屏幕」控件，本机不显示此控件。两处使用应用级 `PeerUsageModel`，订阅同一 `PeerUsageService`。

键鼠共享仍包含键盘和鼠标，不增加独立键盘开关，不改变原输入角色。切换无需保存排列。选择期间不修改首页角色、服务器地址或核心配置；有效模式只取后端快照。显示请求采用主窗口内非模态卡片，允许仅本次或拒绝，携带完整 session/request/generation/epoch。

控件包含提交、等待对方、准备首帧、恢复、失败、资源待清理、取消和结束扩展状态。滑块表示操作目标，状态文字区分仍生效的旧用途。不用命令 accepted 代替真实首帧。修订号倒序事件及已失效连接的命令回复不会提交到当前设备。

没有用途能力时按钮禁用，并显示明确原因；不把旧输入连接虚构为 `InputActive`。当前系统画布仍为键鼠跨屏位置，不能代表系统副屏真实布局。扩展会话或切换期间禁止编辑/保存该画布，设备选择和结束扩展仍可用。

## 已接接口

启用 `BUILD_VIRTUAL_DISPLAY=ON` 后：

- `PeerUsageService::applicationService()` 的 `execute / commandFinished / snapshotChanged`。
- `subscribePeerUsageEvents`、`startPeerUsage`、`switchPeerUsageMode`、`cancelPeerUsageOperation`、`stopPeerUsage`。
- `DisplayService::snapshotChanged` 与 `respondToDisplayRequest`。

关闭构建选项时前端仍可编译，能力保持不可用。此编译开关不代表运行设备具备平台能力。

## 必须补齐的后端接入（P0）

目前后端服务没有实际注册产品连接，因此真实应用只会显示“已连接 · 用途能力尚未就绪”。这不是 UI 扫描或按钮故障。需要完成以下链路后才能实机切换：

1. **认证连接与 UI 设备映射**：现有设备列表只有名称/地址及日志推导的连接状态。由真实认证适配器提供 `deviceKey -> peerId`，其中 peerId 为已验证的 64 位小写 SHA-256 身份。前端入口 `PeerUsageModel::applicationModel()->bindAuthenticatedDevice(deviceKey, peerId)`。deviceKey 沿用 `computer:<配置名称>` 或 `server:<归一化地址:端口>`；它仅是 UI 索引，不能作为信任凭证。同一设备若存在多个 UI 索引，需要适配器显式绑定每一个索引。断线清除映射，重连重新认证绑定。不要按名称、IP 或界面传入指纹自行认领身份。
2. **注册与既存会话同步**：真实适配器调用用途服务、显示服务的 `authenticatedPeer`，提交实际探测能力，并将既存键鼠连接同步为正确的 effectiveMode、原控制方向、焦点和权限。当前没有公开的“导入既存输入会话”接口，不能用 GUI 假完成事件代替。连接生命周期应先更新已连接设备，再注册身份映射和订阅初始快照；断线还需通知用途服务，而不仅更新 UI。
3. **显示授权控制通道**：将请求投递到对方 `incomingAuthenticated`，允许/拒绝及准备/首帧/停止确认传回对应会话。前端卡片已调用 `respondToDisplayRequest`；后端应处理过期、断线、锁屏与权限撤销，并保留完整会话代次校验。
4. **平台探测与媒体装配**：真实平台适配器更新 send/receive/network/input/focus/isolation/release 等能力，运行媒体监督器和清理检查；蓝牙内部流传递认证身份。unknown 不应改成 supported 以启用按钮。
5. **双端事件与恢复**：输入角色保持、断线撤权、应用退出、信任撤销、失败回滚、资源残留恢复需产品级贯通。目前事务单元测试通过不能替代 Mac ↔ Ubuntu 双向实机验收。

## 后续布局及窗口接口

当前 `setDisplayLayout` / `setReceiverPresentation` 返回不支持；真实布局不伪造。若需原型中的完整扩展屏位置编辑，后端至少需实现：

- 系统确认的 displayId、坐标、尺寸、缩放、主屏及可用方向；
- 携带会话代次/显示 epoch/请求 ID 的布局修改与系统确认/拒绝事件；
- 接收窗口显示、最小化/恢复、全屏与退出全屏状态、接收屏选择；
- 实际协商流配置和统计（没有测量时继续返回 null）。

界面只可在后端确认对应能力后启用这些操作；本次没有新增分辨率下拉框或虚构视频参数。

## 联调验收

先用一条已认证的现有键鼠连接完成注册及 UI 索引绑定，确认设备列表和排列页指向同一用途快照。依次检查拒绝保留原输入、取消、允许后释放输入、首帧确认、两页同步、扩展结束不自动控制对方桌面、切回键鼠保留原方向，以及断线/迟到消息/清理失败。Windows 仍需另行实机验证。
