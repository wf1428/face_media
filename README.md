# MergedQtApp

MergedQtApp 是面向 RK3566 嵌入式终端的 Qt 综合应用，包含多媒体播放、人脸门禁、IC 卡与二维码业务、设备通信、网络管理和系统设置。

工程将原多媒体系统与 FaceGate 人脸识别系统集成到同一进程，由统一应用外壳管理界面和模块生命周期，适用于 Qt EGLFS/KMS 全屏运行环境。

## 系统架构

```text
                           MergedQtApp
                                │
              ┌─────────────────┴─────────────────┐
              │             AppShell              │
              │  单一顶层窗口 / 模式与资源调度   │
              └─────────────────┬─────────────────┘
                                │
          ┌─────────────────────┴─────────────────────┐
          │                                           │
┌─────────▼──────────┐                      ┌─────────▼──────────┐
│   多媒体业务模块   │                      │   人脸门禁模块     │
│ MultimediaModule  │                      │ FaceGateModule     │
├────────────────────┤                      ├────────────────────┤
│ 本地视频/图片轮播  │                      │ 摄像头采集与预览   │
│ 网络直播与录播     │                      │ 人脸检测与识别     │
│ GStreamer 播放     │                      │ RKNN 活体检测      │
│ FTP 下载与文件管理 │                      │ 人员与通行记录     │
│ 音量/网络/磁盘状态 │                      │ 录入与后台管理     │
└─────────┬──────────┘                      └─────────┬──────────┘
          │                                           │
          └─────────────────────┬─────────────────────┘
                                │
        ┌───────────────────────▼────────────────────────┐
        │              公共服务与设备业务层              │
        │ MQTT / SQLite / IC卡 / 二维码 / RS485 / 串口  │
        │ SR505 / 补光灯 / U盘 / 配置 / 日志 / 心跳     │
        └───────────────────────┬────────────────────────┘
                                │
        ┌───────────────────────▼────────────────────────┐
        │                 RK3566 平台层                  │
        │ EGLFS/KMS / OpenGL ES / RGA / MPP / V4L2     │
        │ ALSA / RTC / Linux 设备节点与系统接口          │
        └────────────────────────────────────────────────┘
```

程序只创建一个 `QApplication` 和一个 EGLFS 顶层窗口。多媒体主界面与人脸门禁界面作为 `QStackedWidget` 的两个页面运行，避免多个原生窗口和 EGL Surface 竞争显示资源。

## 主要功能

### 多媒体系统

- 本地视频和图片混合轮播
- HLS、RTSP、RTMP、SRT、UDP 和 HTTP-TS 等网络媒体播放
- 直播切换、异常重试、无帧监测和播放恢复
- 录播文件远程下载、更新和播放
- GStreamer 解码、RGA 图像处理和 OpenGL 显示
- 播放列表、音量控制、画面截图和播放状态上报
- 网络状态、磁盘空间和 U 盘状态监测
- 文件管理、FTP、MQTT、网络和系统设置

### 门禁系统

- USB/MIPI 摄像头采集与实时预览
- InspireFace 人脸检测、特征提取和身份比对
- RKNN 活体检测
- 人脸注册、人员导入导出和人员信息管理
- 识别结果、现场抓拍和通行记录
- IC 卡、二维码、密码及人脸通行方式
- 在线和离线业务模式
- 管理员登录、参数配置和维护界面

### 通信与设备

- 通过本地 MQTT IPC 与 `mqttd` 通信
- online_v1、online_v2 和 offline_v1 业务分流
- IC 板、信号板、二维码扫描器和 RS485 通信
- SQLite 本地数据存储与 MySQL 人脸业务支持
- SR505 人体感应和 LED 补光灯控制
- RTC、设备序列号、系统重启和运行心跳

## 应用运行关系

应用启动后默认进入多媒体模式。人体感应触发后，外壳暂停多媒体模块并激活人脸门禁模块；人脸模块按需启动摄像头、推理和活体检测。人脸业务结束且满足返回条件后，人脸模块释放运行资源，多媒体模块恢复原播放状态。

```text
应用启动
   │
   ▼
多媒体模式 ── 人体感应 ──► 人脸门禁模式
   ▲                            │
   └──── 识别结束/现场无人 ─────┘
```

`ModeController` 负责切换顺序和状态保护，两个业务模块通过适配器提供统一的初始化、激活、停用和关闭接口。

## 摄像头与图像处理

