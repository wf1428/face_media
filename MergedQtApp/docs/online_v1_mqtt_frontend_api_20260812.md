# online_v1 MQTT 前端对接接口说明

> 工程：MergedQtApp（RK3566 / Qt 5.12.8）  
> 文档日期：2026-08-27  
> 文档状态：与当前源码实现同步；尚未连接真实 Broker 或目标板验证  
> 适用范围：`feature/online_v1=true` 的 ycLinux V1 MQTT 路由

## 1. 对接结论

online_v1 使用“一条下行主题 + 一条上行主题”，以 JSON `method` 区分业务：

| 方向 | Topic | 说明 |
|---|---|---|
| 平台/前端 → 设备 | `device/ycLinux/{deviceId}/request` | 设备订阅；所有平台指令发到此处 |
| 设备 → 平台/前端 | `device/ycLinux/{deviceId}/event` | 设备发布；所有响应、状态和主动事件发到此处 |

`{deviceId}` 就是 MQTT `client_id`。当前设备自动生成规则为：读取设备序列号，计算带盐 SHA-256，取前 10 位大写十六进制，并加前缀 `YCEEQZ`，例如 `YCEEQZA10C1FFC62`。前端不应自行推算，应使用设备注册信息或实际 Topic 中的值。

当前代码默认 MQTT 端口为 `1883`、keepalive 为 `30s`、QoS 为 `0`、TLS 为关闭；这些均是部署配置，不是业务协议常量。业务 JSON 直接作为 MQTT Payload 发布，本文示例不包含设备内部 mqttd IPC 的 `{"cmd":"publish",...}` 包装。

## 2. 公共报文约定

### 2.1 标准信封

```json
{
  "method": "业务方法名",
  "id": "消息唯一ID",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {},
  "time": "2026-08-12 14:30:00"
}
```

| 字段 | 类型 | 对接要求 |
|---|---|---|
| `method` | string | 区分大小写，必须使用本文列出的准确拼写 |
| `id` | string | 请求/消息唯一 ID；有回执的接口通常原样返回，用作前端关联键 |
| `deviceId` | string | 应与 Topic 中的 `{deviceId}` 及设备当前 `client_id` 一致 |
| `data` | object | 业务字段；无参数时建议发送 `{}` |
| `time` | string | 格式 `yyyy-MM-dd HH:mm:ss`；设备上行均使用此格式 |

注意：

- online_v1 应统一使用 `method` 和小写 `time`，不要使用旧 IC 协议的 `methon`、`Time`。
- 当前通用媒体处理器不会统一校验 `deviceId`；二维码和 `remoteCall` 会严格校验。前端仍应始终发送正确的 `deviceId`。
- 人员同步按 `(deviceId, id)` 做幂等。同一 ID、同一内容会返回缓存结果并增加 `data.duplicate=true`；同一 ID 对应不同内容会返回 `MESSAGE_ID_CONFLICT`。
- MQTT 已提交不等于硬件动作一定成功；应以对应业务回执为准。明确无回执的接口见第 4 节。

### 2.2 接口总览

| method | 方向 | 设备是否回消息 | 说明 |
|---|---|---:|---|
| `getPlayFileList` | 下行 | 是：`sendPlayFileList` | 查询本地视频和图片列表 |
| `getPlayInfo` | 下行 | 是：`sendPlayInfo` | 查询当前播放状态与截图 |
| `deletePlayFile` | 下行 | **否** | 按文件列表 name/id 删除本地视频或图片 |
| `videoControl` | 下行/上行 | 是：同 method | 控制直播、录播、停止、音量；也用于本地音量主动同步 |
| `linuxReboot` | 下行 | 是：同 method | 回执后约 1 秒重启设备 |
| `floorLimit` | 下行 | 是：同 method | 设置一个或多个楼层开放/限行状态并下发分层板 |
| `floorLimit.query` | 下行 | 是：`floorLimit` | 查询设备持久化的楼层开放/限行状态 |
| `floorSchedule` | 下行 | 是：同 method | 全量覆盖设备本地执行的楼层定时开放/限行规则 |
| `floorSchedule.query` | 下行 | 是：`floorSchedule` | 查询设备当前定时规则，`data=null` 不参与解析 |
| `floorSchedule.delete` | 下行 | 是：`floorSchedule` | 根据规则 hash 批量删除设备定时规则 |
| `elevatorMode` | 下行 | 是：同 method | 设置分层板最高8位的自定义电梯模式并立即下发 |
| `elevatorMode.query` | 下行 | 是：`elevatorMode` | 查询设备持久化的电梯模式状态 |
| `heartbeat` | 下行 | 是：同 method | 平台主动探测设备，响应严格回显请求 ID |
| `sync.fullPersonnel` | 下行 | 是：同 method | 全量新增/更新一个人员 |
| `sync.checkAllPersonHash` | 下行 | 是：同 method | 平台全量人员 hash 对账 |
| `sync.deletePersonnel` | 下行 | 是：同 method | 删除人员或指定凭证 |
| `face.requestImages` | 上行 | — | 设备主动索取缺失的人脸原图 |
| `face.responseImages` | 下行 | 是：`Imagesresult` | 平台下发人脸 Base64 原图 |
| `face.accessResult` | 上行 | 异步 `face.deductResult` | 本地人脸识别控梯实际结果 |
| `card.accessResult` | 上行 | 异步 `card.deductResult` | 本地刷卡控梯实际结果 |
| `face.deductResult` / `card.deductResult` | 下行 | **否** | 平台确认人脸/刷卡扣次并同步本地计数 |
| `remoteCall` | 下行 | 异步 `remoteCall.accessResult` | 远程单楼层呼梯 |
| `remoteCall.accessResult` | 上行 | 异步 `remoteCall.deductResult` | 设备报告 RS485 实际执行结果 |
| `remoteCall.deductResult` | 下行 | **否** | 平台确认远程呼梯扣次并同步本地计数 |
| `qr.scan` | 上行 | — | 设备报告扫码值，开始二维码事务 |
| `qr.scanResult` | 下行 | 无直接回执 | 平台返回初步校验结果 |
| `qr.floorControl` | 下行 | 异步 `qr.accessResult` | 平台下发楼层控制数据 |
| `qr.accessResult` | 上行 | — | 设备报告 RS485 实际执行结果 |
| `qr.deductResult` | 下行 | **否** | 平台返回扣次结果并结束事务 |

## 3. 多媒体与系统接口

### 3.1 查询本地视频和图片列表

下行 `getPlayFileList`：

```json
{
  "method": "getPlayFileList",
  "id": "play-list-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {},
  "time": "2026-08-12 14:30:00"
}
```

上行 `sendPlayFileList`：

```json
{
  "method": "sendPlayFileList",
  "id": "play-list-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "file": [
      { "id": 1, "name": "welcome" },
      { "id": 2, "name": "notice" }
    ]
  },
  "time": "2026-08-12 14:30:01"
}
```

