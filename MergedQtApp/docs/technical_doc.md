# 技术文档

> **RK3566 移植说明**：本文档记录的是原 T113 基线，设备路径和播放器实现已不再作为当前代码依据。RK3566 + Qt 5.12.8 的实际配置请参见 [`rk3566_porting.md`](rk3566_porting.md)。


**项目名称**: qt_ycest (电梯IC卡控制与多媒体管理系统)  
**文档日期**: 2026-04-24  
**作者**: Dulin  
**项目路径**: `F:\T113-proj\liu_work\qt_ycest_1024x768_v2\qt_ycest_1024x768\qt_ycest_v4\V2\qt_ycest\`

---

## 一、项目概述

### 1.1 项目定位

qt_ycest 是一款运行在 Allwinner T113-S4 嵌入式 Linux 平台上的 Qt5 桌面应用程序，主要面向电梯场景的 IC 卡门禁控制与多媒体信息发布。该系统集成了离线/在线刷卡验证、RS485 分层板通信、MQTT 远程管理、多媒体播放、FTP 文件下载等功能。

### 1.2 技术栈

| 技术领域 | 技术选型 |
|---------|---------|
| 编程语言 | C++11 |
| UI 框架 | Qt 5.15.2+ (widgets, multimedia, multimediawidgets) |
| 通信协议 | MQTT (通过 mqttd 守护进程 IPC 中转) |
| 串口通信 | Qt SerialPort |
| 数据库 | SQLite (QSQLITE 驱动, WAL 模式) |
| 网络通信 | Qt Network |
| 视频解码 | tplayer/xplayer (全平台 CedarX 硬件加速) |
| 构建系统 | qmake |
| 目标平台 | T113-S4 (ARM Cortex-A7, Tina Linux 5.0) |

### 1.3 屏幕分辨率

目标分辨率: 1024×768

---

## 二、系统架构

### 2.1 整体架构图

```
┌─────────────────────────────────────────────────────────┐
│                    MainWindow (主窗口)                    │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌───────────┐  │
│  │ MenuPage │ │App3Page  │ │ FilePage │ │SettingsDlg│  │
│  │ (菜单页) │ │(多媒体页)│ │(文件管理)│ │ (设置对话框)│  │
│  └──────────┘ └──────────┘ └──────────┘ └───────────┘  │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐                │
│  │CursorOvl │ │UdiskStat │ │CmdDialog │                │
│  │(光标层)  │ │(U盘状态) │ │(调试窗口)│                │
│  └──────────┘ └──────────┘ └──────────┘                │
├─────────────────────────────────────────────────────────┤
│                    ic_board (IC卡业务层)                  │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌───────────┐  │
│  │ IcBoard  │ │IcOffline │ │IcOffline │ │IcEventBrg │  │
│  │ (启动引导)│ │(离线刷卡)│ │ Checker  │ │(事件桥接) │  │
│  └──────────┘ └──────────┘ └──────────┘ └───────────┘  │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌───────────┐  │
│  │Rs485Port │ │SerialInit│ │IcOffline │ │DeviceCfg  │  │
│  │(RS485通信)│ │(串口初始化)│ │   QR     │ │  Sync     │  │
│  └──────────┘ └──────────┘ └──────────┘ └───────────┘  │
├─────────────────────────────────────────────────────────┤
│                   common (通用基础层)                      │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌───────────┐  │
│  │IcBoard   │ │SignalBrd │ │ DbStore  │ │ProbeLog   │  │
│  │ Client   │ │ Client   │ │(数据库)  │ │(探针日志) │  │
│  └──────────┘ └──────────┘ └──────────┘ └───────────┘  │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐                │
│  │IcBoard   │ │SignalBrd │ │ Protocol │                │
│  │ Worker   │ │ Worker   │ │ Decoders │                │
│  └──────────┘ └──────────┘ └──────────┘                │
├─────────────────────────────────────────────────────────┤
│                MQTT 通信层                                │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌───────────┐  │
│  │MqttMgr   │ │MqttSvc   │ │IcMqttGw  │ │MqttIpcCli │  │
│  │(管理器)  │ │(业务服务)│ │(IC网关)  │ │(IPC客户端)│  │
│  └──────────┘ └──────────┘ └──────────┘ └───────────┘  │
├─────────────────────────────────────────────────────────┤
│                外部守护进程                                │
│  ┌──────────┐ ┌──────────┐                              │
│  │  mqttd   │ │ FTP客户端│                              │
│  │(MQTT代理)│ │(文件下载)│                              │
│  └──────────┘ └──────────┘                              │
└─────────────────────────────────────────────────────────┘
```

### 2.2 模块职责

#### 2.2.1 主窗口层 (`mainwindow.cpp`)

**文件路径**: `V2/qt_ycest/mainwindow.cpp`

- 管理 `QStackedWidget` 页面切换（菜单页、多媒体页、文件管理页）
- 初始化 `SettingsDialog`、`CommandDialog`、`KeyService` 等组件
- 保存/加载 Feature 配置（offline_v1 / online_v1 / online_v2）
- 连接各组件间的信号槽

#### 2.2.2 IC卡业务层 (`ic_board/`)

**文件路径**: `V2/qt_ycest/ic_board/`

| 文件 | 职责 |
|------|------|
| `ic_board.cpp/h` | `IcIoBootstrap::startAll()` 启动所有 IC 板 IO 组件 |
| `ic_offline.cpp/h` | 离线刷卡处理：卡号提取、继电器控制、音频反馈 |
| `ic_offline_checker.cpp/h` | 离线刷卡校验：时间权限、楼层权限、密钥校验 |
| `ic_offline_qr.cpp/h` | 离线二维码处理 |
| `ic_event_bridge.cpp/h` | 单例事件桥接：跨模块信号转发 |
| `ic_toast.cpp/h` | 单例 Toast 通知：通过/失败提示 |
| `rs485_driver_port.cpp/h` | RS485 串口通信：方向控制、帧发送/接收 |
| `serial_init.cpp/h` | 串口初始化：QR 扫描器、IC 卡读卡器 |
| `device_config_sync.cpp/h` | 设备配置同步：数据库读写、楼层标志管理 |
| `core_migration_sync.cpp/h` | Core 迁移同步：卡号注册/删除/查询 |
| `core_network_card_compat.cpp/h` | 网卡兼容性检查 |

#### 2.2.3 通用基础层 (`common/`)

**文件路径**: `V2/qt_ycest/common/`

| 子目录 | 职责 |
|--------|------|
| `clients/` | IC板客户端和信号板客户端（串口连接管理） |
| `protocols/` | 协议解码器（IC板帧解码、信号板帧解码、分帧器） |
| `workers/` | 工作线程（IC板工作器、信号板工作器） |
| `sql/` | SQLite 数据库封装（DbStore 单例） |
| `debug/` | 探针日志工具（ProbeLog） |

#### 2.2.4 MQTT 通信层 (`components/settings/mqtt/`)

**文件路径**: `V2/qt_ycest/components/settings/mqtt/`

| 文件 | 职责 |
|------|------|
| `mqttmanager.cpp/h` | MQTT 管理器：IPC 连接、消息分发、配置下发 |
| `mqttservice.cpp/h` | MQTT 业务服务：报文构建、视频控制、文件管理 |
| `ic_mqtt_gateway.cpp/h` | IC MQTT 网关：在线协议的 IC 卡/二维码/RS485 桥接 |
| `mqttipcclient.cpp/h` | IPC 客户端：与 mqttd 守护进程的 Unix Socket 通信 |

---

## 三、核心数据流

### 3.1 离线刷卡流程

```
IC卡读卡器 → 串口(/dev/ttyAS2) → IcBoardClient → IcBoardSplitter
    → IcBoardDecoder → IcBoardWorker → IcEventBridge::cardSwiped
    → IcOffline::onCardSwiped → OfflineChecker::check()
    → [通过] IcEventBridge::cardPassed → 音频播放 + RS485开梯
    → [拒绝] IcEventBridge::cardDenied → 音频播放 + Toast提示
