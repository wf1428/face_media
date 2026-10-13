# Core -> qt_ycest_v3 实施说明（刷卡/二维码）

更新时间：2026-04-09 17:35

## 1. 实施范围

- Qt工程路径：`F:\T113-proj\liu_work\qt_ycest_1024x768_v2\qt_ycest_1024x768\qt_ycest_v3\qt_ycest`
- 对齐范围：刷卡、二维码、IC-MQTT、RS485 回包链路
- 排除范围：人脸机、消防
- 按需求不实施：M6（`sector_num` 修改后发串口到刷卡器）

## 2. 已完成改动（对应 M1~M5）

### M1 刷卡白名单校验闭环（已完成）

- 新增迁移辅助模块（统一白名单读写）：
  - `ic_board/core_migration_audit/core_migration_sync.h`
  - `ic_board/core_migration_audit/core_migration_sync.cpp`
- 刷卡放行前增加白名单拦截：
  - `ic_board/ic_board.cpp:100`
- MQTT 注册/删除卡改为走统一白名单逻辑（并同步 `register_times`）：
  - `components/settings/mqtt/ic_mqtt_gateway.cpp:508`
  - `components/settings/mqtt/ic_mqtt_gateway.cpp:517`

实现点：
- 白名单键：`card_allow_<CardID>`
- 计数对齐 Core：`register_times`
- 兼容保留：同步写 `registered_card_count`

### M2 二维码密钥配置化（已完成）

- 启动时从配置加载二维码密钥：
  - `ic_board/ic_board.cpp:145`
- 每次处理二维码前刷新运行态密钥（支持配置变更后立即生效）：
  - `ic_board/ic_board.cpp:173`

### M3 getjdq 回包上报链路补齐（已完成）

- 事件桥新增 RS485 回包信号：
  - `ic_board/ic_event_bridge.h:16`
  - `ic_board/ic_event_bridge.h:22`
  - `ic_board/ic_event_bridge.cpp:19`
- RS485 收到回包后转发到事件桥：
  - `ic_board/ic_board.cpp:44`
- MQTT 网关接收回包并上报：
  - `components/settings/mqtt/ic_mqtt_gateway.cpp:228`
  - `components/settings/mqtt/ic_mqtt_gateway.cpp:458`
  - `components/settings/mqtt/ic_mqtt_gateway.cpp:467`

上报格式：
- `{"methon":"getjdq","id":"<32位message_id>","jdq":"<回包文本>"}`（`id` 由 `nextMessageId()` 生成）

### M4 getTrafficFloor 返回 Floor 格式对齐 Core（已完成）

- 从 32位hex 改为 128位`0/1`位图：
  - `components/settings/mqtt/ic_mqtt_gateway.cpp:332`
- 位图来源：`DeviceConfigSync::floorFlags128Bits()`（由辅助模块统一输出）

### M5 set_to_zkb 对应配置入口补齐（已完成）

- 在命令配置页补齐以下项（UI + DB映射）：
  - `network_interface`
  - `cardinfo_output`
  - `server_output`
  - `relay_num`
  - `relay_times`
- 代码位置：
  - `components/features/command_dialog.cpp:249`
  - `components/features/command_dialog.cpp:326`

## 3. 工程接入改动

- 工程文件注册新增源码：
  - `qt_ycest.pro:61`
  - `qt_ycest.pro:112`

## 4. 全工程刷卡/二维码链路复核结论

- 刷卡链路：
  - 串口收卡帧 -> `OfflineChecker::check(...)` -> 白名单校验 -> RS485下发 -> MQTT上报 `card`
- 二维码链路：
  - 在线二维码JSON上报 `sendQrCode`
  - 离线二维码使用配置密钥校验，通过后下发RS485
- 楼层通行链路：
  - `openTrafficFloor/closeTrafficFloor` 仍走 128位位图逻辑
  - `getTrafficFloor` 回包 `Floor` 已与 Core 对齐为 128 位字符串
- 继电器查询链路：
  - 下发 `getjdq` 后，RS485回包会触发 MQTT `methon=getjdq` 上报

## 5. M6 说明（未实施）

- 按你的要求，不做 `sector_num` 修改后串口转发刷卡器逻辑。
- 原因：该逻辑主要是 STM32 + W25Q64 扇区体系下的配套行为，Qt侧当前架构可不需要该串口转发步骤。
