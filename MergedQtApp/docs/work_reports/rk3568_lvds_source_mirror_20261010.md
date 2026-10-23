# RK3568：LVDS 为源、HDMI 为镜像目标

Qt 固定显示在 DSI-1 对应的 LVDS 上，保持 1024×768。内核将 Smart1/VP1 的同一份 framebuffer 交给 Smart0/VP0，缩放到 HDMI 当前实际模式。HDMI 拔掉时，LVDS 保持自身的 framebuffer 和输出模式。

## 文件

- 当前驱动及备份目录：`F:/rk3566-proj/face_media/3568双显_同显修改/`。
- 备份：`rockchip_drm_vop2.hdmi_source_backup_20261010.c`，与修改前的 `rockchip_drm_vop2.c` 字节一致。
- 新驱动：`rockchip_drm_vop2.lvds_source.c`。编译内核时，用它替换内核源码中的 `drivers/gpu/drm/rockchip/rockchip_drm_vop2.c`，目标文件名仍为 `rockchip_drm_vop2.c`。
- 原目录中的 `rockchip_drm_vop2.c` 保留未覆盖。
- Qt 修改：`F:/rk3566-proj/face_media/MergedQtApp/main.cpp`。
- DTS 文件未修改，需要手工加入下述属性，再编译、部署设备树。

## 设备树必需修改

在当前 overlay `rk3568-lubancat-2h-lvds-hdmi-1024-768.dts` 的根节点内、最后一个 `};` 前加入：

```dts
fragment@9 {
    target = <&vop>;

    __overlay__ {
        rockchip,lvds-hdmi-mirror;
    };
};
```

若修改的是基础 DTS，在现有 `&vop` 节点内加入同一个布尔属性即可：

```dts
&vop {
    rockchip,lvds-hdmi-mirror;
};
```

两种写法选择实际使用的方式，不必重复添加。`rockchip,lvds-hdmi-mirror` 是本次驱动新增的私有属性，启用它才会启用新镜像路径、限制独立图层，并让 Qt 默认选择 DSI-1。

当前目录的 overlay 已有以下路由，保留即可：

```text
route_dsi0: status=okay，connect=vp1_out_dsi0
dsi0_in_vp0: disabled
dsi0_in_vp1: okay
dsi0、video_phy0: okay
route_hdmi: status=okay，connect=vp0_out_hdmi
hdmi_in_vp0: okay
hdmi_in_vp1: disabled
hdmi: okay
```

现有 VP0 的主图层保持 Smart0，VP1 保持 Smart1。LVDS 的 1024×768 时序及面板初始化序列保持不变。无需交换 HDMI 和 DSI 的 VP 路由。

## 内核变化

1. 镜像源改为 VP1/Smart1，目标改为 VP0/Smart0；源区域保持 LVDS 的 framebuffer 源区域，目标区域为 HDMI 模式的完整有效区域。
2. opt-in 模式下将两个主图层限制在各自的 CRTC，并停止注册独立 cursor/overlay planes。Weston 将光标及各窗口合成到主 framebuffer，避免只镜像背景而漏掉独立图层。
3. 在 atomic_begin 初始化双 VP 翻页等待，atomic_flush 分别记录已提交的 VP，只有两路均产生有效 vblank 后才报告源翻页完成。这样不依赖源 VP 先 flush。
4. HDMI/源输出关闭及驱动解绑时清理镜像等待，避免源事件继续等待已经关闭的 VP。
5. 属性未开启或芯片不是 RK3568 时使用原本的正常 DRM 路径。本次没有改动原有缩放寄存器计算、面板时序和 RK3566 硬件镜像路径。

## Qt 与 Weston

重新编译部署 Qt。Qt 检测到 RK3568 和运行中设备树的新属性后，会明确选择 DSI-1；其他默认环境维持原来的 primaryScreen 路径。可用以下变量显式覆盖屏幕名称：

```sh
export QT_YCEST_OUTPUT_NAME=DSI-1
```

如果目标输出不存在，启动日志会列出 Qt 实际看到的屏幕名并退出，避免静默选择 HDMI。HDMI 插拔或源屏几何改变后，会重新选择源输出并发送全屏请求。窗口所属屏幕改变仅记录服务端通知，避免异步屏幕通知触发连续普通窗口/全屏切换。页面尺寸、控件坐标及业务逻辑不变。

### 2026-10-10：旧版 Qt Wayland 屏幕名称兼容

