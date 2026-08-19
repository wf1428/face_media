# 融合工程总体与 MQTT 协议审计报告

- 复核日期：2026-08-06
- 当前工程：`F:\rk3566-proj\face_media\MergedQtApp`
- 参考目录：`F:\rk3566-proj\face_media\MergedQtApp_V2_板坏后\MergedQtApp`
- 检查方式：只读静态审计；未编译、未运行、未修改工程代码

## 1. 总体结论

本工程是“多媒体播放 + IC 门禁 + 人脸机”的单进程融合程序。`AppShell` 管理多媒体和人脸机的前台切换；MQTT 由外部 mqttd 维持 broker 连接，Qt 程序通过 Unix 本地 socket 与 mqttd 通信；串口、RS485、人员数据库和 FaceGate 均在同一进程内协作。

本次检查覆盖：

- 87 个 `.cpp`、94 个 `.h`、1 个 `.c`；
- 3 个 `.pro`、2 个 `.qrc`、4 个 `.ui`；
- 主工程和独立 FaceGate 工程的全部构建引用；
- 根 QRC 78 个资源和 FaceGate QRC 40 个资源；
- 当前目录相对参考目录的 50 个修改文件、6 个删除文件和 1 个新增文件；
- 所有 MQTT 路由、IPC 消息类型和业务 method；
- 最近删除函数、参数、文件的调用和替代关系。

静态结果：没有发现最近清理改变有效播放、人脸验证或 MQTT/IC 活动逻辑；没有发现工程清单或 QRC 的缺失路径；没有发现 `QDebug`、智能指针和 `std::move` 的直接头文件缺失。

## 2. 融合运行结构

```text
main.cpp
  └─ AppShell（唯一 EGLFS 顶层窗口）
      ├─ QStackedWidget
      │   ├─ MultimediaModuleAdapter → MainWindow → MultimediaDemo
      │   └─ FaceGateModuleAdapter → FaceGateMainWindow
      ├─ ModeController
      ├─ DebugMousePresenceSensor
      └─ CursorOverlay

MQTT broker ←→ mqttd ←Unix socket /tmp/mqttd.sock→ MqttIpcClient
                                                     └→ MqttManager
                                                         ├→ MqttService（播放/FTP）
                                                         ├→ IcMqttGateway（online_v2/IC）
                                                         ├→ NetworkPersonnelStore（online_v1）
                                                         └→ FaceImageSyncBridge（FaceGate 管理页）

串口/二维码/RS485 ←→ IcWorker / clients / workers ←→ IcEventBridge
```

模式切换时只停用前台重资源：

- 多媒体切到人脸机：先停用多媒体，再激活相机、推理和活体；失败则回退多媒体。
- 人脸机切回多媒体：先停相机/推理/活体，再恢复多媒体。
- 数据库和必要后台对象保留到整个程序 shutdown。

## 3. 当前配置实际选择

`static/demoResources/images/logo/net_cfg.ini` 当前关键值：

| 配置 | 当前值 | 作用 |
|---|---|---|
| `feature/offline_v1` | `false` | 不走纯离线模式 |
| `feature/online_v1` | `false` | 不使用 ycLinux V1 路由 |
| `feature/online_v2` | `true` | 当前使用 yc/IC 在线路由 |
| `mqtt/client_id` | `YCEEQZA10C1FFC62` | 设备 MQTT 标识 |
| `mqtt/host_online_v2` | `192.168.3.11` | 当前 broker 主机 |
| `mqtt/sub_topics_online_v2` | `device/yc/YCEEQZA10C1FFC62/responses` | 当前订阅主题 |
| `mqtt/publish_topic_online_v2` | `device/yc/YCEEQZA10C1FFC62/event` | 当前发布主题 |
| `mqtt/ipc_path` | `/tmp/mqttd.sock` | Qt 与 mqttd 的本地 IPC |
| `mqtt/protocol_mode` | `online` | 启用 `IcMqttGateway` |

三种 feature 必须恰好启用一个：

