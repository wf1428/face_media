# 接口文档

> **RK3566 移植说明**：本文档记录的是原 T113 基线，设备路径和播放器实现已不再作为当前代码依据。RK3566 + Qt 5.12.8 的实际配置请参见 [`rk3566_porting.md`](rk3566_porting.md)。


**项目名称**: qt_ycest (电梯IC卡控制与多媒体管理系统)  
**文档日期**: 2026-04-24  
**作者**: Dulin  
**项目路径**: `F:\T113-proj\liu_work\qt_ycest_1024x768_v2\qt_ycest_1024x768\qt_ycest_v4\V2\qt_ycest\`

---

## 一、MQTT 业务接口

### 1.1 概述

MQTT 通信通过 mqttd 守护进程中转，应用通过 Unix Domain Socket IPC 与 mqttd 通信。所有业务消息以 JSON 格式传输。

**Topic 命名规则**:
- online_v1: `device/ycLinux/<clientId>/request` (订阅) / `device/ycLinux/<clientId>/event` (发布)
- online_v2: `device/yc/<clientId>/responses` (订阅) / `device/yc/<clientId>/event` (发布)

**来源文件**: `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp#L310-L319`

---

### 1.2 下行接口 (服务端 → 设备)

#### 1.2.1 videoControl - 视频控制

**来源文件**: `V2/qt_ycest/components/settings/mqtt/mqttservice.cpp#L625-L905`

**请求格式**:
```json
{
    "method": "videoControl",
    "id": "请求ID",
    "time": "2026-04-24 10:00:00",
    "deviceId": "设备ID",
    "data": {
        "videoType": "RECORDED|LIVE|NONE",
        "videoVolume": 50,
        "videoPath": "video.mp4",
        "videoUrl": "rtsp://...",
        "ftpPath": "192.168.1.100",
        "ftpPort": "21",
        "ftpUser": "admin",
        "ftpPwd": "password"
    }
}
```

**videoType 说明**:

| 值 | 说明 | 必需字段 |
|----|------|---------|
| `RECORDED` | 下载并播放录播视频 | `videoPath`, `ftpPath`, `ftpPort`, `ftpUser`, `ftpPwd` |
| `LIVE` | 直播流播放 | `videoUrl` |
| `NONE` | 停止所有播放 | 无 |

**设备回执格式**:
```json
{
    "method": "videoControl",
    "id": "请求ID",
    "Time": "2026-04-24 10:00:01",
    "deviceId": "设备ID",
    "data": {
        "videoType": "recorded",
        "videoVolume": 50,
        "videoPath": "video.mp4",
        "videoUrl": "",
        "status": "ok|error",
        "deviceClientIp": "192.168.1.200",
        "ftpDownload": "100%|0%"
    }
}
```

---

#### 1.2.2 getPlayInfo - 获取播放信息

**来源文件**: `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp#L782-L801`

**请求格式**:
```json
{
    "method": "getPlayInfo",
    "id": "请求ID",
    "deviceId": "设备ID"
}
```

**设备响应格式**:
```json
{
    "method": "sendPlayInfo",
    "id": "请求ID",
    "Time": "2026-04-24 10:00:00",
    "deviceId": "设备ID",
    "data": {
        "info": {
            "videoType": "none|live|recorded",
            "videoVolume": 50,
            "videoPath": "video.mp4",
            "videoUrl": "",
            "name": "视频名称",
            "base64Image": "...",
            "deviceLog": "..."
        }
    }
}
```

---

#### 1.2.3 getPlayFileList - 获取播放文件列表

**来源文件**: `V2/qt_ycest/components/settings/mqtt/mqttservice.cpp#L958-L980`

**请求格式**:
```json
{
    "method": "getPlayFileList",
    "id": "请求ID",
    "deviceId": "设备ID"
}
```

**设备响应格式**:
```json
{
    "method": "sendPlayFileList",
    "id": "请求ID",
    "Time": "2026-04-24 10:00:00",
    "deviceId": "设备ID",
    "data": {
        "file": [
            {"id": 1, "name": "视频1"},
            {"id": 2, "name": "视频2"}
        ]
    }
}
```

---

#### 1.2.4 deletePlayFile - 删除播放文件

**来源文件**: `V2/qt_ycest/components/settings/mqtt/mqttservice.cpp#L918-L944`

**请求格式**:
```json
{
    "method": "deletePlayFile",
    "id": "请求ID",
    "deviceId": "设备ID",
    "data": {
        "name": "视频名称",
        "id": "文件ID"
    }
}
```

---

#### 1.2.5 linuxReboot - 远程重启

**来源文件**: `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp#L834-L882`

**请求格式**:
```json
{
    "method": "linuxReboot",
    "id": "请求ID",
    "deviceId": "设备ID"
}
```

**设备回执格式**:
```json
{
    "method": "linuxReboot",
    "id": "请求ID",
    "Time": "2026-04-24 10:00:00",
    "deviceId": "设备ID",
    "data": {
        "status": "ok"
    }
}
```

---

### 1.3 IC 在线协议接口 (online_v2 模式)