`name` 是不带扩展名的文件基本名；`id` 是按本次目录扫描结果从 1 临时编号，不是稳定资源 ID。扫描固定 video 目录的视频及图片，并补充固定 pic 目录的图片；图片支持 `png/jpg/jpeg/bmp/gif/webp/tif/tiff`，不区分扩展名大小写。删除时使用同一条记录的 `name` 和 `id`，重新查询后使用新列表中的 ID。

### 3.2 查询当前播放信息

下行 `getPlayInfo` 与 3.1 的请求信封相同，仅替换 `method`。设备收到后先请求界面截图，截图回调完成后才发布：

```json
{
  "method": "sendPlayInfo",
  "id": "play-info-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "info": {
      "videoType": "recorded",
      "videoVolume": 50,
      "videoPath": "welcome.mp4",
      "videoUrl": "",
      "name": "welcome",
      "base64Image": "...",
      "deviceLog": ""
    }
  },
  "time": "2026-08-12 14:30:01"
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `videoType` | string | `none` / `live` / `recorded`，响应为小写 |
| `videoVolume` | integer | 当前音量，0–100 |
| `videoPath` | string | 当前 video 目录中的录播视频或图片文件名；仅有 pic 时为空 |
| `videoUrl` | string | 仅 `live` 时返回 URL，其他类型为空 |
| `name` | string | 当前 video 目录中资源的基础名；pic 不参与文件信息上报 |
| `base64Image` | string | 当前画面截图 Base64；代码未固定是否带 data URL 前缀 |
| `deviceLog` | string | 最近的播放、音量、下载状态或错误说明 |

当前实现没有截图等待超时回包；如果界面没有产生截图回调，前端可能收不到 `sendPlayInfo`，应设置业务超时。

### 3.3 删除本地视频或图片

```json
{
  "method": "deletePlayFile",
  "id": "delete-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "name": "welcome",
    "id": "1"
  },
  "time": "2026-08-12 14:30:00"
}
```

- `data.name` 与 `data.id` 对应最近一次 `sendPlayFileList` 中的同一条记录；名称不带扩展名，ID 兼容数值及整数字符串。video 目录的视频/图片及 pic 目录的图片均可删除，同名但不同目录/扩展名时由 ID 确定目标。
- 连续删除保留同一份列表中其余文件的原 ID；再次获取列表后使用新 ID。未携带 ID 的旧请求仅在名称唯一时删除。ID/名称不匹配时拒绝删除并记录日志。
- 当前实现无 MQTT 成功/失败回执。前端需要确认时，可随后调用 `getPlayFileList` 验证。

### 3.4 播放与音量控制 `videoControl`

通用请求字段：

| `data` 字段 | 类型 | 说明 |
|---|---|---|
| `videoType` | string | `LIVE` / `RECORDED` / `NONE`；请求建议大写 |
| `videoVolume` | integer/string | 可选；会转换为整数并限制到 0–100 |
| `videoUrl` | string | LIVE 必填 |
| `videoPath` | string | RECORDED 下载时必填；设备仅取文件名，目录部分会被移除 |
| `ftpPath` | string | RECORDED 下载的 FTP 主机 |
| `ftpPort` | string | RECORDED 下载的 FTP 端口；当前代码按字符串读取 |
| `ftpUser` | string | FTP 用户名 |
| `ftpPwd` | string | FTP 密码；字符串 `null` 会被当作空密码 |

LIVE 示例：

```json
{
  "method": "videoControl",
  "id": "video-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "videoType": "LIVE",
    "videoVolume": 60,
    "videoUrl": "rtsp://192.168.1.10/live"
  },
  "time": "2026-08-12 14:30:00"
}
```

RECORDED 下载示例：

```json
{
  "method": "videoControl",
  "id": "video-002",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "videoType": "RECORDED",
    "videoVolume": 50,
    "videoPath": "welcome.mp4",
    "ftpPath": "192.168.1.20",
    "ftpPort": "21",
    "ftpUser": "media",
    "ftpPwd": "secret"
  },
  "time": "2026-08-12 14:31:00"
}
```

控制回执仍为 `videoControl`，沿用请求 `id/deviceId`，保留原 `data` 后按场景补充：

```json
{
  "method": "videoControl",
  "id": "video-002",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "videoType": "RECORDED",
    "videoVolume": 50,
    "videoPath": "welcome.mp4",
    "ftpPath": "192.168.1.20",
    "ftpPort": "21",
    "ftpUser": "media",
    "ftpPwd": "secret",
    "status": "ok",
    "deviceClientIp": "192.168.1.100",
    "ftpDownload": "100%",
    "downloadResult": "downloaded",
    "reason": "FTP download completed"
  },
  "time": "2026-08-12 14:31:08"
}
```

| 回执字段 | 可能值/说明 |
|---|---|
| `status` | `ok` / `error`；部分“切回已有录播”场景代码可省略 |
| `deviceClientIp` | 当前设备 IP |
| `ftpDownload` | `100%` / `0%`；不是连续进度值，部分场景省略 |
| `downloadResult` | `downloaded`、`already_exists`、`retryable_error`、`local_rejected`、`storage_limited`、`setup_error`、`download_busy` |
| `reason` | 人类可读原因，可省略 |

行为约定：

- `LIVE`：要求 `videoUrl` 非空；支持识别 RTSP、RTMP(S)、SRT、UDP、HLS 和 HTTP-TS。录播下载中切换 LIVE 会返回 `status=error, downloadResult=download_busy`。
- `RECORDED` 且有 `videoPath`：回执延迟到 FTP 最终结果。同一下载任务的重复请求会合并，后一次请求上下文将成为最终回执上下文；并发的其他下载任务会被拒绝。
- `RECORDED` 且无 `videoPath`：停止直播并恢复已有本地录播，可同时调音量；不带 `ftpDownload`。
- `NONE`：停止全部播放。
- 未知 `videoType`：当前代码仍会停止全部播放，前端不得依赖此容错。
- 携带 `videoVolume` 的非下载请求会等待实际音量设置完成后再回执。
- 设备本地手动调音量时也会主动上行 `videoControl`，其 `id` 为当前毫秒时间戳，前端应把“无对应请求 ID 的 videoControl”视为设备状态事件。

### 3.5 远程重启

```json
{
  "method": "linuxReboot",
  "id": "reboot-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {},
  "time": "2026-08-12 14:30:00"
}
```

设备先上行同 ID 回执，再约 1 秒后重启：

```json
{
  "method": "linuxReboot",
  "id": "reboot-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": { "status": "ok" },
  "time": "2026-08-12 14:30:00"
}
```

重复重启请求在设备已进入等待重启状态后会被忽略且无第二次回执。

### 3.6 楼层开放/限行 `floorLimit`

平台设置一个或多个楼层，`action=1` 表示开放，`action=0` 表示限行：

```json
{
  "method": "floorLimit",
  "id": "a7b8c9d0-e1f2-3456-abcd-567890123456",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "floors": [
      { "floor": 1, "action": 1 },
      { "floor": 2, "action": 0 }
    ],
    "time": "2026-08-26 17:30:00"
  }
}
```

设备保存状态、逐层提交 RS485 后，在 event Topic 回报；`id` 原样回显：

```json
{
  "method": "floorLimit",
  "id": "a7b8c9d0-e1f2-3456-abcd-567890123456",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "code": 0,
    "floors": [
      { "floor": 1, "status": 1 },
      { "floor": 2, "status": 0 }
    ],
    "floorHex": "0x00000000000000000000000000000001"
  },
  "time": "2026-08-26 17:30:01"
}
```

处理约定：

- 楼层范围与 online_v2 相同：`-8..-1、1..120`，不接受 0、小数或字符串。
- 同一请求内重复的同楼层同 action 只执行一次；同楼层出现冲突 action 时整笔拒绝。
- 无论本次设置一个还是多个楼层，设备都先更新本地完整楼层状态，再发送一次 `OPEN` 加128个状态字符，共132字节。分层板位序为 `-8..-1、1..120`，帧内状态字符为 `0=开放、1=限行`，其余未操作楼层保持数据库中的当前状态。
- 状态保存到 online_v1 独立配置项 `online_v1_floor_limit_bits`，不调用也不修改 online_v2 的接收入口和楼层状态。存储位与上行 `status` 均使用 `1=开放、0=限行`。
- `floorHex` 是 128 位“开放状态”十六进制数，带 `0x` 前缀。1～120 楼对应 bit0～bit119，所以仅 1 楼开放时末位为 `1`；`-8..-1` 依次使用 bit120～bit127。
- `code=0` 表示状态已保存且 RS485 请求已提交；参数或保存错误时返回 `code=-1`。该回执不等待串口驱动的最终异步发送结果。

查询请求：

```json
{
  "method": "floorLimit.query",
  "id": "b8c9d0e1-f2a3-4567-bcde-678901234567",
  "deviceId": "YCEEQZA10C1FFC62"
}
```

`floorLimit.query` 请求本身没有 `data` 字段，设备不会读取或解析 `data`。查询响应使用 `method=floorLimit` 并回显查询 ID；`data.code=0`，`floors` 返回 1～120、-8～-1 共128个当前状态，`floorHex` 与设置回执采用同一编码。查询只读本地持久化状态，不下发 RS485。

### 3.7 定时楼层开放/限行 `floorSchedule`

平台全量覆盖设备原有全部定时规则；`schedules=[]` 表示清空：

```json
{
  "method": "floorSchedule",
  "id": "a3b4c5d6-e7f8-9012-3456-789abcdef012",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "schedules": [
      { "action": 1, "time": "07:00", "weekDays": [1, 2, 3, 4, 5], "floors": [3, 5], "hash": "2ce839d19e99f591" },
      { "action": 0, "time": "22:30", "weekDays": [1, 2, 3, 4, 5], "floors": [3, 5], "hash": "8c329fcea568ee41" }
    ],
    "time": "2026-08-27 12:00:00"
  },
  "time": "2026-08-27 12:00:00"
}
```

保存成功后回显原请求 ID，并返回设备数据库当前生效的完整规则：

```json
{
  "method": "floorSchedule",
  "id": "a3b4c5d6-e7f8-9012-3456-789abcdef012",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "code": 200,
    "schedules": [
      { "action": 1, "time": "07:00", "weekDays": [1, 2, 3, 4, 5], "floors": [3, 5], "hash": "2ce839d19e99f591" },
      { "action": 0, "time": "22:30", "weekDays": [1, 2, 3, 4, 5], "floors": [3, 5], "hash": "8c329fcea568ee41" }
    ]
  },
  "time": "2026-08-27 12:00:01"
}
```

规则约定：

- `action=1` 开放，`action=0` 限行；`time` 必须为严格的 `HH:mm`。
- `weekDays` 使用 Qt/ISO 星期：`1=星期一`，`7=星期日`；必须是非空数组。
- `floors` 必须是非空数组，范围为 `-8..-1、1..120`。
- `hash` 必须是非空字符串，并且在本次全量列表中唯一；设备原样保存，用作删除规则的稳定标识。
- 同一楼层在相同时间且星期范围重叠时，不允许配置不同 action；整笔规则会返回 `code=400` 且不覆盖旧数据。
- 规则、定时楼层当前状态和最后执行分钟分别保存到 SQLite 配置项 `online_v1_floor_schedules`、`online_v1_floor_schedule_bits`、`online_v1_floor_schedule_last_minute`。
- `floorLimit` 与 `floorSchedule` 不互斥。`floorLimit` 享有即时控制权限，即使楼层存在定时规则，也能随时开放或限行；到达后续定时时间时，再由对应定时规则覆盖该楼层状态。
- 新加入定时管理的楼层以设备当前开放/限行状态初始化；保存规则本身不改变当前状态，也不下发485。
- 设备每5秒按本地日期、星期和 `HH:mm` 检查一次。同一分钟命中的全部规则只执行一次，执行分钟持久化，进程在该分钟内重启也不会重复下发。
- 无论本次命中一个还是多个楼层，都以设备当前全部楼层状态为基础更新命中楼层，并发送一次 `OPEN + 128位状态` 的132字节全量帧，避免7字节单层协议的高楼层板号兼容问题，同时保持其他楼层最近一次控制结果。
- 定时执行属于设备本地任务；即使 MQTT 暂时断开，只要设备仍处于 online_v1 配置，规则仍会按设备时间执行。

查询请求中的 `data` 为 `null`，设备不读取其内容：

```json
{
  "method": "floorSchedule.query",
  "id": "b4c5d6e7-f890-1234-5678-9abcdef01234",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": null,
  "time": "2026-08-27 12:00:00"
}
```

查询响应统一使用 `method=floorSchedule`、`code=200`，回显查询 ID，并在 `schedules` 返回数据库中完整规则列表。参数错误返回 `code=400`，数据库保存失败返回 `code=500`，失败响应会保留当前旧规则并增加 `message`。

按 hash 删除一个或多个定时规则：

```json
{
  "method": "floorSchedule.delete",
  "id": "548aaf559bd549f58abf1f1772defded",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "hashList": ["2ce839d19e99f591"],
    "schedules": [],
    "time": "2026-08-27 16:45:32"
  },
  "time": "2026-08-27 16:45:32"
}
```

设备只解析 `hashList`，请求中的 `schedules` 不参与删除判断。删除不会立即下发485，也不会改变楼层当前开放/限行状态；后续只执行剩余规则。不存在的 hash 按幂等成功处理。

删除回执统一使用 `method=floorSchedule` 并回显删除请求 ID；`data` 只返回 `code` 和删除后的完整 `schedules`：

```json
{
  "method": "floorSchedule",
  "id": "548aaf559bd549f58abf1f1772defded",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "code": 200,
    "schedules": [
      { "action": 0, "time": "22:30", "weekDays": [1, 2, 3, 4, 5], "floors": [3, 5], "hash": "8c329fcea568ee41" }
    ]
  },
  "time": "2026-08-27 16:45:33"
}
```

### 3.8 自定义电梯模式 `elevatorMode`

平台设置总开关和8个模式位：

```json
{
  "method": "elevatorMode",
  "id": "0bff6f034719479b80ae04fa83318631",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "masterSwitch": 1,
    "delay": 0,
    "parking": 0,
    "driver": 0,
    "independent": 0,
    "ext1": 0,
    "ext2": 0,
    "ext3": 0,
    "ext4": 0,
    "time": "2026-08-31 10:05:06"
  },
  "time": "2026-08-31 10:05:06"
}
```

`masterSwitch`及8个模式字段都必须是整数0或1。设置成功后保存到SQLite配置项 `online_v1_elevator_mode`，并立即把当前楼层权限与模式状态组合后下发485。

模式位使用板端128位的最高8位：

| 板端位索引 | 字段 | 总开关开启时的值 |
|---:|---|---|
| 120 | `delay` | 1=启用，0=关闭 |
| 121 | `parking` | 1=启用，0=关闭 |
| 122 | `driver` | 1=启用，0=关闭 |
| 123 | `independent` | 1=启用，0=关闭 |
| 124 | `ext1` | 1=启用，0=关闭 |
| 125 | `ext2` | 1=启用，0=关闭 |
| 126 | `ext3` | 1=启用，0=关闭 |
| 127 | `ext4` | 1=启用，0=关闭 |

- `masterSwitch=1`：132字节持续状态帧保留0～119位业务数据，并把120～127位替换为数据库模式状态；36字节一次性呼梯/通行帧保留0～119位并把120～127位清零，避免误触发模式通道。
- `masterSwitch=0`：模式覆盖完全停用，所有帧保持原有楼层权限逻辑，113～120层恢复使用原始楼层权限。
- 原始128位楼层权限始终独立保存；模式开启期间不会删除113～120层原有权限。
- `OPEN + 128字符`帧使用数据库模式值覆盖最后8个字符；`#@ + 32HEX + #!`帧把最后两个HEX字符固定为 `00`。模式开启期间113～120不再作为普通呼梯/通行楼层使用。
- `floorLimit` 和 `floorSchedule` 无论单层、多层均直接发送132字节全量帧；模式开启时，最终串口出口继续用数据库模式状态覆盖最高8位，楼层操作不能直接修改模式位。
- 保护在RS485驱动最终发送入口统一执行，覆盖 `floorLimit`、`floorSchedule`、`remoteCall`、人员/访客同步后的刷卡、人脸、密码、二维码通行以及其他36字节楼层帧。通行类帧不会携带数据库模式值；继电器、时间同步等非楼层帧不修改。
- `masterSwitch=1` 时，如果 `remoteCall` 的目标楼层，或刷卡、人脸、密码、人员二维码、访客二维码的一次授权楼层全部位于113～120，设备会在RS485发送前拒绝，并以 `success=false, reason="楼层已被电梯模式占用"` 上报。授权中只要同时包含任一113楼以下楼层或地下楼层，就保持原有逻辑继续执行，不因同时包含113～120楼而整笔拒绝。