- 都未启用或启用多个：配置判定失败。
- `offline_v1`：明确不建立 MQTT。
- `online_v1`：读取 `host/sub/publish/username/password_online_v1`。
- `online_v2`：读取相应 `_online_v2` 键。

兼容值：`protocol_mode=legacy` 归一为 `online`，`protocol_mode=ic` 归一为 `offline`。`IcMqttGateway` 只在归一后的 `online` 模式启用。

## 4. MQTT 主题路由

| 模式 | 下行订阅 | 上行发布 | 主要业务 |
|---|---|---|---|
| offline_v1 | 无 | 无 | 本地卡、离线二维码和本地授权 |
| online_v1 | `device/ycLinux/<clientId>/request` | `device/ycLinux/<clientId>/event` | 多媒体、人员全量同步、人脸图片、remoteCall、V1 二维码 |
| online_v2 | `device/yc/<clientId>/responses` | `device/yc/<clientId>/event` | 多媒体和 IC 在线卡/二维码/楼层/RS485 |

普通请求的响应主题规则：

- 请求以 `/request` 结尾：替换为 `/event`；
- 其他请求：末尾追加 `/event`；
- online_v2 当前多数上行直接使用配置的 `.../event`。

## 5. mqttd 本地 IPC

`MqttIpcClient` 使用 `QLocalSocket`，每条消息是一行 JSON。Qt 发给 mqttd 的常用命令是：

```json
{
  "cmd": "publish",
  "topic": "...",
  "payload": { }
}
```

程序会自动重连本地 socket。收到的 `type` 含义如下：

| `type` | 含义 | Qt 侧处理 |
|---|---|---|
| `hello` | mqttd 初始状态 | 更新连接、订阅和配置显示 |
| `status` | 当前 broker/config 状态 | 更新 UI；连接成功后持久化当前主题路由 |
| `test_result` | 测试连接结果 | 更新状态并记录 broker、端口、ClientId、QoS、TLS |
| `conn` | broker 状态变化 | 处理 `connected`、`disconnected`、`failed` |
| `ack` | mqttd 命令应答 | 记录 `cmd/ok/msg` |
| `msg` | broker 下行消息 | 提取 `topic/payload` 后进入业务路由器 |
| `error` | mqttd 错误 | 记录 `msg` |

兼容处理还包括：

- 直接的 `topic + JSON` 文本行；
- `payload` 被错误包装成字符串的损坏 `msg`；
- `method` 和 IC 旧拼写 `methon`。

## 6. 普通业务报文格式

多媒体和 online_v1 使用的标准 envelope 一般为：

```json
{
  "method": "videoControl",
  "id": "请求ID",
  "deviceId": "设备ID",
  "data": {},
  "time": "yyyy-MM-dd HH:mm:ss"
}
```

IC 兼容协议的上行格式不同，保留历史拼写和大小写：

```json
{
  "methon": "card",
  "id": "32位十进制递增ID",
  "Time": "yyyy-MM-dd HH:mm:ss",
  "CardID": "...",
  "Floor": "..."
}
```

IC 下行同时接受 `method/methon`、`id/reqId`、`time/Time`，并兼容 `data` 嵌套或扁平字段。

## 7. 多媒体 MQTT method 说明

| 下行 method | 请求数据 | 实际作用 | 上行/回执 |
|---|---|---|---|
| `getPlayFileList` | 公共 envelope | 扫描本地视频目录 | `sendPlayFileList`，`data.file` 为文件数组 |
| `getPlayInfo` | 公共 envelope | 请求整屏与视频帧合成截图，读取当前播放状态 | `sendPlayInfo`，见下表 |
| `deletePlayFile` | `data.name`，可带 `data.id` | 按文件名删除本地视频 | 当前只记成功/失败日志，没有 MQTT 业务回执 |
| `videoControl` | `data.videoType` 及类型相关字段 | 控制录播、直播、停止和音量 | 仍以 `videoControl` 回同一请求 ID |
| `linuxReboot` | 公共 envelope | 先回执再延迟 1 秒 sync/reboot | `data.status=ok` |
| 无 method 的流地址消息 | 指定 `streamUrlTopic`；`data.videoUrl` 或根 `url` | 等待流地址时启动直播 | 无独立业务回执 |

