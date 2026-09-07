# MergedQtApp

MergedQtApp 是运行在 RK3566 上的 Qt 5.12.8 单进程融合应用，将原多媒体系统与 FaceGateQt 人脸门禁系统合并到同一个 `QApplication` 和 EGLFS 顶层窗口中。

## 工程结构

```text
AppShell（唯一 EGLFS 顶层窗口）
├── QStackedWidget
│   ├── MultimediaModuleAdapter
│   │   └── 原多媒体 MainWindow
│   └── FaceGateModuleAdapter
│       └── FaceGateMainWindow
├── ModeController                 模式切换状态机
├── Sr505PresenceSensor            /dev/sr505 人体感应适配
├── LedFillLight                   /dev/led 补光灯适配
└── CursorOverlay                  全程序唯一光标实例
```

主要目录：

| 路径 | 说明 |
| --- | --- |
| `shell/` | 应用外壳、模式控制、SR505 和补光灯 |
| `modules/multimedia/` | 多媒体模块适配层 |
| `modules/facegate/` | 人脸门禁模块及适配层 |
| `modules/facegate/FaceGateQt/core/` | 摄像头、推理、活体和验证核心逻辑 |
| `components/rga/` | Rockchip RGA 图像转换封装 |
| `platform/` | RK3566 路径、设备节点和系统操作适配 |
| `config/` | 融合外壳配置 |
| `deploy/rk3566/` | RK3566 启动和部署脚本 |

正式入口是根目录的 `main.cpp`。`MergedQtApp.pro` 是规范工程入口，并包含实际构建文件 `qt_ycest.pro`；FaceGateQt 自带的独立入口不参与融合程序构建。

## 模式切换逻辑

应用默认进入多媒体界面，两条业务链路互斥运行：

- 进入人脸模式前，多媒体模块暂停播放并保留当前直播或媒体目标。
- 进入人脸模式后，启动人脸引擎、活体线程和摄像头采集链路。
- 返回多媒体模式时，停止摄像头和活体线程，再恢复暂停前的媒体目标。
- FaceGate 数据库线程按原有策略保留，不随每次界面切换重复创建。

SR505 的当前规则如下：

1. 只有多媒体主界面允许人体感应触发模式切换；设置页等受保护界面不会被强制切走。
2. SR505 高电平切换到人脸识别界面。应用启动时读取到的初始高电平同样有效。
3. 人脸识别界面中，如果已经检测到人脸，SR505 变为低电平不会中断识别。
4. 如果没有检测到人脸、没有识别结果保持、也不在密码或管理界面，SR505 低电平会立即返回多媒体界面。
5. 最终识别成功或失败后启动外壳返回计时，默认保持 5 秒；期间忽略 SR505 下降沿。
6. 5 秒到期时：SR505 仍为高电平则继续停留在人脸界面并恢复识别；已经为低电平则返回多媒体界面。
7. 密码输入和管理面板期间禁止人体感应切换，避免正在进行的操作被打断。

外壳返回时间由 `config/app_shell.ini` 中的以下配置控制：

```ini
[modeSwitch]
recognitionReturnMs=5000
```

FaceGate 配置中的 `verify/result_hold_ms` 属于识别模块内部结果状态，和外壳的 5 秒模式返回计时不是同一个参数。

## 补光灯逻辑

- 进入 FaceGate 并成功激活识别链路时，向 `/dev/led` 写入 `1`。
- 最终通行成功时关闭补光灯。
- 结果保持结束后，如果人体仍为高电平并继续识别，则重新打开补光灯。
- 返回多媒体界面、应用启动和应用退出时都会主动关闭补光灯。
- `/dev/led` 不存在或控制失败只记录警告，不阻止界面切换和人脸识别。

## 摄像头自动适配

默认摄像头来源为 `auto`。正常部署不需要修改配置文件，也不需要设置环境变量来区分 MIPI、USB YUYV 或 USB MJPEG 摄像头。

自动探测过程：

1. 按名称扫描 `/dev/video*`。
2. 只接受驱动为 `uvcvideo`，并同时具有 `Video Capture` 和 `Streaming` 能力的 USB 节点；Metadata 节点会被排除。
3. 使用 `VIDIOC_ENUM_FMT` 判断节点是否支持 `MJPG` 和 `YUYV`。
4. 根据实际支持格式选择后端，而不是依赖固定的 `/dev/video9` 等节点编号。
5. 所有 USB 候选均启动失败时，才回退到原 MIPI 链路。

选择顺序：