```

**关键文件**:
- `V2/qt_ycest/common/protocols/icboard_splitter.cpp` - 帧拆分
- `V2/qt_ycest/common/protocols/icboard_decoder.cpp` - 帧解码
- `V2/qt_ycest/ic_board/ic_offline.cpp` - 离线刷卡处理
- `V2/qt_ycest/ic_board/ic_offline_checker.cpp` - 权限校验

### 3.2 在线刷卡流程

```
IC卡读卡器 → 串口 → ... → IcEventBridge::cardPassed
    → IcMqttGateway::onCardPassed → publishEvent("card")
    → MqttManager → mqttd IPC → MQTT Broker → 服务端
    → 服务端返回 sendFloorQr → IcMqttGateway::handleIncoming
    → executeSendFloorQr → RS485 发送开梯帧
```

**关键文件**:
- `V2/qt_ycest/ic_board/ic_mqtt_gateway.cpp` - 在线网关
- `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp` - 消息管理

### 3.3 MQTT 消息处理流程

```
mqttd IPC → MqttIpcClient::frameReceived → MqttManager::onIpcFrameReceived
    → handleIpcLine() → JSON解析
    → [type=msg] extractPayloadObjectFromMsgObject → dispatchPayloadMessage
        → IcMqttGateway::handleIncoming (在线IC协议)
        → handleGetPlayFileList (播放文件列表)
        → handleGetPlayInfo (播放信息)
        → handleDeletePlayFile (删除文件)
        → handleVideoControl (视频控制)
        → handleStreamUrlMessage (直播流)
        → handleLinuxReboot (远程重启)