`sendPlayInfo.data.info` 字段：

| 字段 | 含义 |
|---|---|
| `videoType` | `none/live/recorded` 小写状态 |
| `videoVolume` | 0–100 |
| `videoPath` | 当前录播文件路径 |
| `videoUrl` | 仅 LIVE 返回当前 URL，其他类型返回空 |
| `name` | 当前媒体名 |
| `base64Image` | 合成截图 Base64 |
| `deviceLog` | 最近下载、音量或播放说明 |

### 7.1 `videoControl`

通用输入：

- `data.videoType`：`RECORDED`、`LIVE`、`NONE`；未知值按停止所有播放处理。
- `data.videoVolume`：数字或数字字符串，夹到 0–100。

RECORDED：

- 有 `videoPath`：启动 FTP 下载；`videoPath` 只取文件名，FTP 主机、端口、用户等来自 data 并写入任务配置。
- 无 `videoPath`：停止直播并恢复已有录播，可用于只调音量。
- 同一下载任务重复消息合并；另一个任务正在下载时拒绝。

LIVE：

- 要求 `videoUrl`；持久化流地址并推断 `rtsp/rtmp/srt/udp/hls/http-ts`。
- RECORDED 下载中不允许切换，回 `download_busy`。

NONE：停止所有播放。

回执会保留原请求 `data`，再补充：

| 字段 | 含义 |
|---|---|
| `deviceClientIp` | 当前设备客户端 IP |
| `status` | 需要时为 `ok/error` |
| `ftpDownload` | 下载链需要时为 `100%/0%` |
| `downloadResult` | `downloaded`、`already_exists`、`retryable_error`、`local_rejected`、`storage_limited`、`setup_error`、`download_busy` 等 |
| `reason` | 人可读原因 |

带音量的非下载请求会等待实际音量设置结果后统一回执，避免提前报告成功。

## 8. online_v1 人员与人脸同步

这些 method 只在当前 publish 主题为 `device/ycLinux/.../event` 且下行来自 `device/ycLinux/.../request` 时处理。

| method | 关键 data | 作用 | 回执/后续 |
|---|---|---|---|
| `sync.fullPersonnel` | `personId`、`personHash`、`person`、`faces`、`icCards`、`qrCodes`、`rules`、`floors` | 事务式新增/更新一名网络人员及全部授权数据；按请求 ID/内容哈希幂等 | 同 method 回 event，`resultcode` 0/1；成功后检查缺失图片并自动发 `face.requestImages` |
| `sync.checkAllPersonHash` | `personIdHashes` 对象 | 将平台全量 personId/hash 与本地比较 | 返回 `missing/changed/matched/extra/inferredDeletedCount`；本地多余人员标记删除 |
| `sync.deletePersonnel` | `personId`、`deleteType`、可选 `personHash/items` | 按删除类型删除/标记人员或其子数据 | 同 method 回 event，包含状态、代码和消息 |
| `face.responseImages` | `personId`、`faces`，每张含 hash 和 Base64 图片 | 先保存为待校验图片，再进行单人脸、检测置信度、人脸质量和特征提取校验；该链路不做活体检测 | 全部图片通过后发布 `Imagesresult` 成功；任一图片失败则删除该人员聚合数据和图片，并发布失败原因 |

统一同步回执 `data` 至少包含：

- `status`：`SUCCESS/FAILURE`；
- `code`：细分结果码文本；
- `message`：说明；
- `personId`：有人员上下文时返回；
- `resultcode`：成功 0、失败 1。

`sync.fullPersonnel` 如果 `personHash` 和所有人脸 hash 都未变化，会跳过重复元数据更新，但仍检查图片文件是否缺失，以允许上一次图片下载失败后重试。

`face.requestImages` 上行 `data` 来自本地缺图清单，主要为 `personId` 和 `faceHashes`。

`face.responseImages` 的处理分为两个阶段：