查询请求：

```json
{
  "method": "elevatorMode.query",
  "id": "af266479a7fa48e88e97a143728742e2",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {},
  "time": "2026-08-31 10:07:29"
}
```

设置和查询均回复 `method=elevatorMode` 并回显请求ID：

```json
{
  "method": "elevatorMode",
  "id": "af266479a7fa48e88e97a143728742e2",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "code": 200,
    "masterSwitch": 1,
    "delay": 1,
    "parking": 0,
    "driver": 0,
    "independent": 0,
    "ext1": 0,
    "ext2": 0,
    "ext3": 0,
    "ext4": 0
  },
  "time": "2026-08-31 10:07:30"
}
```

### 3.9 平台探活 `heartbeat`

平台发送空 `data` 对象：

```json
{
  "method": "heartbeat",
  "id": "a1b2c3d4-e5f6-7890-abcd-ef0123456789",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {},
  "time": "2026-08-26 12:00:00"
}
```

设备立即返回同 method，并严格回显请求 `id`：

```json
{
  "method": "heartbeat",
  "id": "a1b2c3d4-e5f6-7890-abcd-ef0123456789",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "code": 200,
    "time": "2026-08-26 12:00:01"
  },
  "time": "2026-08-26 12:00:01"
}
```

`data.time` 与报文顶层 `time` 由设备在同一时刻生成，格式均为 `yyyy-MM-dd HH:mm:ss`。探活只确认当前设备进程能够接收并发布 MQTT 业务消息，不触发 RS485 或其他硬件动作。