```

**关键文件**:
- `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp#L1071-L1147` - 消息分发

### 3.4 视频控制流程

```
MQTT videoControl → MqttService::handleVideoControlMessage
    → [RECORDED] 写FTP配置 → ftpDownloadTaskReady → FTP下载 → 下载完成回调
    → [LIVE] 写stream.url → liveStreamReady → MultimediaDemo拉流播放
    → [NONE] stopAllPlayRequested → 停止所有播放
```

**关键文件**:
- `V2/qt_ycest/components/settings/mqtt/mqttservice.cpp#L625-L905` - 视频控制处理

---

## 四、协议规范

### 4.1 IC板串口协议

**帧格式**:

| 帧类型 | 长度 | 帧头 | 帧尾 | 说明 |
|--------|------|------|------|------|
| 通过帧 | 36字节 | `#@` | `#!` | 刷卡通过确认 |
| 访客卡 | 103字节 | `#@` | - | 访客卡原始数据 |
| 多层卡 | 115字节 | `#@` | - | 多层卡原始数据 |

**字段解析** (来源: `V2/qt_ycest/common/protocols/icboard_decoder.cpp`):

| 字段 | 偏移 | 长度 | 说明 |
|------|------|------|------|
| 帧头 | 0 | 2 | `#@` (0x23 0x40) |
| 卡号 | 35 | 8 | ASCII 编码卡号 |
| 继电器次数(访客) | 99 | 2 | HEX 编码 |
| 继电器号(访客) | 101 | 2 | DEC 编码 |
| 继电器次数(多层) | 111 | 2 | HEX 编码 |
| 继电器号(多层) | 113 | 2 | DEC 编码 |

### 4.2 信号板串口协议

**帧格式** (来源: `V2/qt_ycest/common/protocols/signalboard_decoder.cpp`):

| 字段 | 偏移 | 说明 |
|------|------|------|
| STX | 0 | 0x02 (帧头) |
| 方向 | 1 | '0'=停, '1'=上行, '3'=下行 |
| 模式 | 5 | '8'=正常, '1'=满员, '2'=超载, '3'=消防, '4'=检修 |
| 楼层十位 | 7 | 数字或 'B'/'-' |
| 楼层个位 | 8 | 数字 |
| ETX | 11 | 0x03 (帧尾) |

**帧长度**: 固定 12 字节

### 4.3 RS485 协议

**帧格式** (来源: `V2/qt_ycest/ic_board/rs485_driver_port.cpp`):

- 方向控制: 通过 `/dev/rs485_dir` GPIO 设备控制收发方向
- 发送: 先设置 TX 方向 → 写数据 → 恢复 RX 方向
- 接收: 持续监听，数据到达后 emit `frameReceived`

### 4.4 MQTT IPC 协议

应用与 mqttd 守护进程通过 Unix Domain Socket 通信，使用 JSON 行协议：

**下行命令** (应用 → mqttd):

| 命令 | 说明 |
|------|------|
| `{"cmd":"set_config","mqtt":{...},"subs":[...]}` | 设置 MQTT 配置和订阅 |
| `{"cmd":"apply"}` | 应用配置并连接 |
| `{"cmd":"publish","topic":"...","payload":{...}}` | 发布消息 |
| `{"cmd":"status"}` | 请求状态 |
| `{"cmd":"test"}` | 测试连接 |

**上行消息** (mqttd → 应用):

| type | 说明 |
|------|------|
| `hello` / `status` | 连接状态推送 |
| `conn` | 连接状态变更 (connected/disconnected/failed) |
| `msg` | MQTT 消息转发 |
| `ack` | 命令应答 |
| `error` | 错误通知 |

---

## 五、配置管理

### 5.1 配置文件

**主配置文件**: `/mnt/UDISK/res/static/demoResources/images/logo/net_cfg.ini`

**分组结构**:

| 分组 | 字段 | 说明 |
|------|------|------|
| `[feature]` | offline_v1, online_v1, online_v2 | 功能开关 |
| `[feature]` | air_sys, media_sys, broad_sys, tbk_sys | 子系统开关 |
| `[mqtt]` | protocol_mode | 协议模式 (offline/online) |
| `[mqtt]` | ipc_path | mqttd IPC 路径 |
| `[mqtt]` | client_id | MQTT 客户端 ID |
| `[mqtt]` | port, keepalive, qos, tls | MQTT 连接参数 |
| `[mqtt]` | sub_topics_online_v1/v2 | 订阅主题 |
| `[mqtt]` | publish_topic_online_v1/v2 | 发布主题 |
| `[mqtt]` | username/password_online_v1/v2 | 认证凭据 |
| `[ftp]` | host, port, username, password | FTP 配置 |
| `[ftp]` | remote_path, local_path | 文件路径 |
| `[stream]` | url | 直播流地址 |
| `[network]` | IP/ip | 设备 IP 地址 |

**来源文件**: `V2/qt_ycest/mainwindow.cpp#L527-L593`、`V2/qt_ycest/components/settings/mqtt/mqttservice.cpp`

### 5.2 数据库

**数据库文件**: `demo.db` (路径由 `MqttConfig::dbPath` 指定)

**表结构** (来源: `V2/qt_ycest/common/sql/dbstore.cpp`):

```sql
CREATE TABLE IF NOT EXISTS config (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS swipe_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts TEXT NOT NULL,
    card_id TEXT,
    card_type TEXT,
    result INTEGER NOT NULL,
    reason TEXT,
    raw BLOB,
    uploaded INTEGER NOT NULL DEFAULT 0
);
```

**PRAGMA 设置**:
- `journal_mode=WAL` - Write-Ahead Logging 模式
- `synchronous=NORMAL` - 性能/安全折中
- `foreign_keys=ON` - 启用外键约束
- `temp_store=MEMORY` - 临时表使用内存

---

## 六、线程模型

### 6.1 主线程

- Qt 事件循环
- UI 渲染与交互
- 串口数据读取 (IC板客户端)
- MQTT IPC 通信

### 6.2 工作线程

**信号板工作线程** (来源: `V2/qt_ycest/common/clients/signalboard_client.cpp`):

```
SignalBoardClient → QThread → SignalBoardWorker
    → 串口读取 → 分帧 → 解码 → emit stateReceived (跨线程 QueuedConnection)
```

### 6.3 定时器

| 定时器 | 间隔 | 用途 | 来源文件 |
|--------|------|------|---------|
| 心跳定时器 | 8000ms | 写入心跳文件 | `main.cpp#L100` |
| IPC 重连定时器 | 1000ms | 自动重连 mqttd | `mqttmanager.cpp#L383` |
| 信号板在线检测 | 800ms | 检测信号板在线状态 | `signalboard_worker.cpp` |
| IC 网关保活 | 60000ms | 发送 keepalive 消息 | `ic_mqtt_gateway.cpp#L267` |
| U盘检测 | 300ms | 检测 U盘插拔 | `mainwindow.cpp#L523` |
| 磁盘使用监控 | 30000ms | 监控磁盘空间 | `disk_usage_monitor.cpp` |

---

## 七、构建与部署

### 7.1 构建依赖

**来源文件**: `V2/qt_ycest/qt_ycest.pro`

```
QT += core gui multimedia multimediawidgets serialport network sql
CONFIG += c++11
```

**外部库**:
- `libtplayer` - 全志硬件视频解码
- `libxplayer` - 扩展播放器
- `libasound` - ALSA 音频

**SDK 路径** (硬编码):
```
CEDARX_OUT = /home/meetyoo/100ask_s4_sdk/t113_tinasdk5.0-v1/out/t113_s4/100ask_devkit_nand/openwrt
```

### 7.2 资源文件

**来源文件**: `V2/qt_ycest/resources.qrc`

资源包含: 图标、样式图片、音频文件、字母/数字图片、空调图标、广播图标等。

### 7.3 部署路径

| 路径 | 用途 |
|------|------|
| `/mnt/app/` | 应用配置目录 |
| `/mnt/UDISK/res/static/demoResources/images/logo/net_cfg.ini` | 网络与MQTT配置 |
| `/mnt/UDISK/` | U盘/存储挂载点 |
| `/mnt/UDISK/res/static/demoResources/images/video/` | 视频文件目录 |
| `/run/app_stack/qt_heartbeat` | 心跳文件 |
| `/tmp/mqttd.sock` | mqttd IPC socket |
| `/dev/ttyAS1` | QR 扫描器串口 |
| `/dev/ttyAS2` | IC 卡读卡器串口 |
| `/dev/ttyAS4` | RS485 串口 |
| `/dev/rs485_dir` | RS485 方向控制 GPIO |
| `/dev/rtc1` | RTC 时钟设备 |