1. Base64 图片通过格式校验并安全写入 `network_faces/<personId>`，`network_face.status` 暂记为 `IMAGE_PENDING_VALIDATION`，人员状态暂记为 `FACE_VALIDATING`。
2. FaceGate 推理线程逐张执行单人脸检测、`faceDetectThreshold` 检测置信度校验、`faceQualityThreshold` 人脸质量校验和特征提取。这里明确不调用活体检测。

只有该人员的所有 `network_face` 记录都成为 `IMAGE_READY`，人员状态才更新为 `READY`。随后向当前 online_v1 event 主题发布：

```json
{
  "method": "Imagesresult",
  "id": "沿用 face.responseImages 的 id；缺失时生成",
  "deviceId": "沿用下行 deviceId；缺失时使用本机配置",
  "data": {
    "personId": "人员ID",
    "result": true,
    "msg": "人脸图像同步成功。"
  },
  "time": "yyyy-MM-dd HH:mm:ss"
}
```

失败时 `result=false`，`msg` 返回具体原因，例如“人脸质量过低，请重新录入人脸。”、“人脸识别质量过低，请重新录入人脸。”、“未检测到人脸，请重新录入人脸。”或“图像中检测到多张人脸，请重新录入单人脸图像。”。保存失败、推理服务未就绪、特征落库失败和 30 秒校验超时也按失败处理。失败路径会事务删除该人员在 `network_*`、`sync_inbox` 中的聚合记录，并清理对应 `network_faces` 目录，使平台可重新下发该人员。

## 9. online_v1 `remoteCall`

输入要求：

- `id`、`deviceId`、`data.personId` 非空；
- `deviceId` 与当前设备一致；
- `data.floor` 必须是 JSON 整数；
- 楼层范围 `-8..-1` 或 `1..120`，不允许 0。

通过校验后使用公共楼层位图构造器生成 36 字节 RS485 帧并发送。实际成功/失败只记录 RS485 完成日志，当前协议明确“不产生 MQTT 回包”。

## 10. online_v1 二维码协议

### 10.1 流程

1. 扫描器产生二维码，创建本地二维码事务和 `scanId`。
2. 上行 `qr.scan`：`data.qrCode`。
3. 下行 `qr.scanResult`：平台初步授权结果。
4. 下行 `qr.floorControl`：楼层授权及位图。
5. 本地核对楼层数组与 `floorHex` 一致，发送 RS485。
6. RS485 完成后上行 `qr.accessResult`。
7. 下行 `qr.deductResult`：平台确认次数扣减，更新本地规则/访客次数并结束事务。

同一时刻只允许一个待处理二维码；重复消息按 message ID 和本地事务幂等处理。

### 10.2 method 与字段

| method | 方向 | 关键字段 | 含义 |
|---|---|---|---|
| `qr.scan` | 上行 | `qrCode` | 报告扫描值并开始平台授权 |
| `qr.scanResult` | 下行 | `code`、`success`、`reason`、内层 `data` | `code=200 && success=true` 才接受 |
| `qr.floorControl` | 下行 | `qrCode`、`sourceType`、`personId`/`invitationId`、`floorHex`、`floors` | 下发楼层授权；`sourceType` 只能为 `CREDENTIAL` 或 `VISITOR` |
| `qr.accessResult` | 上行 | `qrCode`、`success` | 报告设备/RS485 实际开启结果 |
| `qr.deductResult` | 下行 | `qrCode`、`sourceType`、`personId`、`code`、`deducted`、`message`、`remainingCount`、`usedCount` | 平台扣次确认并同步本地计数 |

`floorHex` 可为裸 32 个十六进制字符，或 `#@` + 32 字符 + `#!`。程序会从 `floors` 重新生成 36 字节帧并比较中间 32 字符，二者不一致则拒绝。

`CREDENTIAL` 必须带 `personId`；`VISITOR` 不允许带 `personId`。

### 10.3 二维码 code 含义

