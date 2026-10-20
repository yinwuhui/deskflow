# Ubuntu 开发构建证据

Ubuntu26.04.1 LTS，x86_64，GNOME50.1／Wayland；Qt6.10.2、GCC15.2、OpenSSL3.5.5。系统Portal接口RemoteDesktop2／InputCapture1／Clipboard1；独立libportal0.10.0安装前缀供本次源码构建使用，不替换系统libportal0.9.1。

- configure-linux.log、build-linux.log、rebuild-linux.log、ctest-linux.log：较早开发快照。39个CTest目标通过，17.43秒。XWindowsClipboardTests在此构建只执行无真实桌面操作的默认构造用例，不证明X11剪贴板实测。
- configure-linux-final.log、build-linux-final.log：后续全部收尾代码的成功编译，含GUI、核心和测试程序。最终代码没有再运行Linux回归。
- 本地socket／模型测试不等于两台电脑联机；未启动新版共享、没有改变两端正在使用的连接。用户已要求自行进行真实联机验收。

不包含登录凭据、证书私钥或用户配置内容。