板上 Qt 日志实际使用 `Screen17`、`Screen18`，无法直接用 DRM 连接器名 `DSI-1` 查找 `QScreen`。修正后的 `main.cpp` 先按名称查找；在 RK3568 Wayland 下未找到 DSI-1 时，从运行中的 `/proc/device-tree/dsi@fe060000/panel@0` 读取面板物理尺寸进行唯一匹配。若尺寸匹配无法唯一确定，再读取原生显示时序，以像素尺寸（兼容旋转及输出缩放）和刷新率唯一匹配。当前面板为 149×228 mm、1024×768、约 79.46 Hz。匹配仍有歧义时退出并提示显式指定 Qt 名称，不按显示顺序猜测。

Qt 每次启动记录 `[DISPLAY] selector=lvds-source-v2`、二进制路径、Qt 版本及每个屏幕的名称、几何区域、物理尺寸和刷新率。首次全屏显示后只绑定监控，不立即再次切换普通窗口/全屏；热插拔后仍恢复源屏全屏。此修正只改变 `main.cpp`，未再修改内核及页面。

最初提供的旧日志及二进制检索未检出选屏提示；用户随后提供的最新日志已确认第一版选屏逻辑在运行，并因找不到名为 DSI-1 的 QScreen 以 status=1 退出。修正旧版 Qt 屏幕名称匹配后，板上已运行 selector=lvds-source-v2，成功通过 1024×768、79.466 Hz 匹配到 Screen18。早期关于运行二进制没有选屏逻辑的判断已被新日志更正。

当前 Weston 配置的 DSI-1 为 `transform=90`。物理模式仍为 1024×768，但逻辑显示区域会旋转为 768×1024；固定的 1024×768 页面可能因最小尺寸限制撑大窗口。若使用横向 1024×768 页面，应设 `transform=normal`，重启 Weston 和 Qt 后验证。若需要竖屏，保留旋转并匹配页面布局；旋转本身不能作为完全无画面的已确认原因。

重新部署后检查实际日志（用户设备路径）：

```sh
grep -E '\[DISPLAY\]|root shown' /var/log/face-media/face_media.log | tail -n 40
```

应能看到 `selector=lvds-source-v2`、`requested="DSI-1"`，以及面板匹配到的实际 Qt 名称。不能固定把 Screen18 视为 LVDS，其编号可能随 Weston 重启变化。本机仍无 Qt 构建环境，此补丁已做代码及 API 核对，未编译或在板上验证。

### 2026-10-10：明确指定 wl-shell 全屏输出（当前版本 v3）

v2 的板上日志先记录源屏为 Screen18，随后却出现 `[DISPLAY] server reported screen "Screen17"`。视频帧已执行 OpenGL 绘制且无 GL 错误，拔掉 HDMI 后 LVDS 恢复画面。结合 Qt 5.12 wl-shell 的全屏请求源码，发现该请求始终传入空 wl_output；Qt 层 setScreen 不能把这条请求变成指定源输出的请求。Weston 可以根据焦点及已有输出选择全屏目标，当前最终选择了 HDMI；这与内核将 LVDS framebuffer 作为镜像源的方案冲突。

当前补丁在 Qt showFullScreen 之后，通过 QPlatformNativeInterface 获取同一条 Wayland 连接上的 `wl_shell_surface` 及源屏 `wl_output`，补发明确携带源输出的 `wl_shell_surface_set_fullscreen`。启动、热插拔恢复时均使用；若服务端迟到的 screenChanged 通知仍指向 HDMI，仅重发指定输出请求，不反复切换普通窗口/全屏。无须创建新的 surface、修改 Qt 系统插件或改变内核镜像方向。启动标识改为 `selector=lvds-source-v3`。

代码修改：`main.cpp` 及 `qt_ycest.pro`。Linux 构建增加 `QT += gui-private` 和 `PKGCONFIG += wayland-client`，必须重新运行 qmake 再编译。构建环境须包含对应 Qt 5.12.8 的私有开发头文件和 Wayland client 开发文件，例如 Debian/Ubuntu 的 qtbase5-private-dev、libwayland-dev；交叉编译时需要安装/同步到构建工具使用的 Qt 和目标 sysroot，单独安装到板上不等于交叉编译环境已具备这些文件。运行时继续使用 `QT_WAYLAND_SHELL_INTEGRATION=wl-shell`。明确配置其他 shell 协议而没有 wl_shell_surface 时会报错退出，不能静默回到不指定输出的行为。

