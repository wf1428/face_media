# Web 本地 API 实施记录（2026-10-15 发起，2026-10-16 验证）

依据：[Web 本地 API 设计](web_local_api_design_20261015.md)。

2026-10-22 更新：定时梯控写入/删除已由 Web 独立实现，兼容原规则格式和共享存储，MQTT 源码与原执行条件保持原样；图片广告及共享播放选择已接入。以下为原阶段记录，当前能力和修改清单以[最新接入记录](web_schedule_media_20261022.md)为准。

本轮按以下 5 个任务完成现有本地业务接线和能力边界处理。标准文档的 39 条路由全部登记，其中 30 条接到本地业务，9 条因缺少独立执行能力明确返回 501。**路由存在不代表硬件功能已经接通。**

| 任务 | 内容 | 结果 |
|---|---|---|
| 1 | 标准路由、统一鉴权、能力查询、操作结果与本地状态桥 | 完成；保留原 /api/v1，新增 /api；复用 Cookie、CSRF、Origin 校验 |
| 2 | 设备概览、实际播放状态、当前界面截图、页面轮询 | 完成；本地数据库、真实媒体快照、活动界面截图；未知存储返回 null |
| 3 | 本地人员、凭证、楼层、规则、人脸任务；保留原 Excel 导入 | 完成；人员差量事务、版本检查、充值幂等；人脸逐张追加；原导入组件/解析器/人员仓储保持不变 |
| 4 | 本地媒体与已有梯控能力；无实际执行能力明确返回未接入 | 可用能力完成；视频控制、楼层开放/限行、特殊模式、立即重启；缺口见下表 |
| 5 | 页面接线、构建、服务端/Qt/浏览器回归及受保护文件核对 | 完成开发机验证；物理开发板验收仍需进行，不能用浏览器测试桩替代 |

## 独立关系与数据来源

手机页面 → device-webd 标准路由 → Qt WebService 工作线程 → WebLocalApi 本地事务 / WebLocalRuntime 主线程桥 → 原本地数据、播放器、界面、RS485 IO。

- 不调用 MQTT 消息分派、查询回执或网络人员写入入口；不新增 MQTT 启停、mqttd 检测或 offline/online 准入条件。
- 人员范围为原 `person.deleted=0`；读取 `face_feature`、`local_ic_card`、`local_qr_code`、`local_access_rule`、`local_timing_rule`、`local_access_usage`、`local_floor`。不创建第二套人员库。
- 本地人员楼层按原离线通行逻辑汇总该人员全部分组。Web 保存的是整个人员的楼层集合，统一以当前 client_id 为分组字段；不改变原通行服务按人员汇总楼层的行为。
- 基础资料修改不调用 Excel 聚合覆盖入口。次数和金额使用增量充值，保留已消费记录；查询里的新增充值字段固定返回 0，避免将上一次 Excel 导入的充值数再次提交。
- 原有人员导入组件仍上传原始 Excel/图片和完整清单，继续调用原 XLS/XLSX 解析器及本地插入流程。单张新录人脸另走原质量/特征 worker，追加特征，不覆盖该人员其他人脸。
- 卡号继续兼容十进制、十六进制和读头字节序；校验本地及网络卡号冲突。二维码映射到原本地凭证表并由原离线通行服务校验有效期/次数。
- 播放状态来自播放器实际成员及音量控制器，不用设置请求冒充播放完成。切换后轮询源状态，未达到目标则报超时。
- 概览统计使用同一 SQL 快照；媒体存储读取视频目录所在实际文件系统，单位 GiB。按 2026-10-16 确认的部署方式，允许直接使用板载 eMMC 根分区，不要求独立挂载；目录或文件系统不可用时返回 null，不使用默认数字替代。
- 楼层/模式读取原公共配置键，通过公共组帧与 IcEventBridge 发送；配置保存与串口发送分别报告，`hardwareConfirmed=null`，不声称硬件动作已确认。

## 标准接口接入范围