| code | 提示含义 |
|---:|---|
| 4000 | 二维码参数错误 |
| 4001 | 二维码无效 |
| 4002 | 二维码格式错误 |
| 4003 | 二维码记录不存在 |
| 4100 | 二维码已停用 |
| 4101 | 尚未生效 |
| 4102 | 已过期 |
| 4200 | 通行次数已用完，需充值 |
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

未识别 code 优先使用服务端 `reason/message`，否则显示“二维码处理失败”。

## 11. online_v2 / IC MQTT method

`IcMqttGateway` 在 `protocol_mode=online` 时优先拦截下列 IC method。

### 11.1 设备上行事件

| `methon` | 字段 | 含义 |
|---|---|---|
| `card` | `CardID`、`Floor` | 已注册卡刷卡事件；若楼层未同时到达，等待 10ms 后以空楼层兜底 |
| `sendQrCode` | `qrCode` | online_v2 二维码扫描值，等待服务端 `sendFloorQr` |
| `keep` | `Connection_status="YC-ZKB is alive"` | 每 60 秒保活 |
| `getjdq` | `jdq` | RS485 接收文本上报 |

未注册卡不会通过 IC 网关上报。

### 11.2 服务端下行与成功响应

| 下行 method | 输入/作用 | 成功上行 |
|---|---|---|
| `reboot` | 设备重启 | `rebootSuccess`，沿用请求 ID 后重启 |
| `setTime` | `data.time/Time` 或 envelope 时间；设置系统时间并同步 RTC | 无专用成功消息，失败记日志 |
| `registerCardOne` | `CardNo/CardID/cardNo/cardId`；规范化并注册卡 | `registerSuccess` + `CardID` |
| `deleteCardOne` | 同上；删除卡 | `deleteSuccess` + `CardID` |
| `openFloor` | `floor/Floor/openFloor`；构造并发送单楼层帧 | `openFloorSuccess` |
| `openTrafficFloor` | `openTrafficFloor/Floor/data`；更新交通梯楼层位并发 RS485 | `openTrafficFloorSuccess` |
| `closeTrafficFloor` | `closeTrafficFloor/Floor/data`；关闭交通梯楼层位并发 RS485 | `closeTrafficFloorSuccess` |
| `getTrafficFloor` | 查询当前 128 位楼层状态 | `controlFloor` + `faceDevice` + `Floor` 128 位字符串 |
| `sendFloorQr` | 平台给 online_v2 二维码返回 RS485 数据 | 实际 RS485 完成后本地显示成功/失败，不发单独 MQTT success |
| `setjdq` | `setjdq/cmd/data`，发送指定 RS485 命令 | 无专用回执 |
| `getjdq` | 向 RS485 发送字面量 `getjdq` | 收到 RS485 数据后以上行 `getjdq` 返回 |
| `ResponseAlive` | `data.code/msg` | 只记录服务端存活响应 |

IC 成功响应使用请求原 ID；无请求上下文的事件 ID 来自统一 SQLite 配置中的递增计数，并左补零到 32 位。

## 12. online_v1 与 online_v2 二维码分流

`IcWorker` 读取 feature 后明确分流，不会让同一帧同时进入两套协议：

- `OnlineV2`：解析二维码后发 `IcEventBridge::onlineQrScanned`，由 `IcMqttGateway` 上报 `sendQrCode`。
- `OnlineV1`：发 `onlineV1QrScanned`，由 `MqttManager` 上报 `qr.scan` 并走事务协议。
- `Offline`：交给 `OfflineQr::handleFrame` 做本地日期、MD5、权限和 payload 校验。

因此，虽然 `protocol_mode=online` 会启用 IC 网关，二维码仍由 feature 路由信号隔离。

## 13. FaceGate 与 MQTT 的交叉关系

`FaceImageSyncBridge` 只承担进程内 UI/数据桥接：

- FaceGate 管理页请求网络人员列表；
- MqttManager 返回 `NetworkPersonnelStore` 的 JSON；
- 人脸图片同步状态回显到管理页；
- MqttManager 通过 bridge 把落盘图片交给 FaceGate 推理线程做离线质量校验，并接收校验结果。

