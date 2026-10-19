// SPDX-License-Identifier: MIT
// Run from this design directory after npm install && npx playwright install chromium.
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const path = require('node:path');
const {pathToFileURL} = require('node:url');
(async()=>{
 const browser=await chromium.launch({headless:true,...(process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE?{executablePath:process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE}:{})});
 const page=await browser.newPage({viewport:{width:1280,height:1000}}),errors=[],network=[];
 page.on('pageerror',e=>errors.push(e.message));page.on('console',m=>{if(m.type()==='error')errors.push(m.text());});
 page.on('request',r=>{if(/^https?:/.test(r.url()))network.push(r.url());});
 const results=[];const check=(id,ok)=>{assert.ok(ok,id);results.push(id);};
 const state=()=>page.evaluate(()=>window.DeskflowPrototype.getState());
 const click=id=>page.locator('#'+id).click();
 const route=async v=>{await page.selectOption('#ac-jump',v);};
 const event=async v=>{await page.selectOption('#review-event',v);await click('review-emit');};
 try{
 await page.goto(pathToFileURL(path.resolve(__dirname,'../原型/index.html')).href);
 await page.waitForFunction(()=>!!window.DeskflowPrototype);
 check('P01 初始设备与状态', (await state()).nodes.length===6 && await page.locator('#app-title').innerText()==='Deskflow · 已连接 1 台电脑');
 const views=await page.locator('#ac-jump option').evaluateAll(a=>a.map(x=>x.value));
 for(const v of views){await route(v);check('PAGE '+v,await page.locator('#ac-main h2').count()>0);}
 await click('review-reset');await click('global-add');await click('setup-permission-next');await event('incoming');
 check('P02 来源端等待接收端请求',(await state()).setup.stage==='trust');
 await click('setup-trust');check('P03 指纹未确认不可继续',(await state()).setup.stage==='trust');
 await page.check('#setup-identity-ack');await click('setup-trust');await event('peerApprove');await event('connectOk');
 check('P04 连接不等于输入验证',(await state()).peers['new-peer'].transport==='connected'&&!(await state()).peers['new-peer'].verified);
 await click('setup-layout-next');await event('inputVerified');await click('setup-done');
 check('P05 设置与设备页共享同一状态',(await state()).nodes.length===7&&(await state()).peers['new-peer'].verified);
 await route('trust');await page.locator('[data-revoke="new-peer"]').click();await click('revoke-confirm');
 check('P06 撤销仅影响指定电脑',(await state()).peers['new-peer'].trust==='revoked'&&(await state()).peers.work.transport==='connected'&&(await state()).nodes.length===7);
 await page.locator('[data-reverify="new-peer"]').click();await page.check('#setup-identity-ack');await click('setup-trust');await event('peerApprove');
 check('P07 重新核对不自动连接',(await state()).peers['new-peer'].transport==='offline');
 await click('review-reset');await page.selectOption('#review-role','receiver');await click('global-add');await click('setup-permission-next');await click('setup-candidate');
 await page.check('#setup-identity-ack');await click('setup-trust');await event('peerReject');await click('setup-cancel');
 check('P08 更换失败保留原来源',(await state()).runtime.sourcePeerId==='work'&&(await state()).peers.work.transport==='connected');
 await click('review-reset');await route('sharing');await page.check('#ac-switch-delay');await page.check('#ac-switch-double');await click('ac-sharing-save');
 check('P09 延迟与双触独立',(await state()).sharing.delay&&(await state()).sharing.double);
 const active=(await state()).activeRevision;await click('ac-apply-pending');await click('apply-start');await event('applyFail');
 check('P10 应用失败不改生效版本',(await state()).activeRevision===active&&(await state()).pending.length>0);
 await click('ac-apply-pending');await click('apply-start');await event('applyOk');
 check('P11 应用成功不伪造连接',(await state()).activeRevision>active&&Object.values((await state()).peers).every(p=>p.transport!=='connected'));
 await event('connectOk');check('P12 实际连接事件更新会话',(await state()).peers.work.transport==='connected');
 await route('layout');await click('ac-layout-edit');await page.selectOption('#ac-selected','work');await page.fill('#ac-col','3');await click('ac-set-position');await click('ac-layout-cancel');
 check('P13 取消画布编辑保留原位置',(await state()).nodes.find(n=>n.id==='work').x===1);
 await route('rules');await click('ac-rule-new');await page.fill('#ac-rule-name','示例规则');await page.fill('#ac-rule-key','Ctrl+Alt+L');await click('ac-rule-save');
 check('P14 快捷键冲突可见',(await page.locator('#ac-dialog-error').innerText()).includes('已被'));await click('ac-dialog-close');if(await page.locator('#ac-dialog-discard').isVisible())await click('ac-discard-edit');
 await route('receiver');await page.fill('#ac-x-scale','11');await click('ac-receiver-save');check('P15 输入倍率边界',(await state()).receiver.xScale===1);
 await click('review-reset');await click('global-pause');await event('wake');check('P16 唤醒保留暂停',(await state()).runtime.paused);await click('global-pause');
 await click('app-system-menu');check('P17 面板共用连接状态',(await page.locator('#app-menu').innerText()).includes('已连接 1 台电脑'));await click('menu-hide');await event('disconnect');
 check('P18 后台重要事件通知',await page.locator('#app-notification').isVisible());const seq=(await state()).issueSequence;await event('disconnect');check('P19 重复掉线去重',(await state()).issueSequence===seq);await click('notification-open');
 check('P20 通知进入处理页',(await state()).view==='diagnostics'&&(await state()).runtime.window==='visible');
 await page.uncheck('#review-tray');await click('app-system-menu');await click('menu-hide');check('P21 无返回入口不隐藏',(await state()).runtime.window==='visible');
 await click('review-reset');await route('network');await page.fill('#network-port','24900');await click('network-save');check('P22 网络保存待应用',(await state()).network.port===24900&&(await state()).activeConfig.network.port===24800);
 await route('config');await page.check('#ac-file-mode');await click('ac-file-save');check('P23 文件模式保存不提前只读',!(await state()).activeFileMode);await click('ac-apply-pending');await click('apply-start');await event('applyOk');await route('layout');check('P24 文件模式应用后画布只读',await page.locator('#ac-layout-edit').isDisabled());
 await click('review-reset');await page.selectOption('#ac-platform','windows');await route('maintenance');await page.check('#ac-service');await click('ac-maintenance-save');await page.selectOption('#ac-fault','service');await click('app-system-menu');await click('menu-quit');await click('quit-confirm');check('P25 停止服务失败保持界面',(await state()).runtime.window==='visible');await click('ac-dialog-close');
 await click('review-reset');await page.selectOption('#ac-fault','readonly');await route('network');check('P26 只读写入不改变配置',await page.locator('#network-port').isDisabled()&&(await state()).network.port===24800);
 await page.selectOption('#ac-fault','normal');await click('review-first');await click('setup-receiver');await page.selectOption('#ac-fault','permission');await click('setup-permission-next');check('P27 权限缺失阻止继续',(await state()).setup.stage==='permission');await event('permissionReady');await click('setup-permission-next');await page.selectOption('#ac-fault','discovery');await route('setup');check('P28 发现失败有手动入口',await page.locator('#setup-manual').isVisible());
 await click('setup-manual');await page.fill('#setup-address','192.0.2.21');await click('setup-manual-next');await page.check('#setup-identity-ack');await click('setup-trust');await event('peerApprove');await event('connectOk');await click('setup-layout-next');await click('setup-skip');await click('setup-done');check('P29 首次接收端跳过验证保留未验证',!(await state()).peers['manual-source'].verified&&(await state()).nodes.length===2);

 await click('review-reset');await route('device');await page.locator('[data-device-dialog="work"]').click();await page.fill('#ac-alias-new','work-pc');await click('ac-alias-add');check('A01 别名全局唯一',(await page.locator('#ac-dialog-error').innerText()).includes('无效'));
 await page.fill('#ac-alias-new','work-office');await click('ac-alias-add');await page.locator('[data-device-tab="mapping"]').click();check('A02 六类修饰键完整',await page.locator('[id^="ac-mod-"]').count()===6);await page.selectOption('#ac-mod-Meta','Ctrl');await page.locator('[data-device-tab="corners"]').click();await page.check('#ac-corner-tl');await page.fill('#ac-corner-size','20');await click('ac-device-save');
 check('A03 设备高级配置保存',(await state()).nodes.find(n=>n.id==='work').mapping.Meta==='Ctrl'&&(await state()).nodes.find(n=>n.id==='work').aliases.includes('work-office'));
 await page.locator('[data-device-dialog="dev"]').click();await page.locator('[data-device-tab="fixes"]').click();check('A04 X11修复可用',await page.locator('#ac-fix-xtest').count()===1);await click('ac-dialog-close');await page.locator('[data-device-dialog="test"]').click();await page.locator('[data-device-tab="fixes"]').click();check('A05 Wayland不展示X11修复',await page.locator('#ac-fix-xtest').count()===0);await click('ac-dialog-close');
 await route('rules');await click('ac-rule-new');await page.fill('#ac-rule-key','Ctrl+Alt+2');await click('ac-action-add');await page.selectOption('#ac-action-type-1','keytap');await page.fill('#ac-action-key-1','Ctrl+C');await page.locator('#ac-action-key-1').dispatchEvent('change');await page.selectOption('#ac-action-trigger-1','release');await click('ac-action-add');await page.selectOption('#ac-action-type-2','restart');await click('ac-rule-save');check('A06 重启动作影响确认',(await page.locator('#ac-dialog-error').innerText()).includes('重启'));await page.check('#ac-rule-restart-ack');await click('ac-rule-save');check('A07 多动作和释放触发保存',(await state()).rules.at(-1).actions.length===3&&(await state()).rules.at(-1).actions[1].trigger==='release');
 await route('certificates');await page.fill('#ac-cert-path','draft.pem');await route('logs');await route('certificates');check('A08 页面草稿保留',await page.locator('#ac-cert-path').inputValue()==='draft.pem');await page.selectOption('#ac-fault','write');await click('ac-cert-save');check('A09 写入失败保留原配置',(await state()).security.path==='Deskflow.pem');await page.selectOption('#ac-fault','normal');
 await click('ac-cert-fingerprint');check('A10 现有指纹图案保留',(await page.locator('#ac-fingerprint-display').innerText()).includes('╔'));await click('ac-fp-hash');check('A11 完整SHA256',((await page.locator('#ac-fingerprint-display').innerText()).match(/[0-9A-F]{2}/g)||[]).length===32);await click('ac-dialog-close');
 await page.click('#ac-tls');check('A12 TLS降级须确认',await page.locator('#ac-tls-confirm').isDisabled());await click('ac-tls-cancel');check('A13 取消保留TLS',(await state()).security.tls);
 await route('logs');await page.fill('#ac-log-search','工作电脑');await click('ac-log-next');check('A14 日志搜索匹配跳转',(await page.locator('#ac-log-count').innerText()).startsWith('2 / 3'));await page.check('#ac-log-file');await page.selectOption('#ac-log-level','Verbose');await click('ac-log-save');await click('ac-log-detach');check('A15 日志独立窗口示意',await page.locator('#ac-floating-log').isVisible());await click('ac-floating-attach');
 await route('device');await page.locator('[data-device-dialog="work"]').click();await page.fill('#ac-node-name','未保存名称');await click('ac-dialog-close');check('A16 弹窗草稿保护',await page.locator('#ac-dialog-discard').isVisible());await click('ac-discard-edit');check('A17 放弃弹窗不改模型',(await state()).nodes.find(n=>n.id==='work').name==='工作电脑');
 await route('certificates');await page.selectOption('#ac-cert-bits','4096');await click('ac-cert-generate');await click('ac-generate-confirm');check('A18 本机身份变化停止共享',(await state()).security.bits==='4096'&&(await state()).runtime.process==='stopped'&&Object.values((await state()).peers).every(p=>p.transport==='offline'&&p.localNeedsReverify));
 await click('review-reset');await page.selectOption('#ac-platform','windows');await page.check('#ac-portable');await route('maintenance');check('A19 便携版无服务控制',await page.locator('#ac-service').count()===0);await page.uncheck('#ac-portable');await page.check('#ac-enter-enabled');await click('ac-maintenance-save');check('A20 空命令校验',(await page.locator('#ac-note').innerText()).includes('不能为空'));await page.fill('#ac-enter-command','example-command --on-enter');await click('ac-maintenance-save');check('A21 保存命令不执行',(await page.locator('#ac-note').innerText()).includes('没有执行'));
 await click('ac-clear-profile');await click('clear-model');check('A22 清理需确认对象',(await page.locator('#integrated-error').innerText()).includes('Deskflow'));await click('ac-dialog-close');
 await click('review-reset');await route('certificates');await page.click('#ac-tls');await page.check('#ac-tls-ack');await click('ac-tls-confirm');check('A23 未加密待应用标识',(await page.locator('#ac-main').innerText()).includes('已保存未加密兼容配置'));await click('ac-apply-pending');await click('apply-start');await event('applyOk');check('A24 未加密已生效标识',(await page.locator('#ac-main').innerText()).includes('未加密兼容模式已生效'));await click('review-reset');

 await click('ac-add-offline');await page.fill('#ac-new-name','预配电脑');await page.fill('#ac-new-identifier','preconfigured-pc');await click('ac-new-save');await route('trust');check('A25 预配离线电脑不伪造信任',Object.values((await state()).peers).some(p=>p.name==='预配电脑'&&p.trust==='unknown'&&!p.digest));await route('layout');await page.selectOption('#ac-selected','dev');await click('ac-remove-node');await click('ac-remove-node-confirm');await route('trust');check('A26 移除排列保留独立信任记录',await page.locator('[data-reverify="dev"]').isVisible()&&!(await state()).nodes.some(n=>n.id==='dev'));
 await click('review-reset');await route('trust');await page.locator('[data-verify-input="work"]').click();check('A27 已有连接可重进输入验证',(await state()).setup.stage==='verify');await click('setup-skip');await click('setup-done');
 for(const platform of ['mac','windows','x11','wayland']){await page.selectOption('#ac-platform',platform);for(const v of ['general','maintenance','receiver','certificates','diagnostics'])await route(v);check('PLATFORM '+platform,errors.length===0);}
 for(const width of [1280,1024,736,390,320]){await page.setViewportSize({width,height:1000});for(const v of views){await route(v);const overflow=await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth+2);check('WIDTH '+width+' '+v,!overflow);}}
 await page.setViewportSize({width:1280,height:1000});await click('review-reset');await page.selectOption('#ac-platform','mac');await page.emulateMedia({colorScheme:'light'});
 if(process.env.DESIGN_SCREENSHOT_DIR){await page.screenshot({path:path.join(process.env.DESIGN_SCREENSHOT_DIR,'贯通原型-设备.png'),fullPage:true});await route('trust');await page.screenshot({path:path.join(process.env.DESIGN_SCREENSHOT_DIR,'贯通原型-设备信任.png'),fullPage:true});}
 await route('layout');await page.emulateMedia({colorScheme:'dark'});check('VISUAL 深色表面',await page.locator('#ac-window').evaluate(e=>getComputedStyle(e).backgroundColor)==='rgb(33, 37, 43)');if(process.env.DESIGN_SCREENSHOT_DIR)await page.screenshot({path:path.join(process.env.DESIGN_SCREENSHOT_DIR,'贯通原型-深色.png'),fullPage:true});await page.emulateMedia({colorScheme:'light'});check('VISUAL 浅色表面',await page.locator('#ac-window').evaluate(e=>getComputedStyle(e).backgroundColor)==='rgb(247, 248, 250)');
 if(process.env.DESIGN_SCREENSHOT_DIR){await click('review-first');await click('setup-source');await click('setup-permission-next');await event('incoming');await page.screenshot({path:path.join(process.env.DESIGN_SCREENSHOT_DIR,'贯通原型-首次身份核对.png'),fullPage:true});}

 check('P30 离线无远程请求',network.length===0);check('P31 无脚本和控制台错误',errors.length===0);
 console.log(JSON.stringify({result:'PASS',checks:results.length,cases:results,errors,remoteRequests:network},null,2));
 }catch(e){console.error(e);console.error('Browser errors:',errors);process.exitCode=1;}finally{await browser.close();}
})();