**来源文件**: `V2/qt_ycest/ic_board/ic_mqtt_gateway.cpp#L695-L875`

#### 1.3.1 设备上报消息

##### card - 刷卡上报

```json
{
    "methon": "card",
    "id": "消息ID(32位数字)",
    "Time": "2026-04-24 10:00:00",
    "CardID": "卡号",
    "Floor": "楼层"
}
```

##### sendQrCode - 二维码上报

```json
{
    "methon": "sendQrCode",
    "id": "消息ID",
    "Time": "2026-04-24 10:00:00",
    "qrCode": "二维码内容"
}
```

##### keep - 保活消息

```json
{
    "methon": "keep",
    "id": "消息ID",
    "Time": "2026-04-24 10:00:00",
    "Connection_status": "YC-ZKB is alive"
}
```

##### getjdq - RS485 回包上报

```json
{
    "methon": "getjdq",
    "id": "消息ID",
    "Time": "2026-04-24 10:00:00",
    "jdq": "RS485回包文本"
}
```

#### 1.3.2 服务端下行指令

##### reboot - 远程重启

```json
{
    "method": "reboot",
    "id": "请求ID"
}
```

**回执**: `rebootSuccess`

##### setTime - 设置时间

```json
{
    "method": "setTime",
    "time": "2026-04-24 10:00:00"
}
```

##### registerCardOne - 注册单张卡

```json
{
    "method": "registerCardOne",
    "data": {
        "CardNo": "卡号"
    }
}
```

**回执**: `registerSuccess` + `{"CardID": "卡号"}`

##### deleteCardOne - 删除单张卡

```json
{
    "method": "deleteCardOne",
    "data": {
        "CardNo": "卡号"
    }
}
```

**回执**: `deleteSuccess` + `{"CardID": "卡号"}`

##### openFloor - 远程开梯

```json
{
    "method": "openFloor",
    "data": {
        "floor": "楼层号或帧数据"
    }
}
```

**floor 格式**:
- 数字: `"1"`, `"-1"`, `"120"` (自动构建开梯帧)
- 34字节正文: 自动补 `#!` 尾部
- 36字节完整帧: `#@...#!`

**回执**: `openFloorSuccess`

##### openTrafficFloor - 开放楼层限行

```json
{
    "method": "openTrafficFloor",
    "data": {
        "openTrafficFloor": "指令"
    }
}
```

**指令格式**:
- 单层: `"aa001"` (aa=开放, 001=楼层号)
- 批量: `"OH<32位HEX位图>"` (OH=开放)
- 楼层号: `"1"`, `"-1"` (自动转换为 aa 格式)

**回执**: `openTrafficFloorSuccess`

##### closeTrafficFloor - 关闭楼层限行

```json
{
    "method": "closeTrafficFloor",
    "data": {
        "closeTrafficFloor": "指令"
    }
}
```

**指令格式**: 同 openTrafficFloor，但使用 `bb`/`CL` 前缀

**回执**: `closeTrafficFloorSuccess`

##### getTrafficFloor - 查询楼层限行状态

```json
{
    "method": "getTrafficFloor",
    "id": "请求ID"
}
```

**回执**:
```json
{
    "methon": "controlFloor",
    "id": "请求ID",
    "Time": "...",
    "faceDevice": "设备名称",
    "Floor": "128位0/1字符串"
}
```

##### sendFloorQr - 二维码开梯回包

```json
{
    "method": "sendFloorQr",
    "data": "34字节帧数据"
}
```

##### setjdq - 设置继电器

```json
{
    "method": "setjdq",
    "data": {
        "setjdq": "指令"
    }
}
```

##### getjdq - 查询继电器状态

```json
{
    "method": "getjdq"
}
```

##### ResponseAlive - 存活响应

```json
{
    "method": "ResponseAlive",
    "data": {
        "code": 0,
        "msg": "ok"
    }
}
```

---

## 二、IPC 接口 (应用 ↔ mqttd)

**来源文件**: `V2/qt_ycest/components/settings/mqtt/mqttipcclient.h`、`mqttmanager.cpp`

### 2.1 连接

- 协议: Unix Domain Socket
- 默认路径: `/tmp/mqttd.sock` (由 `MqttConfig::ipcPath` 配置)
- 通信格式: JSON 行协议 (每行一条 JSON 消息，以 `\n` 分隔)

### 2.2 下行命令

| 命令 | 格式 | 说明 |
|------|------|------|
| set_config | `{"cmd":"set_config","mqtt":{...},"subs":[...]}` | 设置MQTT配置 |
| apply | `{"cmd":"apply"}` | 应用配置并连接Broker |
| publish | `{"cmd":"publish","topic":"...","payload":{...}}` | 发布消息 |
| status | `{"cmd":"status"}` | 请求状态 |
| test | `{"cmd":"test"}` | 测试连接 |

### 2.3 上行消息