网络同步图片现在会自动提取特征，并把特征、模型版本和就绪状态写回 `network_face`。但这些记录仍未装载到 FaceEngine 的本地识别图库；日常闸机识别继续使用 FaceGate Repository 的本地人员/特征表。两套数据共用 `/home/cat/face_media/access_control.db` 文件，但不是同一组业务表。

## 14. 最近修改的总体差异复核

相对参考目录：

- 修改：50 个源/头/工程文件；
- 删除：6 个重复或未构建文件；
- 新增：`rs485_dir.c` 独立内核模块。

此前清理差异主体是日志、注释、无调用符号、无读取成员、直接 include 和三个内部参数。本次后续需求在此基础上新增了 `Imagesresult`、人脸图片离线质量校验和失败回滚；topic 选择、RS485 帧生成、二维码状态机以及摄像头实时验证状态机未改动。

关键交叉检查：

| 检查 | 结果 |
|---|---|
| 主工程 qmake 引用 | 187 项，0 缺失 |
| FaceGate qmake 引用 | 54 项，0 缺失 |
| 两个 QRC | 118 项，0 缺失 |
| 删除文件残留引用 | 0 |
| `QDebug` 直接 include 缺失 | 0 |
| `<memory>` 直接 include 缺失 | 0 |
| `<utility>` 直接 include 缺失 | 0 |
| `isFloorUpperLetterAscii` | 定义和 3 个调用存在 |
| `bytesOf` | 定义和调用存在 |
| 已确认删除的旧函数 | 当前 0 命中 |

## 15. 现有风险与产品边界

这些项目不是最近清理造成，但需要在联调和协议验收中确认：

1. `deletePlayFile` 没有 MQTT 业务回执。
2. online_v1 `remoteCall` 没有 MQTT 业务回执，只以 RS485 日志判定结果。
3. FaceGate 的 `GateOutputService` 只发状态，不直接驱动继电器/GPIO。
4. MQTT 网络人员不会自动进入 FaceGate 识别图库。
5. FaceGate 当前配置加载强制 QSQLITE，MySQL 参数不会改变实际驱动。
6. mqttd 不在本工程源码内；Qt 侧只能验证 IPC 合约，无法静态证明 mqttd 对 broker 的 QoS、重连和订阅行为。
7. 编译、模型加载、硬件节点、RS485 电气方向、串口波特率和目标板时序必须由目标板测试确认。

## 16. 建议的端到端手工测试矩阵

| 场景 | 重点观察 |
|---|---|
| offline_v1 | 不连接 MQTT；卡、二维码、时间、次数和楼层本地校验 |
| online_v1 人员 | full/hash/delete 幂等、回执、缺图自动请求、图片落盘 |
| online_v1 二维码 | scan → scanResult → floorControl → RS485 → accessResult → deductResult；超时和重复消息 |
| online_v1 remoteCall | floor 边界、deviceId、不回包约定、RS485 实际完成 |
| online_v2 IC | 卡注册/删除、刷卡、二维码、楼层开关、交通梯、get/setjdq、保活和重启 |
| 多媒体 MQTT | 文件列表、截图信息、录播下载、直播、NONE、音量、重复任务和空间不足 |
| 模式切换 | 播放 → 人脸相机/活体 → 播放恢复；激活失败回滚 |
| FaceGate | 本地图库、活体、识别、重复抑制、录入、日志、音频和闸机硬件接入 |
| 故障 | mqttd 断开、broker 断开、数据库不可用、U 盘拔出、相机断开、RS485 超时 |

## 17. 最终判断

整个工程的静态调用链、参考目录差异和 MQTT 协议已经分层复核。最近几次清理没有证据表明改变了有效逻辑；此前暴露的误删函数和直接头文件问题在当前源码中已恢复或补齐。

报告同时确认了四个需要产品/目标板重点验证的现有边界：删除文件无回执、remoteCall 无回执、人脸开闸服务仍是状态占位、网络人员未自动进入识别图库。这些结论应与平台协议和硬件实现一起验收。
