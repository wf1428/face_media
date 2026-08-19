# Core 与 Qt 迁移差异复核（按排除规则过滤）

- 复核时间：2026-04-13
- Core 基线：`F:\T113-proj\liu_work\YC-IC2024-ZKBlatest20260205--(YC-ZKBV4)-常开\YC-IC2024-ZKBlatest20260205--(YC-ZKBV4)-常开\Core`
- Qt 工程：`F:\T113-proj\liu_work\qt_ycest_1024x768_v2\qt_ycest_1024x768\qt_ycest_v4\qt_ycest`

## 1. 本次按你的规则排除项

以下差异不计入“未移植缺陷”：

1. 消防、人脸、消防双继电器相关。
2. 发送 RS485 同时控制继电器的逻辑。
3. 源工程刷卡时间判断不支持跨天（以 Qt 侧为准）。
4. 二维码重复刷问题（服务端只认最新二维码）。
5. 限行/开放楼层语义（Qt 现实现按你描述语义处理）。

## 2. 已对齐主链路（抽查）

1. 卡注册/删除：`registerCardOne`、`deleteCardOne` 已在 Qt 处理。
- Qt: `components/settings/mqtt/ic_mqtt_gateway.cpp:655-669`

2. 远程开梯/限行：`openFloor`、`openTrafficFloor`、`closeTrafficFloor`、`getTrafficFloor` 已在 Qt 处理。
- Qt: `components/settings/mqtt/ic_mqtt_gateway.cpp:673-730`

3. `getjdq` 下发与回包上报已对齐。
- Qt: `components/settings/mqtt/ic_mqtt_gateway.cpp:599-615`, `757-760`

4. 楼层 128 位位图与持久化已在 Qt 实现。
- Qt: `ic_board/device_config_sync.cpp:233-325`

## 3. 排除后仍存在的迁移缺口

### 3.1 按键任务链路未迁移（确认存在）

Core 有完整按键扫描与按键任务：
- Core: `Core\Src\key.c:16-119`（`key_scan()` / `key_task()` / `query_registered_card()`）
- Core: `Core\Src\freertos.c:226`（循环调用 `key_task()`）

Qt 工程内无 `KEY1/KEY2/KEY3/KEY4`、`key_scan`、`key_task` 对应实现（全工程检索为 0）。

影响：
- Core 的本地按键行为（查询信息、查询注册卡、按键复位）在 Qt 中没有等价按键通道。
- 其中“重启”在 Qt 命令页有替代按钮；但按键触发链路本身不存在。

### 3.2 `network_interface` 配置已存储，但未驱动运行模式切换（部分未迁移）

Core 行为：
- `ifconfig eth0 up/down` 改写 `NETWORK_INTERFACE` 并重启，运行时据此切换网络/离线处理分支。
- Core: `Core\Src\set_to_zkb.c:76-89`
- Core: `Core\Src\freertos.c:241,250,355,386`

Qt 现状：
- `network_interface` 可在 DB 配置（命令页 + `DeviceConfigSync`）。
- Qt: `components/features/command_dialog.cpp:249,326`
- Qt: `ic_board/device_config_sync.cpp:50,95,160`
- 但实际运行分支由 `/mnt/UDISK/res/static/demoResources/images/logo/net_cfg.ini` 的 `mqtt/protocol_mode` 控制，不使用 `network_interface`。
- Qt: `ic_board/ic_board.cpp:16-34,199-231`

影响：
- 该项在 Qt 中目前更像“兼容存档字段”，不是 Core 等价的模式开关。

### 3.3 `cardinfo_output` / `server_output` 开关仅落库，未见业务侧消费（部分未迁移）

Core 行为：
- `cardinfo_output` 控制刷卡调试打印。
- Core: `Core\Src\my_swipe_card.c` 多处 `if(cardinfo_output)`
- `server_output` 控制下行消息打印。
- Core: `Core\Src\floor.c:271`

Qt 现状：
- 两个字段可配置并落 DB。
- Qt: `components/features/command_dialog.cpp:250-251,327-328`
- Qt: `ic_board/device_config_sync.cpp:51,53,96,98,161,162`
- 但在 `ic_board` / `ic_mqtt_gateway` 主流程未检索到对应读取分支。

影响：
- 当前只具备“配置存储”，未形成与 Core 一样的运行时开关效果。

## 4. 说明（与你第3条相关）

- Qt 的 `OfflineChecker::timePermit()` 已支持跨天时段。
- Qt: `ic_board/ic_offline_checker.cpp:184-213`
- 但 `CoreNetworkCardCompat` 仍保留 Core 老比较方式（兼容路径）。
- Qt: `ic_board/core_migration_audit/core_network_card_compat.cpp:54-59`

按你要求，本项不作为本次“未移植缺陷”统计，仅做提示。

## 5. 结论

按你给的排除条件过滤后，确认仍有 3 项需要关注：

1. 按键任务链路未迁移（明确缺口）。
2. `network_interface` 未作为运行模式开关生效（部分缺口）。
3. `cardinfo_output/server_output` 未驱动业务日志分支（部分缺口）。

其余核心链路（卡注册删除、远程开梯、限行控制、`getjdq`、128位楼层位图）本次对照未发现新的明显漏移植点。