## 4. 人员、人脸与呼梯接口

### 4.1 人员同步统一回执

三个 `sync.*` 接口都以原 `method/id/deviceId` 回执：

```json
{
  "method": "sync.fullPersonnel",
  "id": "sync-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "status": "SUCCESS",
    "code": "APPLIED",
    "message": "网络人员数据已保存",
    "personId": "P10001",
    "resultcode": 0
  },
  "time": "2026-08-12 14:30:01"
}
```

| 字段 | 说明 |
|---|---|
| `status` | 成功为 `SUCCESS`，失败实际为 `ERROR` |
| `code` | 细分文本代码，例如 `APPLIED`、`HASH_UNCHANGED`、`INVALID_PAYLOAD` |
| `message` | 结果说明 |
| `personId` | 有人员上下文时存在 |
| `resultcode` | **成功为 `0`，失败为 `-1`** |
| `duplicate` | 命中幂等缓存时为 `true` |

前端不要只按 HTTP 风格的正数判断失败，也不要使用旧文档中的失败值 `1`；当前源码明确返回 `-1`。

### 4.2 全量同步单个人员 `sync.fullPersonnel`

最小必需字段为信封 `id`、`data.personId`、`data.personHash`。完整结构如下：

```json
{
  "method": "sync.fullPersonnel",
  "id": "sync-person-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "personId": "P10001",
    "personHash": "aggregate-hash-v1",
    "person": {
      "name": "张三",
      "phone": "13800000000",
      "idCard": "",
      "employeeNo": "E001",
      "department": "研发部",
      "position": "工程师",
      "hireDate": 1786464000000,
      "personType": "EMPLOYEE",
      "status": 1,
      "remark": ""
    },
    "faces": [
      { "faceHash": "face_hash_001", "name": "front", "bindTime": "2026-08-12 10:00:00" }
    ],
    "icCards": [
      { "cardId": "00112233", "name": "工卡", "bindTime": "2026-08-12 10:00:00" }
    ],
    "qrCodes": [
      {
        "qrUnique": "qr-business-key",
        "qrName": "员工码",
        "qrType": "CREDENTIAL",
        "purpose": "ACCESS",
        "validFrom": "2026-08-12 00:00:00",
        "validTo": "2027-08-12 23:59:59",
        "maxUseCount": 100,
        "visitorName": "",
        "visitorPhone": "",
        "bindTime": "2026-08-12 10:00:00"
      }
    ],
    "rules": {
      "time": true,
      "count": true,
      "relay": false,
      "amount": false,
      "duration": true,
      "remoteCall": true,
      "remoteCallDataCurrentDayCallCount": 12,
      "remoteCallDataEveryDayUpdateCallCount": 13,
      "cardPassword": true,
      "cardPasswordDataPassword": "123456",
      "controlElevator": true,
      "countDataTotal": 100,
      "countDataAddCount": 0,
      "amountDataTotal": 0,
      "amountDataAddAmount": 0,
      "amountDataUnitPrice": 0,
      "durationDataStartTime": "2026-08-12 00:00:00",
      "durationDataEndTime": "2027-08-12 23:59:59",
      "controlElevatorDataValueType": 1,
      "timingRules": [
        { "days": [1, 2, 3, 4, 5], "timeRange": ["08:00:00", "20:00:00"] }
      ]
    },
    "floors": [
      { "deviceId": "YCEEQZA10C1FFC62", "floor": [1, 2, 3] }
    ]
  },
  "time": "2026-08-12 14:30:00"
}
```