| 摄像头能力 | 处理方式 |
| --- | --- |
| 同时支持 MJPEG、YUYV | 优先 MJPEG/MPP，失败后回退 YUYV |
| 仅支持 MJPEG | 使用 MJPEG/MPP |
| 仅支持 YUYV | 使用原 USB YUYV 链路 |
| 没有可用 USB 摄像头 | 使用原 MIPI 链路 |

双格式摄像头成功识别时会输出：

```text
[CAMERA-AUTO] dual-format USB camera; preferring MJPEG/MPP /dev/videoN
```

### MIPI 链路

原 MIPI 实现保持不变：使用 V4L2 MMAP 采集 `[camera]` 中配置的 NV12 或 NV21 数据，再交给现有 RGA 和人脸处理流程。

### USB YUYV 链路

原 USB YUYV 实现保持独立：

```text
V4L2/UVC YUYV (MMAP)
    -> RGA
    -> NV12
    -> RGB/预览/人脸检测与识别
```

摄像头不支持的白平衡、曝光等 UVC 控制只会产生警告，不应导致采集失败。例如：

```text
[CAMERA-BACKEND] USB control exposure_auto_priority: unsupported
```

### USB MJPEG/MPP 链路

MJPEG 后端单独实现在：

- `modules/facegate/FaceGateQt/core/UsbMjpegCaptureBackend.h`
- `modules/facegate/FaceGateQt/core/UsbMjpegCaptureBackend.cpp`

它没有与原 YUYV 或 MIPI 后端混写，管线为：

```text
v4l2src (mmap)
    -> image/jpeg caps
    -> jpegparse
    -> mppjpegdec format=NV12
    -> video/x-raw,format=NV12
    -> appsink
    -> RGA/RGB/预览/人脸检测与识别
```

`appsink` 只保留最新一帧，避免推理速度低于摄像头帧率时累积历史画面。MPP 解码输出可能带有 stride，后端按有效宽高复制 Y、UV 平面，向现有 `CameraService` 提供连续 NV12 数据。

退出人脸模式时，MJPEG 后端先释放当前样本，将 GStreamer 管线切换到 `NULL`，再释放 bus、pipeline 和 MPP 资源。YUYV/MIPI 后端执行 `VIDIOC_STREAMOFF`、解除映射并关闭设备句柄。

### 画面方向

当前工程没有对摄像头画面做水平镜像。不同 USB 摄像头显示方向不一致，通常来自摄像头固件输出方向、传感器安装方向或镜头模组设计，而不是 MJPEG 解码产生的镜像。预览和送入检测识别的图像来自同一帧，若后续需要统一镜像，应同时作用于显示和算法输入，不能只翻转预览。

## 配置文件

### 外壳配置

源码配置：`config/app_shell.ini`。

程序查找顺序：

1. `APP_SHELL_CONFIG` 指定的路径（仅作为可选调试覆盖）。
2. 可执行文件同目录的 `app_shell.ini`。
3. 可执行文件目录下的 `config/app_shell.ini`。
4. 当前工作目录下的 `config/app_shell.ini`。

主要配置：

```ini
[application]
initialMode=multimedia

[modeSwitch]
presenceDriver=sr505
sr505Device=/dev/sr505
sr505ActiveHigh=true
sr505ReconnectMs=1000
recognitionReturnMs=5000

[fillLight]
device=/dev/led
```

目前仅支持以多媒体模式启动；其他 `initialMode` 值会记录警告并强制使用 `multimedia`。

### FaceGate 配置

RK3566 运行时路径固定为：

```text
/home/cat/face_media/modules/facegate/FaceGateQt/config/facegate.ini
```

如果目标文件不存在，程序会从资源中的根目录 `facegate.ini` 模板创建；已存在的运行配置不会被覆盖。

摄像头主要参数：

```ini
[camera]
device=/dev/video0
width=640
height=480
pixel_format=nv12

[camera_usb]
device=auto
width=640
height=480
fps=30
```

自动模式会自行判断 USB 原始格式，因此不需要手工把 `camera_usb/pixel_format` 改成 MJPEG。`QT_YCEST_CAMERA_SOURCE=auto|usb|mipi` 仍保留为开发诊断覆盖；未设置时就是 `auto`。

## 构建依赖

- Qt 5.12.8：Core、Gui、Widgets、OpenGL、SerialPort、Network、Sql、VirtualKeyboard、QML、Quick、QuickWidgets。
- GStreamer 1.0：core、app、video、allocators、rtp 及运行时插件 `v4l2src`、`jpegparse`、`appsink`。
- Rockchip MPP GStreamer 插件：`mppjpegdec`。
- Rockchip RGA：必须提供 `librga.pc` 或 `rockchip_rga.pc`，工程不启用 CPU 图像转换回退。
- InspireFace SDK、RKNN Runtime 和对应 RK3566 sysroot。
- Linux V4L2/UVC 驱动及目标设备节点权限。