| 接口组 | 本轮接入 | 明确未接入（HTTP 501） |
|---|---|---|
| A 设备 9 项 | 信息、登录、SSID、登录改密、截图、概览、立即重启 | A05 配置包应用/回滚；A08 定时开关机/唤醒 |
| B 人员 5 项 | 列表、新增、修改、软删除、详情 | 无 |
| C 凭证 6 项 | 查询、初始化、规则更新、卡/二维码/人脸增删、注册结果 | 规则内的继电器执行不接入；已有继电器开关保留，禁止新修改 |
| D 人员楼层 2 项 | 保存/清空本地人员楼层集合 | 无 |
| E 梯控 10 项 | 楼层读写、定时规则读取、模式读写 | E03/E05/E06 定时规则写入/删除/重应用；E09/E10 信号休眠 |
| F 空调 2 项 | 保留页面并显示未接入 | F01/F02 设置与遥测：当前无实际空调驱动 |
| G 视频 5 项 | multipart 上传、真实文件列表/删除、实际播放状态、播放控制 | 无；文件时长未取得时为 null |

定时梯控的现有定时执行器属于 `MqttManager`，并受原主题/模式条件控制。本轮严格不改 MQTT，也不额外启动第二个竞争串口的定时器，因此只读实际规则，禁用 3 个写入接口。要支持离线定时梯控，需要后续明确授权把执行器迁到公共本地业务层，再让两种入口复用。

额外提供 `GET /api/device/capabilities` 与 `GET /api/operations/{id}`。页面根据真实能力启用控件，不检查 mqttd 是否启动。旧登录、SSID、原始视频流上传、下载和 Excel 批次接口继续兼容。

## 图片、事务和操作恢复

- 人脸 HTTP JSON 上限 1 MiB，解码后最多 512 KiB；验证 JPEG/PNG 的真实类型和最大 1600 万像素。HTTP 服务写受控暂存文件，IPC 只传账号归属和资源句柄。Qt 计算真实图片摘要供幂等判断。
- 截图在主线程采集、后台编码，IPC 只传小型文件元数据；HTTP 再转换为 Base64。截图一次只处理一个请求。
- 图片接收句柄 10 分钟过期；截图文件保留 120 秒；失败人脸暂存保留 24 小时。专用图片暂存约 64 MiB 上限；清理不遍历或删除原 U 盘导入文件。人脸子项删除后清理不再使用的 Web 注册照。
- `web_local_revisions`、`web_local_operations`、`web_local_assets` 与独立 `web_local_schema_versions` 位于原 SQLite，初始化可重复执行；人员表只增加带默认值的 remark 字段。
- 人员、凭证、充值和幂等结果在同一个写事务提交。操作记录按账号+接口+Idempotency-Key 隔离，同键内容不同返回 409；明确完成结果保留 7 天。
- 同一页面中的网络失败重试复用未确定请求的幂等键；刷新页面后应先核对实际资料和用量。没有在浏览器持久保存密码、人员请求或登录令牌。
- 异步操作返回 `operationId/state/result/code/message`，通过操作查询获取完成结果；进程重启后未确定任务标为 UNKNOWN，不重放硬件动作或充值。重启任务比较 Linux boot_id，确认设备发生重启后记为完成。
- 改密使用账号现有 PBKDF2 迭代参数和新随机盐，校验原密码，撤销当前账号全部 Web 会话；不改热点、卡密码或 MQTT 密码。
- 标准视频 multipart 保持分块写盘，复用原校验/同步/原子发布；允许 file 位于元数据之前，按实际字节核对长度。临时文件同时使用独占创建、flock 和活动任务登记。
- `common/storage_policy.cpp` 只修正受控路径比较的分隔符：Qt 规范化路径统一使用 `/`。Linux 行为不变，Windows 隔离测试可复用同一注册照配额实现。

## 验证与部署

开发机验证：前端生产构建；Windows 独立服务器单元检查（Linux 随机令牌相关用例未在 Windows 执行）；Qt/SQLite 原 Excel 集成及新增本地业务回归；ARM64/Linux/OpenSSL 3 服务器交叉编译链接；真实 Chromium 页面回归。详细结果见开发目录 `.web-build/` 的构建日志、`browser-local-api/browser-results.json` 和 `local-api-source-audit.json`。

Qt 集成回归覆盖人员分页/创建重试/版本冲突、编辑不清空凭证、充值只加一次、楼层失败回滚、网络卡号冲突、访客有效期、人脸追加、原离线通行服务读取 Web 数据并扣费、真实图片暂存句柄、播放器/截图异步源桥、串口任务去重、账号任务隔离、改密以及不确定任务恢复。截图、播放器、串口和特征提取使用测试源回调，未声称完成物理硬件测试。

2026-10-16 eMMC 修正：取消媒体目录必须位于指定独立挂载点的限制，保留目录存在、受控路径、读写权限和文件系统可用/非只读检查；上传的空间及配额校验保持原样。旧挂载点参数仅保留配置兼容，概览按实际视频目录统计容量。本次无需改动或重建服务器、网页、MQTT 和启动脚本。回归使用普通临时目录和不存在的旧挂载点配置，验证文件列表、上传流程及容量统计继续可用，缺失目录与普通文件路径仍被拒绝。