`icCards/qrCodes/rules/floors` 按完整快照替换；`faces` 按 `faceHash` 差量保留已有图片和特征。成功回执还可能包含 `personHash`、`changed`、`faceCount`、`icCardCount`、`qrCodeCount`。若人员聚合 hash、人脸 hash 集合及本地 READY 状态均未变化，返回 `code=HASH_UNCHANGED, changed=false`。

`rules.cardPasswordDataPassword` 是当前人员唯一的通行密码，一个人员不支持配置多个密码。`rules.cardPassword=true` 时，人脸主界面的“密码通行”会用该密码查找人员，并执行与人脸/刷卡一致的通行期限、定时规则、次数、金额和楼层权限校验。校验通过后才发送 36 字节 RS485 楼层帧；密码错误、密码重复对应多人或其他权限校验失败时不发送 RS485。设备日志不记录密码明文。

`rules.count` 与 `rules.amount` 独立生效：仅 `count=true` 时只判断并扣减一次 `countDataTotal` 对应的剩余次数，不读取金额；`amount=true` 时要求 `amountDataUnitPrice>0`，每次实际完成 RS485 通行后从剩余金额扣减一个单价。两者同时开启时必须同时满足“剩余次数大于 0”和“剩余金额不少于一个单价”，成功后在同一笔本地事务内扣次数 1、扣金额一个单价。剩余金额为 0 或不足一个单价时按 `code=4201` 显示“通行余额不足，请联系管理员充值”，拒绝通行且不发送 RS485。

`rules.duration` 与 `rules.time` 是两套互相独立的开关：`duration=true` 时校验 `durationDataStartTime/durationDataEndTime` 总有效期限；`time=true` 时校验 `timingRules` 中的星期和每日时间段。两者同时启用时必须同时通过，任一校验失败都不会发送 RS485 楼层帧。期限和定时数据只在主设备本地参与判断，不会编码进下发分层板的 36 字节楼层帧。

同步成功后，设备检查缺失图片；有缺图时立即主动上行 `face.requestImages`：

```json
{
  "method": "face.requestImages",
  "id": "设备生成的32位无连字符UUID",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "personId": "P10001",
    "faceHashes": ["face_hash_001"]
  },
  "time": "2026-08-12 14:30:01"
}
```

### 4.3 全量人员 hash 对账 `sync.checkAllPersonHash`

```json
{
  "method": "sync.checkAllPersonHash",
  "id": "hash-check-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "personIdHashes": {
      "P10001": "aggregate-hash-v1",
      "P10002": "aggregate-hash-v3"
    }
  },
  "time": "2026-08-12 14:30:00"
}
```

成功回执 `data` 在统一字段外增加：

```json
{
  "missing": ["平台有、设备无的personId"],
  "changed": ["双方都有但hash不同的personId"],
  "matched": ["双方hash一致的personId"],
  "extra": ["设备有、平台清单无的personId"],
  "inferredDeletedCount": 1
}
```

重要副作用：`extra` 中的本地人员会被标记为已删除。因此前端必须发送**平台完整清单**，不能把此接口当作局部查询。

### 4.4 删除人员或凭证 `sync.deletePersonnel`

```json
{
  "method": "sync.deletePersonnel",
  "id": "delete-person-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "personId": "P10001",
    "deleteType": "FACE",
    "personHash": "aggregate-hash-after-delete",
    "items": ["face_hash_001"]
  },
  "time": "2026-08-12 14:30:00"
}
```

| `deleteType` | `items` | 行为 |
|---|---|---|
| `ALL` / `PERSON` | 不要求 | 删除全部子数据并把人员标记删除 |
| `IC_CARD` | 必须非空 | `items` 为 `cardId` 数组 |
| `FACE` | 必须非空 | `items` 为 `faceHash`；也兼容 Base64 data URL |
| `QR_CODE` | 必须非空 | `items` 为 `qrUnique` 数组 |

成功回执增加 `deleteType`、`processedItemCount`，部分删除且请求带新 `personHash` 时也会返回它。

### 4.5 人脸图片下发 `face.responseImages`

平台收到 `face.requestImages` 后，下发：

```json
{
  "method": "face.responseImages",
  "id": "face-images-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "personId": "P10001",
    "faces": [
      {
        "name": "front",
        "faceHash": "face_hash_001",
        "faceBase64": "data:image/jpeg;base64,/9j/4AAQ..."
      }
    ]
  },
  "time": "2026-08-12 14:30:02"
}
```

约束：

- `personId` 必须存在且未删除；`faces` 必须是非空数组。
- `faceHash` 允许 `[A-Za-z0-9_-]`，长度 1–128，并必须匹配此前 `fullPersonnel.faces` 中的记录。
- `faceBase64` 可为纯 Base64 或 `data:image/...;base64,` 格式；内容必须能解码为图片。
- 同一人员一次只能有一个图片校验任务；设备校验超时为 30 秒。
- 设备进行单人脸、检测置信度、图像质量和特征提取校验；此链路不做活体检测。

最终按下发图片逐张上行 `Imagesresult`。同一批图片正常沿用同一个下行 ID，通过 `faceHash` 区分每张图片：