`qt_ycest.pro` 顶部的 `INSPIREFACE_ROOT`、`RKNN_ROOT` 和 `SYSROOT` 需要与实际构建环境一致。

## 构建

在已经配置 Qt、pkg-config 和交叉编译 sysroot 的环境中执行：

```sh
qmake MergedQtApp.pro
make -j4
```

构建前可检查 MJPEG 所需插件：

```sh
gst-inspect-1.0 v4l2src
gst-inspect-1.0 jpegparse
gst-inspect-1.0 mppjpegdec
gst-inspect-1.0 appsink
```

## 运行

将二进制、配置、模型、数据库、音频和静态资源部署到约定目录后执行：

```sh
chmod +x deploy/rk3566/run_merged_qt_app.sh
deploy/rk3566/run_merged_qt_app.sh
```

脚本默认设置：

```text
QT_QPA_PLATFORM=eglfs
QT_QPA_EGLFS_INTEGRATION=eglfs_kms
QT_QPA_EGLFS_FORCE888=1
QT_QPA_EGLFS_SWAPINTERVAL=1
```

若外部已经设置这些变量，脚本会尊重外部值。程序仍禁止 `linuxfb`。桌面调试可以显式执行：

```sh
QT_QPA_PLATFORM=xcb ./MergedQtApp
```

## 摄像头验证与排障

列出设备及能力：

```sh
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video9 -D
v4l2-ctl -d /dev/video9 --list-formats-ext
```

独立验证 640x480@30 MJPEG 输入和 MPP 解码：

```sh
gst-launch-1.0 -e -v \
  v4l2src device=/dev/video9 io-mode=mmap do-timestamp=true num-buffers=300 \
  ! image/jpeg,width=640,height=480,framerate=30/1 \
  ! jpegparse \
  ! mppjpegdec format=NV12 \
  ! video/x-raw,format=NV12 \
  ! fakesink sync=false
```

设备节点编号可能随插拔变化，以上 `/dev/video9` 仅用于命令行验证；应用自动模式不依赖该固定编号。

常见错误：

- `The pixelformat 'YUYV' is invalid`：当前摄像头节点不支持 YUYV，应使用其枚举出的 MJPEG 格式。
- `USB MJPEG pipeline did not reach PLAYING`：先用独立 GStreamer 命令确认 UVC 协商、插件和 MPP 解码器。
- `Failed to set/query UVC probe control: -110`：UVC 控制传输超时，通常发生在 MPP 解码前，优先检查摄像头固件、USB 供电、线材、Hub 以及 Linux 4.19 挂起/恢复兼容性。
- `USB VIDIOC_S_FMT failed (errno=5)`：UVC 格式协商返回 I/O 错误，不代表 YUYV 到 NV12 的 RGA 转换出错。
- `rkisp-vir0: update sensor info failed -19` 或 `MIPI VIDIOC_STREAMON failed (errno=19)`：自动模式在 USB 后端全部失败后尝试 MIPI 回退，而设备上没有可用 MIPI 传感器时产生的次级错误。
- `[FILL-LIGHT] cannot open LED device`：补光灯节点不存在，不影响摄像头和识别主链路。

## USB 释放与发热说明

回到多媒体界面后，可以用以下命令确认摄像头没有被进程占用：

```sh
fuser -v /dev/video9 /dev/video10
```

确认 USB 运行时电源状态：

```sh
USB_IF=$(readlink -f /sys/class/video4linux/video9/device)
USB_DEV=$(dirname "$USB_IF")

cat "$USB_DEV/power/control"
cat "$USB_DEV/power/runtime_status"
cat "$USB_DEV/power/autosuspend_delay_ms"
cat "$USB_DEV/power/runtime_usage"
```

典型的正常释放结果是：

```text
control=auto
runtime_status=suspended
runtime_usage=0
```

`suspended` 表示软件采集链路已经释放并进入 USB 运行时挂起，不代表 USB 5V 已物理断开。摄像头在挂起后仍发热，通常与模组内部传感器、ISP、USB 桥或稳压电路继续供电有关。若产品要求多媒体模式下彻底断电降温，需要支持独立端口断电的 USB Hub，或增加由 GPIO 控制的 USB 5V 负载开关；单纯销毁 GStreamer/MPP 管线不能保证切断 VBUS。