人脸模块通过统一采集接口支持以下摄像头链路：

| 摄像头类型 | 数据路径 |
| --- | --- |
| MIPI | V4L2 → NV12/NV21 → RGA |
| USB YUYV | V4L2 MMAP → YUYV → RGA → NV12 |
| USB MJPEG | V4L2/GStreamer → MJPEG → MPP → NV12 |

程序默认自动扫描可用的 V4L2/UVC 采集节点并选择匹配的后端。不同输入经过统一图像处理后，交由预览、人脸识别、活体检测和录入流程使用。

## 目录结构

```text
MergedQtApp/
├── common/                  公共协议、数据访问、客户端和工作线程
├── components/              多媒体、设置、输入、文件管理等通用组件
├── config/                  应用外壳配置
├── deploy/rk3566/           RK3566 启动与部署脚本
├── device/                  设备配置与底层适配
├── docs/                    接口、移植和专项设计文档
├── ic_board/                IC 卡、二维码和门禁控制业务
├── modules/
│   ├── multimedia/          多媒体模块适配层
│   └── facegate/            人脸门禁模块及 FaceGateQt
├── platform/                RK3566 路径和系统能力封装
├── shell/                   应用外壳、模式控制和模块接口
├── static/                  图片、配置和默认媒体资源
├── main.cpp                 融合应用入口
├── mainwindow.cpp/.h/.ui    多媒体主窗口
├── MergedQtApp.pro          qmake 工程入口
└── qt_ycest.pro             源文件、依赖与链接配置
```

### 核心模块

| 模块 | 主要职责 |
| --- | --- |
| `shell/AppShell` | 创建统一窗口并装配两个业务模块 |
| `shell/ModeController` | 管理多媒体和人脸模式切换 |
| `components/multimedia` | 播放器、本地轮播、直播、状态监测和截图 |
| `components/settings` | 网络、FTP、MQTT、音频及系统设置 |
| `modules/facegate/FaceGateQt` | 人脸识别、活体、录入、记录和管理 |
| `common` | 协议解析、串口客户端、数据库和公共任务 |
| `ic_board` | IC 卡、二维码、在线/离线门禁及 RS485 |
| `platform` | RK3566 文件路径、设备节点和系统调用 |

## 技术栈

- C++17
- Qt 5.12.8
- GStreamer 1.0
- OpenGL ES 2.0
- Rockchip RGA
- Rockchip MPP
- RKNN Runtime
- InspireFace SDK
- V4L2/UVC
- SQLite / MySQL
- MQTT / FTP

## 构建

构建前需要准备 RK3566 交叉编译工具链、Qt 5.12.8、目标 sysroot 和相关开发库。

检查 `qt_ycest.pro` 中的 SDK 路径：

```text
INSPIREFACE_ROOT
RKNN_ROOT
SYSROOT
```

执行：

```sh
qmake MergedQtApp.pro
make -j4
```

生成的可执行程序为：

```text
MergedQtApp
```

## 配置

| 配置 | 说明 |
| --- | --- |
| `config/app_shell.ini` | 应用模式、人体感应和补光灯 |
| `modules/facegate/FaceGateQt/config/facegate.ini` | 摄像头、模型、识别、数据库和音频 |
| `static/demoResources/images/logo/ycest_cfg.ini` | 多媒体及界面配置 |
| `static/demoResources/images/logo/net_cfg.ini` | 网络、协议和设备配置 |

目标机上的模型、数据库、音频、媒体和静态资源需要保持工程约定的目录结构。

## 运行

RK3566 目标机使用：

```sh
chmod +x deploy/rk3566/run_merged_qt_app.sh
deploy/rk3566/run_merged_qt_app.sh
```

启动脚本默认使用：

```text
QT_QPA_PLATFORM=eglfs
QT_QPA_EGLFS_INTEGRATION=eglfs_kms
```

桌面环境调试可显式选择 XCB：

```sh
QT_QPA_PLATFORM=xcb ./MergedQtApp
```

## 相关文档

- [技术文档](MergedQtApp/docs/technical_doc.md)
- [接口文档](MergedQtApp/docs/interface_doc.md)
- [RK3566 移植说明](MergedQtApp/docs/rk3566_porting.md)
- [性能文档](MergedQtApp/docs/performance_doc.md)
- [FaceGate 设计说明](MergedQtApp/modules/facegate/FaceGateQt/docs/face_gate_design.md)