| type | 格式 | 说明 |
|------|------|------|
| hello/status | `{"type":"status","connected":true,"last_rc":0,"mqtt":{...},"sub_topics":"..."}` | 状态推送 |
| conn | `{"type":"conn","state":"connected|disconnected|failed","rc":0}` | 连接状态变更 |
| msg | `{"type":"msg","topic":"...","payload":{...}}` | MQTT消息转发 |
| ack | `{"type":"ack","cmd":"...","ok":true,"msg":"..."}` | 命令应答 |
| error | `{"type":"error","msg":"..."}` | 错误通知 |

---

## 三、串口接口

### 3.1 IC卡读卡器

**来源文件**: `V2/qt_ycest/ic_board/serial_init.h`、`V2/qt_ycest/common/clients/icboard_client.cpp`

| 参数 | 值 |
|------|-----|
| 设备路径 | `/dev/ttyAS2` |
| 波特率 | 9600 (默认) |
| 数据位 | 8 |
| 校验位 | 无 |
| 停止位 | 1 |
| 流控 | 无 |
| 模式 | 只读 |

### 3.2 QR 扫描器

**来源文件**: `V2/qt_ycest/ic_board/serial_init.h`

| 参数 | 值 |
|------|-----|
| 设备路径 | `/dev/ttyAS1` |
| 波特率 | 9600 |
| 模式 | 只读 |

### 3.3 RS485 串口

**来源文件**: `V2/qt_ycest/ic_board/rs485_driver_port.cpp`

| 参数 | 值 |
|------|-----|
| 设备路径 | `/dev/ttyAS4` |
| 波特率 | 9600 |
| 方向控制 | `/dev/rs485_dir` GPIO |
| 模式 | 读写 |

---

## 四、数据库接口 (DbStore)

**来源文件**: `V2/qt_ycest/common/sql/dbstore.h`、`dbstore.cpp`

### 4.1 初始化

```cpp
static bool bootstrap(const QString& dbPath);
static void close();
```

### 4.2 配置管理

```cpp
static bool setConfig(const QString& key, const QVariant& value);
static QVariant getConfig(const QString& key, const QVariant& defaultValue = {});
static bool deleteConfig(const QString& key);
static bool hasConfig(const QString& key);
```

### 4.3 刷卡日志

```cpp
static bool insertSwipeLog(const QString& ts, const QString& cardId,
                           const QString& cardType, int result,
                           const QString& reason,
                           const QByteArray& raw = QByteArray(),
                           int uploaded = 0);

static QList<SwipeLogRow> querySwipeLogs(int limit = 50, int offset = 0);
static QList<SwipeLogRow> queryUnuploadedLogs(int limit = 100);
static bool markUploaded(const QList<qint64>& ids);
```

### 4.4 通用 SQL

```cpp
static bool exec(const QString& sql);
static QList<QVariantMap> query(const QString& sql, const QList<QVariant>& binds = {});
```

### 4.5 维护

```cpp
static bool vacuum();
static bool checkpoint();
static QString lastError();
```

---

## 五、事件桥接接口 (IcEventBridge)

**来源文件**: `V2/qt_ycest/ic_board/ic_event_bridge.h`

IcEventBridge 是单例事件总线，用于跨模块信号转发。

### 5.1 信号

| 信号 | 参数 | 说明 |
|------|------|------|
| `cardSwiped` | `(QString cardId, QString cardType, QByteArray rawFrame)` | 刷卡事件 |
| `cardPassed` | `(QString cardId, QString floor, QByteArray rawFrame)` | 刷卡通过 |
| `cardDenied` | `(QString cardId, QString reason)` | 刷卡拒绝 |
| `onlineQrScanned` | `(QString qrCode)` | 在线二维码扫描 |
| `rs485FrameReceived` | `(QByteArray frame)` | RS485 帧接收 |
| `rs485SendFinished` | `(QString sourceTag, bool ok, QString reason)` | RS485 发送完成 |
| `toastPassRequested` | `()` | 请求显示通过提示 |
| `toastFailRequested` | `(QString reason)` | 请求显示失败提示 |

### 5.2 方法

```cpp
void requestRs485Send(const QByteArray &frame, const QString &sourceTag = QString());
void emitToastPassRequested();
void emitToastFailRequested(const QString &reason);
```

---

## 六、设备配置接口 (DeviceConfigSync)

**来源文件**: `V2/qt_ycest/ic_board/device_config_sync.h`

### 6.1 数据结构

```cpp
struct DeviceConfig {
    QString deviceName;
    int floorNum = 1;
    QByteArray secret8;        // 8字节离线校验密钥
    QByteArray floorFlags;     // 楼层限行标志
};
```

### 6.2 主要接口

```cpp
static DeviceConfig bootstrapAndLoad(const QString& dbPath);
static bool saveFloorFlags128Bits(const QByteArray& bits);
static QByteArray floorFlags128Bits();
```

---

## 七、Core 迁移同步接口 (CoreMigrationSync)

**来源文件**: `V2/qt_ycest/ic_board/core_migration_audit/core_migration_sync.h`

```cpp
static bool registerCard(const QString& cardId);
static bool deleteCard(const QString& cardId);
static bool isRegisteredCard(const QString& cardId);
static QString normalizeCardId(const QString& raw);
static QString floorFlags128String();
static QString sanitizeRs485Text(const QByteArray& frame);
```