```json
{
  "method": "Imagesresult",
  "id": "face-images-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "personId": "P10001",
    "name": "front",
    "faceHash": "face_hash_001",
    "result": false,
    "msg": "人脸质量过低，请重新录入人脸。",
    "failed_faces": [
      { "name": "front", "faceHash": "face_hash_001" }
    ]
  },
  "time": "2026-08-12 14:30:05"
}
```

每张图片都有独立的 `result` 和 `msg`；成功图片 `result=true` 且不含 `failed_faces`，失败图片的 `failed_faces` 最多包含当前这一张。失败消息可能包括无脸、多脸、置信度/质量不足、特征提取失败、服务未就绪、图片保存失败、重复人脸或校验超时。不同人员重复时消息显示冲突人员姓名；同一 `personId` 下存在相似人脸时仍按原规则保留最新下发图片，但最新图片及本批被替换图片的 `msg` 会使用人员姓名说明替换结果，不暴露内部 `personId`。前端应使用 `faceHash + result` 判断单图结果，不要对中文 `msg` 做业务分支。

### 4.6 人脸识别控梯结果 `face.accessResult`

本地识别人脸并实际完成 RS485 控梯后，设备主动上报：

```json
{
  "method": "face.accessResult",
  "id": "设备生成的UUID",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "personId": "P10001",
    "floors": [1, 2],
    "floorText": "1,2",
    "success": true,
    "rs485": "2340...2321",
    "floorHex": "32位大写十六进制楼层位图",
    "reason": "可选失败原因"
  },
  "time": "2026-08-12 14:30:05"
}
```

`floorHex` 仅在 RS485 帧恰好为 36 字节时存在；`reason` 为空时省略。

人脸实际完成 RS485 后，设备先按已启用规则原子扣减本地次数/金额，再发送 `success=true`。平台后续返回的 `face.deductResult` 作为权威值校准，不会再次按增量重复扣减。

### 4.6.1 刷卡控梯结果 `card.accessResult`

刷卡权限验证并完成 RS485 控梯后，设备主动上报：

```json
{
  "method": "card.accessResult",
  "id": "设备生成的UUID",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "personId": "P10001",
    "cardId": "1063318622",
    "floors": [1, 2],
    "floorText": "1,2",
    "success": true,
    "rs485": "2340...2321",
    "floorHex": "32位大写十六进制楼层位图",
    "reason": "可选失败原因"
  },
  "time": "2026-08-12 14:30:05"
}
```

刷卡实际完成 RS485 后同样先原子扣减本地次数/金额，再上报 `success=true`；断网分支只累计批量补报记录，不会再次扣减。

### 4.6.2 平台返回人脸/刷卡扣次结果

推荐分别使用 `face.deductResult` 和 `card.deductResult`；设备还兼容
`ic.deductResult`、`access.deductResult` 和 `deductResult`：

```json
{
  "method": "face.deductResult",
  "id": "全局唯一扣次结果ID",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "personId": "P10001",
    "code": 200,
    "deducted": true,
    "message": "次数和金额扣减完成",
    "remainingCount": 29,
    "usedCount": 3,
    "remainingAmount": 7.5,
    "usedAmount": 1.5
  },
  "time": "2026-08-12 14:30:06"
}
```

当 `code=200`、`deducted=true` 且人员开启次数规则时，`remainingCount` 和 `usedCount` 必须是非负 JSON 数字；设备以平台值校准本地剩余次数和已用次数。人员开启金额规则时，平台可同时返回非负 JSON 数字 `remainingAmount/usedAmount`，设备据此校准本地金额；未返回这两个金额字段时保留设备已经完成的本地金额扣减。设备也兼容 `passRemainingAmount/passUsedAmount` 字段名。顶层 `id` 用于幂等，重复回执不会重复应用；较晚到达且已用次数或已用金额更小的旧回执不会把本地状态倒退。`deducted=false` 时记录为成功跳过，不修改本地状态。

### 4.6.3 MQTT 断线通行的批量补报

本节的“断线”是指设备仍运行在 `online_v1`，但网络链路/外网不可用，或者与 MQTT Broker 的连接已经断开；不是 `offline_v1` 运行模式。两种状态任意一个成立都按离线通行处理。网络和 MQTT 状态变化只更新统一离线标志，不直接写入通行次数；每次刷卡、二维码、人脸或密码实际完成 RS485 通行后只扣减一次本地规则状态、记录一次批量补报，因此两个断线条件重复触发不会造成重复计数或重复扣款。二维码在该状态下使用本地人员、规则、有效期和楼层权限完成校验。

网络恢复且 Broker 恢复连接后，设备按通行类型分别发送一条批量 `accessResult`。发生过网络断开后，即使设备内仍保留旧的 MQTT“已连接”状态，也不会在网络刚恢复时直接补报；设备会先向 mqttd 重新查询连接状态，收到断网后的 MQTT 已连接确认才发送。四种 method 为 `card.accessResult`、`qr.accessResult`、`face.accessResult` 和 `password.accessResult`；没有待报记录的类型不发送：

```json
{
  "method": "password.accessResult",
  "id": "设备生成的UUID",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "records": [
      {
        "personId": "P10001",
        "success": true,
        "count": 3
      },
      {
        "personId": "P10002",
        "success": true,
        "count": 2
      }
    ]
  },
  "time": "2026-08-12 18:00:00"
}
```

`count` 是该人员在本次断线待报期间通过相应方式实际成功通行的累计次数。消息成功提交给 mqttd 后，设备按本次快照消费本地待报数；补报过程中新增的通行次数继续保留，避免误删。本功能不新增对批量消息的 `deductResult` 关联或等待逻辑。

### 4.7 远程呼梯 `remoteCall`

```json
{
  "method": "remoteCall",
  "id": "remote-call-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "personId": "P10001",
    "floor": 8,
    "source": "platform"
  },
  "time": "2026-08-12 14:30:00"
}
```

`floor` 必须是 JSON 数字且为整数，范围为 `-8..-1` 或 `1..120`，不允许 `0`，也不接受字符串 `"8"`。`id/deviceId/personId` 均不能为空，且 `deviceId` 必须与本机一致。

设备在 RS485 驱动返回实际结果后上报 `remoteCall.accessResult`。顶层 `id` 由设备重新生成；`success` 反映本次 RS485 结果。模式总开关开启且目标为113～120楼时不下发RS485，直接上报 `success=false`，并增加 `reason="楼层已被电梯模式占用"`：

```json
{
  "method": "remoteCall.accessResult",
  "id": "设备生成的UUID",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "personId": "P10001",
    "floor": 8,
    "success": true,
    "source": "platform"
  },
  "time": "2026-08-12 14:30:05"
}
```

RS485 成功时，如果该人员配置了远程呼梯规则且本地当日剩余次数大于 0，设备先把 `remoteCallDataCurrentDayCallCount` 预扣 1，并持久化待平台确认标志。随后平台返回：