内核文件未再次修改，页面及业务逻辑未改。保留当前旋转配置先验证窗口是否确实进入 LVDS；root shown 的 1024×1024 与源逻辑区域 768×1024 不一致，仍需后续对照页面布局处理，当前补丁不会自动修改所有页面尺寸。

部署后查看 `/var/log/face-media/face_media.log`，应出现：

```text
[DISPLAY] selector=lvds-source-v3
[DISPLAY] native wl-shell fullscreen requested on "Screen18"
```

上面的 Screen18 仅为当前 Weston 会话的名称。若出现迟到的 Screen17 通知，应随后恢复到源屏对应的 Screen18。实际画面还需板上验证，不能只用请求发送日志断定服务端完成迁移。检查 HDMI 接入、拔线、重新插线时 Qt 画面及光标，并对照 DRM primary framebuffer 状态。

本地核对了 Qt 5.12 的原生资源名称及 Wayland/Weston 对指定输出的处理路径，未进行完整 Qt 编译及板上验证。源码依据：

- https://raw.githubusercontent.com/qt/qtwayland/5.12/src/plugins/shellintegration/wl-shell/qwaylandwlshellsurface.cpp
- https://raw.githubusercontent.com/qt/qtwayland/5.12/src/plugins/shellintegration/wl-shell/qwaylandwlshellintegration.cpp
- https://raw.githubusercontent.com/qt/qtwayland/5.12/src/client/qwaylandnativeinterface.cpp
- https://raw.githubusercontent.com/wayland-mirror/weston/8.0.0/libweston-desktop/wl-shell.c
- https://raw.githubusercontent.com/wayland-mirror/weston/8.0.0/desktop-shell/shell.c

Weston 保持 DRM 后端。DSI-1 保持 `mode=current`、`transform=normal`；HDMI 接入时也须正常启用其输出。当前内核只镜像已启用的两个 CRTC，没有独立启动 HDMI 的逻辑，不要把 HDMI 配成 `mode=off`。

```ini
[output]
name=DSI-1
mode=current
transform=normal

[output]
name=HDMI-A-1
mode=preferred
transform=normal
```

以上 HDMI 段用于按 EDID 选择首选模式，不能保证任意显示器一定输出 2560×1440；以 DRM summary 中的实际模式为准。无须启用 Weston 的 mirror 功能，也无须 EGLFS 的 KMS JSON。

镜像对象是 LVDS 逻辑输出。光标在该输出内时随主 framebuffer 镜像；Weston 仍会暴露两个逻辑输出，本改动没有将它们合并成一个逻辑桌面，也没有限制鼠标进入 HDMI 的逻辑区域。当前 Goodix 的 `WL_OUTPUT=DSI-1` 映射继续使用。如果还要求鼠标活动范围始终限制在源屏，需要在 Weston 端进一步处理输入范围。

HDMI 使用铺满缩放：1024×768 显示到 2560×1440 会改变宽高比例，不做等比留黑边。原界面的细节数量仍来自 1024×768。

## 验证

本地对生产 C 函数做语法解析和 DRM/IRQ stub 控制流模拟，19 项检查通过，包括源/目标单独提交、1024×768/1280×720/1920×1080/2560×1440 目标模式、插拔、fence 引用、提交与中断的不同先后顺序、关闭时事件清理。原有寄存器/缩放/plane 更新函数的内容与修改前一致。

本机没有 Qt 和内核交叉编译工具链，未完成实际编译或板上运行验证。模拟检查不能替代编译及真实硬件测试。

部署后检查：

```sh
ls /proc/device-tree/vop@fe040000/rockchip,lvds-hdmi-mirror
dmesg | grep 'LVDS Smart1/VP1'
cat /sys/kernel/debug/dri/0/summary
cat /sys/kernel/debug/dri/0/state
```

两路连接时，Smart1 源/目标均应为 1024×768；Smart0 源为 1024×768，目标为 HDMI 实际分辨率；两路 fb ID 和 pitch 应一致，额外 cursor/overlay planes 不再注册。鼠标在 LVDS 逻辑输出内移动时两路显示相同光标，触摸输入后仍按现有 Qt 策略隐藏光标。拔线后 VP0 关闭，VP1 保持 1024×768，并继续更新；重新插线后恢复镜像，Qt 仍位于 DSI-1。

依次验证不同 HDMI 模式和多次插拔，并检查内核/Weston 日志中是否有 flip_done 超时、WARN 或提交失败。