浏览器由实际 device-webd 提供 dist、CSP、图标及控制台日志；Qt API 响应使用浏览器隔离测试桩。覆盖原 SSID 提示/刷新恢复、导航图标、概览/截图、新增人员/录卡、播放结果轮询、密码改后登录、原始视频及 Excel/人脸清单上传导入。

本轮核对原 MQTT 文件、Qt 启动脚本、原 XLS/XLSX 解析器、PersonImportWorker、本地人员仓储和 PersonnelImport 组件均未改动。无源文件删除。**部署需同时重新构建并替换板端 Qt、device-webd、dist，不能只更新网页。** 沿用原自动启动配置，不需重新安装热点服务。全量 Qt BSP 链接、实际截图视频合成、RKNN 人脸质量、RS485 传输、断电恢复和真实 WiFi/TLS 需要在开发板验收。

## 本轮文件清单

新增 7 个、修改 30 个源文件/文档，删除 0 个。另已生成前端 dist；构建目录和测试截图不计入源文件清单。

### 新增

- [docs/web_local_api_implementation_20261015.md](../docs/web_local_api_implementation_20261015.md)
- [modules/web/web_local_api.cpp](../modules/web/web_local_api.cpp)
- [modules/web/web_local_api.h](../modules/web/web_local_api.h)
- [modules/web/web_local_runtime.cpp](../modules/web/web_local_runtime.cpp)
- [modules/web/web_local_runtime.h](../modules/web/web_local_runtime.h)
- [web/frontend/src/api/local.js](../web/frontend/src/api/local.js)
- [web/server/src/standard_api.cpp](../web/server/src/standard_api.cpp)

### 修改

- [common/sql/web/web_account_repository.cpp](../common/sql/web/web_account_repository.cpp)
- [common/sql/web/web_account_repository.h](../common/sql/web/web_account_repository.h)
- [common/storage_policy.cpp](../common/storage_policy.cpp)
- [components/multimedia/multimediademo.cpp](../components/multimedia/multimediademo.cpp)
- [components/multimedia/multimediademo.h](../components/multimedia/multimediademo.h)
- [main.cpp](../main.cpp)
- [mainwindow.cpp](../mainwindow.cpp)
- [modules/web/web.pri](../modules/web/web.pri)
- [modules/web/web_bridge_server.cpp](../modules/web/web_bridge_server.cpp)
- [modules/web/web_service.cpp](../modules/web/web_service.cpp)
- [modules/web/web_service.h](../modules/web/web_service.h)
- [web/BOARD_RUN.md](../web/BOARD_RUN.md)
- [web/README.md](../web/README.md)
- [web/frontend/src/api/index.js](../web/frontend/src/api/index.js)
- [web/frontend/src/api/request.js](../web/frontend/src/api/request.js)
- [web/frontend/src/views/device/DevicePreview.vue](../web/frontend/src/views/device/DevicePreview.vue)
- [web/frontend/src/views/device/DeviceSettings.vue](../web/frontend/src/views/device/DeviceSettings.vue)
- [web/frontend/src/views/elevator/ElevatorControl.vue](../web/frontend/src/views/elevator/ElevatorControl.vue)
- [web/frontend/src/views/person/PersonDetail.vue](../web/frontend/src/views/person/PersonDetail.vue)
- [web/frontend/src/views/person/PersonList.vue](../web/frontend/src/views/person/PersonList.vue)
- [web/frontend/src/views/video/VideoManage.vue](../web/frontend/src/views/video/VideoManage.vue)
- [web/server/CMakeLists.txt](../web/server/CMakeLists.txt)
- [web/server/include/session_store.h](../web/server/include/session_store.h)
- [web/server/include/web_server.h](../web/server/include/web_server.h)
- [web/server/src/session_store.cpp](../web/server/src/session_store.cpp)
- [web/server/src/web_server.cpp](../web/server/src/web_server.cpp)
- [web/server/tests/session_test.cpp](../web/server/tests/session_test.cpp)
- [web/tests/browser_check.py](../web/tests/browser_check.py)
- [web/tests/qt_web_tests.cpp](../web/tests/qt_web_tests.cpp)
- [web/tests/qt_web_tests.pro](../web/tests/qt_web_tests.pro)