```json
{
  "method": "remoteCall.deductResult",
  "id": "全局唯一扣次结果ID",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "code": 200,
    "deducted": true,
    "message": "次数扣减完成",
    "personId": "P10001",
    "floor": 8,
    "remainingCount": 11,
    "usedCount": 1,
    "passRemainingCount": 14,
    "passUsedCount": 6
  },
  "time": "2026-08-12 14:30:06"
}
```

`code=200,deducted=true` 时，`remainingCount/usedCount` 必须是非负整数，设备以平台值覆盖本地远程呼梯剩余次数和已用次数；可选的 `passRemainingCount/passUsedCount` 必须成对出现，存在时同步人员通行总次数。处理完成后清除待确认标志。

平台返回 `code=200,deducted=false`（例如未配置远程呼梯规则）或失败码（例如 `4500`）时，设备只记录跳过/失败状态并清除待确认标志，已经发生的本地预扣不恢复。`remoteCall.deductResult.id` 用于幂等，重复结果不会再次处理。同一人员、同一楼层存在未完成事务时，设备不会接受新的不同 ID 呼梯请求，以免无法关联不带原请求 ID 的扣次结果。

## 5. online_v1 二维码授权流程

```mermaid
sequenceDiagram
    participant D as 设备
    participant P as 平台/前端
    D->>P: qr.scan (qrCode, scanId)
    P->>D: qr.scanResult (初步授权)
    P->>D: qr.floorControl (楼层与floorHex)
    Note over D: 校验数据并执行RS485
    D->>P: qr.accessResult (实际开启结果)
    P->>D: qr.deductResult (扣次确认)
```

同一时刻设备只接受一个二维码事务；默认每个阶段超时约 10 秒。前端应低延迟返回，并对重复消息保持相同 `id` 和内容。

### 5.1 设备上报扫码 `qr.scan`

```json
{
  "method": "qr.scan",
  "id": "设备生成的scanId UUID",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": { "qrCode": "扫码原始内容" },
  "time": "2026-08-12 14:30:00"
}
```

前端应保存 `qrCode` 与该事务的 `scanId`。当前后续下行使用各自消息 ID，并没有强制要求把 `scanId` 放回信封；事务主要以当前设备上的单笔待处理状态和 `qrCode` 关联。

### 5.2 平台返回初步结果 `qr.scanResult`

```json
{
  "method": "qr.scanResult",
  "id": "scan-result-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "code": 200,
    "success": true,
    "reason": "",
    "data": {
      "authorizationId": "可选平台授权上下文"
    }
  },
  "time": "2026-08-12 14:30:01"
}
```

只有 `code == 200 && success == true` 才视为通过。内层 `data` 当前只存入事务审计，不参与楼层控制。拒绝结果会结束尚未执行 RS485 的事务，无额外 MQTT 回执。

### 5.3 平台下发楼层 `qr.floorControl`

CREDENTIAL 示例：

```json
{
  "method": "qr.floorControl",
  "id": "floor-control-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "qrCode": "扫码原始内容",
    "sourceType": "CREDENTIAL",
    "personId": "P10001",
    "invitationId": "",
    "floors": [1, 8],
    "floorHex": "00000000000000000000000000000000"
  },
  "time": "2026-08-12 14:30:01"
}
```

| 字段 | 约束 |
|---|---|
| `qrCode` | 必须与当前待处理扫码完全一致 |
| `sourceType` | 只能为 `CREDENTIAL` 或 `VISITOR`，设备会转成大写 |
| `personId` | CREDENTIAL 必填；VISITOR **不得携带非空 personId** |
| `invitationId` | 访客邀请标识；当前解析不强制非空 |
| `floors` | 非空数组；元素可为 JSON 整数或整数字符串；范围 `-8..-1, 1..120` |
| `floorHex` | 32 位十六进制，或 `#@` + 32 位十六进制 + `#!` |

设备会从 `floors` 重新生成 RS485 楼层位图并与 `floorHex` 比较，二者必须完全一致。上例的 `floorHex` 仅为格式占位，联调时必须使用与实际楼层算法一致的值。

### 5.4 设备上报执行结果 `qr.accessResult`

```json
{
  "method": "qr.accessResult",
  "id": "设备生成的UUID",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "qrCode": "扫码原始内容",
    "success": true
  },
  "time": "2026-08-12 14:30:02"
}
```

此结果表示楼层数据校验和 RS485 实际执行结果。`success=false` 且存在明确失败原因时会携带可选 `reason`；模式总开关开启且 `floors` 全部位于113～120时，设备不下发RS485，回报 `reason="楼层已被电梯模式占用"`。CREDENTIAL 与 VISITOR 使用相同判断；混合包含低楼层与113～120楼时保持原有执行逻辑。

### 5.5 平台返回扣次结果 `qr.deductResult`

```json
{
  "method": "qr.deductResult",
  "id": "deduct-001",
  "deviceId": "YCEEQZA10C1FFC62",
  "data": {
    "qrCode": "扫码原始内容",
    "sourceType": "CREDENTIAL",
    "personId": "P10001",
    "code": 200,
    "deducted": true,
    "message": "扣次成功",
    "remainingCount": 99,
    "usedCount": 1
  },
  "time": "2026-08-12 14:30:03"
}
```

`remainingCount/usedCount` 只有 JSON 数字才会应用。该消息用于同步本地次数并结束当前二维码事务，设备不再发送 MQTT 回执。成功但业务无需扣次时，可用 `code=200, deducted=false`，设备会记录为成功跳过。

### 5.6 二维码错误码

| code | 设备显示含义 |
|---:|---|
| 4000 | 二维码参数错误 |
| 4001 | 二维码无效 |
| 4002 | 二维码格式错误 |
| 4003 | 二维码记录不存在 |
| 4100 | 二维码已停用 |
| 4101 | 二维码尚未生效 |
| 4102 | 二维码已过期 |
| 4200 | 通行次数已用完 |
| 4201 | 通行余额不足 |
| 4202 | 二维码使用次数已用完 |
| 4300 | 当前设备无通行权限 |
| 4301 | 未配置可通行楼层 |
| 4400 | 访客邀请已失效 |
| 4401 | 访客邀请未配置设备 |
| 4402 | 当前设备不在访客可通行列表 |
| 4403 | 访客邀请未配置楼层 |
| 4500 | 设备开启失败 |
| 4501 | 次数扣减失败 |

未知 code 优先显示平台 `reason/message`，为空时显示“二维码处理失败”。

## 6. 前端实现建议与已知边界

