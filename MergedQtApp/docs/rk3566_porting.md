# RK3566 + Qt 5.12.8 移植说明

本版本已移除 T113/CedarX `tplayer/xplayer` 依赖，统一使用 GStreamer 1.0，并将设备路径、配置路径、音频和系统命令集中到 `platform/rk3566_platform.*`。

## 构建依赖

目标根文件系统需要 Qt 5.12.8 的 Core、Gui、Widgets、OpenGL、SerialPort、Network、Sql/SQLite 插件，以及 GStreamer 1.0 开发包和运行插件。运行时至少应具备：

- `playbin`、`uridecodebin`、`typefind`；
- 本地文件、HTTP、UDP、RTSP 对应 source/demux 插件；
- H.264/H.265 等业务所需解码器；
- `alsasink`；
- `appsink`、`videoconvert`；视频由 Qt `QOpenGLWidget` 在 EGLFS 场景中最终合成。

Rockchip BSP 若提供 `mppvideodec`、`mpph264dec`、`mpph265dec` 或 `rkvideodec`，程序会提高其 rank，优先走 MPP 硬件解码；插件不存在时由 GStreamer 自动回退。

交叉编译示例：

```bash
export PKG_CONFIG_SYSROOT_DIR=/opt/rk3566-sysroot
export PKG_CONFIG_LIBDIR=/opt/rk3566-sysroot/usr/lib/aarch64-linux-gnu/pkgconfig:/opt/rk3566-sysroot/usr/share/pkgconfig
/opt/qt-5.12.8-rk3566/bin/qmake qt_ycest.pro
make -j$(nproc)
```

## 默认运行路径

- 运行数据目录：`/var/lib/qt_ycest`，不可写时回退到 Qt 用户数据目录；
- 主配置：`/mnt/UDISK/res/static/demoResources/images/logo/net_cfg.ini`；
- 界面配置：`/mnt/UDISK/res/static/demoResources/images/logo/ycest_cfg.ini`；
- 人脸门禁配置：`/home/cat/face_media/modules/facegate/FaceGateQt/config/facegate.ini`；
- SQLite：`/var/lib/qt_ycest/demo.db`；
- 音量文件：`/var/lib/qt_ycest/volume.dat`；
- 心跳：`/run/qt_ycest/heartbeat`；
- 安装目录：`/opt/qt_ycest/bin`；
- 静态资源：`/mnt/UDISK/res/static`；
- 业务存储及磁盘监控根目录：`/mnt/UDISK`；
- 视频下载与本地轮播目录：`/mnt/UDISK/res/static/demoResources/images/video`。

程序启动时检查以上三份磁盘配置；缺失时从 qrc 内置模板复制，已存在的文件不覆盖。初始化完成后，所有配置读写只使用磁盘路径，qrc 仅作为首次创建时的只读模板。

## 可覆盖环境变量

| 变量 | 用途 |
|---|---|
| `QT_YCEST_CONFIG_DIR` | 配置目录 |
| `QT_YCEST_DB_PATH` | SQLite 文件路径 |
| `QT_YCEST_GST_VIDEO_SINK` | 桌面调试视频 sink；EGLFS 正式模式固定使用程序内 appsink |
| `QT_YCEST_GST_AUDIO_SINK` | 音频 sink 工厂名，例如 `alsasink` |
| `QT_YCEST_ALSA_DEVICE` | ALSA PCM，例如 `default` 或 `hw:0,0` |
| `QT_YCEST_ALSA_CARD` | `amixer -c` 的声卡号 |
| `QT_YCEST_ALSA_CONTROL` | mixer control，例如 `Master`/`Speaker` |
| `QT_YCEST_SIGNAL_UART` | 信号板串口 |
| `QT_YCEST_QR_UART` | 二维码串口 |
| `QT_YCEST_CARD_UART` | 读卡器串口 |
| `QT_YCEST_RS485_UART` | RS485 串口 |
| `QT_YCEST_RS485_DIR_DEVICE` | 旧式外部方向设备；RK3566 通常留空 |
| `QT_YCEST_RTC_DEVICE` | RTC 设备，默认 `/dev/rtc0` |
| `QT_YCEST_KEY_DEVICE` | 独立按键输入节点，不再默认占用 `/dev/input/event0` |
| `QT_YCEST_RTSP_LATENCY_MS` | RTSP jitterbuffer 延迟，默认 300 ms |

同样的配置也可写入 `net_cfg.ini` 的 `[paths]`、`[hardware]`、`[gstreamer]` 和 `[audio]` 分组，参见 `deploy/rk3566/net_cfg.ini.example`。

## RS485

RK3566 分支优先通过 Linux `TIOCSRS485` 启用 UART 内核自动方向控制。若 BSP 设备树已经完成自动方向配置而 ioctl 返回 `ENOTTY/EINVAL`，程序会记录告警后继续使用硬件/驱动自动方向。只有显式配置 `hardware/rs485_dir_device` 时，才使用旧 T113 风格的 0/1 方向字符设备。

## 无桌面 EGLFS + Qt OpenGL ES 显示

正式运行不启动 X11/XCB，也不使用 `linuxfb`、`/dev/fb0 mmap` 或独立的
`kmssink` 视频层。Qt 通过 EGLFS/KMS 建立唯一的 DRM/GBM/EGL 全屏窗口；
GStreamer 继续优先使用 Rockchip MPP 硬件解码，解码后的 NV12 帧通过
`appsink` 送入 `VideoHoleWidget(QOpenGLWidget)`，由 OpenGL ES 着色器完成
NV12 到 RGB 的转换、等比缩放和最终合成。

显示链路：

```text
H.264/H.265 -> MPP硬件解码 -> NV12 -> appsink
                                      -> Qt QOpenGLWidget/OpenGL ES
Qt Widgets/弹窗/光标 -----------------> EGLFS/KMS -> DSI显示
```

视频、提示条、弹窗和软件光标处于同一个 Qt 场景中，因此不存在独立原生视频层
覆盖 UI 的问题。为避免 Qt 5.12 EGLFS 在根 surface 创建后才引入
`QOpenGLWidget` 导致黑屏，`main.cpp` 会先构造完整控件树，再首次显示唯一的
EGLFS 根窗口。

推荐通过脚本启动：

```bash
/opt/qt_ycest/bin/deploy/rk3566/run_qt_ycest.sh
```

核心环境变量：

```bash
unset DISPLAY WAYLAND_DISPLAY XDG_SESSION_TYPE
export QT_QPA_PLATFORM=eglfs
export QT_QPA_EGLFS_INTEGRATION=eglfs_kms
export QT_QPA_EGLFS_HIDECURSOR=1
export QT_QPA_EGLFS_FORCE888=1
export QT_YCEST_EGLFS_ATOMIC=0
export QT_QPA_EGLFS_KMS_ATOMIC=0
export QT_OPENGL=es2
export QT_YCEST_VIDEO_OUTPUT=qt
```

运行前应停止 display-manager，确保 Qt 可以独占 DRM/KMS 设备。RK3566 Linux
4.19 BSP 默认使用 legacy page-flip；只有确认当前内核和 Qt 组合的 atomic 提交
稳定时，才设置 `QT_YCEST_EGLFS_ATOMIC=1`。