1. 按 Topic 中的 `{deviceId}` 建立设备会话，使用 `method + id` 关联请求和响应；主动事件不能假设存在前端请求上下文。
2. 对有回执接口设置合理超时：普通查询可先按 5–10 秒，图片校验至少覆盖设备 30 秒校验窗口，FTP 下载按文件大小单独配置。
3. 不要用新 ID 自动重发 `linuxReboot`、`remoteCall` 或人员写入请求。`remoteCall` 与人员同步重试都应复用原 ID 和完全相同的内容。
4. `deletePlayFile`、`remoteCall.deductResult`、`qr.deductResult` 当前无后续业务回执；`qr.scanResult` 也无直接回执。前端应以 `remoteCall.accessResult` 判断设备执行结果，并在其后发送扣次结果。
5. `sync.checkAllPersonHash` 会删除平台清单之外的本地人员，必须由有权限的全量同步任务调用。
6. `Imagesresult` 按人脸逐条发布，`data.faceHash` 标识当前图片；兼容字段 `failed_faces` 使用下划线命名且失败时最多包含当前一张，请按实际字段解析。
7. online_v1 当前路由会清空独立 `streamUrlTopic`，所以“无 method、只含 URL”的历史流地址消息在标准 online_v1 配置下不会命中；直播应使用 `videoControl`。
8. online_v1 的 `protocolMode` 在主程序中为 `offline`，用于关闭旧 `IcMqttGateway`。`openFloor`、`registerCardOne`、`sendQrCode`、`keep` 等旧 IC 方法属于 online_v2，不是本协议接口。
9. 源码中没有 MQTT Payload 版本号。前后端发布协议变更时，建议后续增加显式 `protocolVersion` 或在 Topic 中做版本隔离。
10. `floorLimit` 的确认表示本地状态已保存并已把 RS485 发送请求交给硬件队列，不表示分层板已经返回确认。

## 7. 联调验收清单

- [ ] 能订阅 `device/ycLinux/{deviceId}/event`，并向对应 `/request` 发布 JSON 对象。
- [ ] `getPlayFileList`、`getPlayInfo` 能按原 ID 收到响应。
- [ ] `heartbeat` 回包的 method、deviceId 与 ID 正确，且 data.time 和顶层 time 一致。
- [ ] `floorLimit` 的开放、限行、混合批次、重复楼层、冲突 action、边界楼层和错误参数均验证；单楼层、多楼层均为一次 `OPEN + 128位状态` 的132字节全量帧。
- [ ] `floorLimit.query` 能在进程重启后返回持久化状态，1楼开放时 floorHex 最低位为 1。
- [ ] `floorSchedule` 全量覆盖、空数组清空、hash 持久化、重复 hash、规则查询、星期边界、时间格式和同时间冲突均验证。
- [ ] `floorSchedule.delete` 单个/批量删除、重复 hash、未知 hash 幂等和删除后重启持久化均验证。
- [ ] 已配置定时规则的楼层仍可由 `floorLimit` 即时控制，并在下一个定时时间由 `floorSchedule` 再次覆盖。
- [ ] 定时单楼层、多楼层132字节全量帧、同分钟防重复和 MQTT 断线期间本地执行均验证。
- [ ] `elevatorMode` 设置、持久化、查询、字段边界和请求ID回显均验证。
- [ ] 总开关开启时，OPEN帧的120～127位由模式状态覆盖，36字节HEX帧的120～127位清零，两类帧的0～119位均不变。
- [ ] 总开关关闭时不修改任何原始楼层权限帧，113～120层权限恢复正常。
- [ ] 人脸、密码、刷卡、人员、访客二维码和remoteCall的36字节帧不会触发模式通道；floorLimit和floorSchedule的132字节帧保持数据库模式状态。
- [ ] 总开关开启时，remoteCall及刷卡、人脸、密码、人员/访客二维码的纯113～120楼授权在RS485前拒绝并上报“楼层已被电梯模式占用”；混合低楼层授权继续按原逻辑执行。
- [ ] LIVE、NONE、RECORDED 下载和仅调音量四种 `videoControl` 路径均验证。
- [ ] 正确处理 `videoControl` 的可选字段及设备主动音量事件。
- [ ] 人员新增、hash 未变化、ID 幂等、ID 冲突、部分删除、全量 hash 对账均验证。
- [ ] `face.requestImages → face.responseImages → Imagesresult` 成功与失败路径均验证。
- [ ] `face.accessResult` 成功与 RS485 失败事件均验证。
- [ ] MQTT Broker 断线期间四种通行成功次数能够持久化，重连后按 method 分批补报，补报期间新增次数不会丢失。
- [ ] `remoteCall` 楼层边界（-8、-1、1、120、0、字符串）、RS485 成功/失败、`remoteCall.accessResult` 均验证。
- [ ] `remoteCall.deductResult` 的成功同步、跳过、设备失败、失败不回退本地次数、重复回执和同人员同楼层并发保护均验证。
- [ ] QR 完整链路、拒绝、超时、重复消息、VISITOR/CREDENTIAL、floorHex 不一致均验证。
- [ ] 确认前端把人员同步失败 `resultcode=-1` 正确识别为失败。

## 8. 主要源码依据

| 内容 | 当前源码位置 |
|---|---|
| client_id 与 online_v1 Topic 生成 | `components/settings/mqttpage.cpp:34-73, 87-123` |
| online_v1 路由选择与配置 | `components/settings/mqttpage.cpp:282-383, 594-629` |
| method 注册总表 | `components/settings/mqtt/mqttmanager.cpp:691-829` |
| floorLimit、floorLimit.query 与 heartbeat | `components/settings/mqtt/mqttmanager.cpp` |
| floorSchedule 规则、hash删除、查询和本地定时执行 | `components/settings/mqtt/mqttmanager.cpp`、`mqttmanager.h` |
| elevatorMode设置、查询与数据库持久化 | `components/settings/mqtt/mqttmanager.cpp`、`ic_board/rs485_floor_frame_builder.cpp` |
| 所有RS485楼层帧的最终模式位保护 | `ic_board/rs485_driver_port.cpp`、`rs485_floor_frame_builder.cpp` |
| online_v1 独立楼层状态位持久化 | `components/settings/mqtt/mqttmanager.cpp`、`common/sql/dbstore.cpp` |
| remoteCall、accessResult 与 deductResult | `components/settings/mqtt/mqttmanager.cpp`、`common/sql/network_personnel_store.cpp` |
| 人员同步回执与 resultcode | `components/settings/mqtt/mqttmanager.cpp:939-1037`、`mqttmanager.h:60-65` |
| face.accessResult 与二维码流程 | `components/settings/mqtt/mqttmanager.cpp:1039-1532` |
| 人脸图片请求、响应和 Imagesresult | `components/settings/mqtt/mqttmanager.cpp:1534-1888` |
| 多媒体、重启与响应 Topic | `components/settings/mqtt/mqttmanager.cpp:2146-2584`、`mqttservice.cpp:455-1111` |
| 人员字段、幂等、删除与 hash 对账 | `common/sql/network_personnel_store.cpp:344-411, 953-1302, 1850-2110` |
| 人脸图片落盘与校验前处理 | `common/sql/network_personnel_store.cpp:525-950` |
