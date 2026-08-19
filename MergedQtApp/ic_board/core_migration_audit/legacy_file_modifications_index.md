# 旧代码改动索引（已落地）

以下为本次直接修改的 Qt 既有源码文件（非新增文件）：

1. `ic_board/ic_board.cpp`
- 增加刷卡白名单拦截。
- 二维码密钥改为配置读取。
- RS485 回包转发到事件桥。

2. `ic_board/ic_event_bridge.h`
- 增加 `emitRs485FrameReceived(...)`。
- 增加信号 `rs485FrameReceived(...)`。

3. `ic_board/ic_event_bridge.cpp`
- 实现 `emitRs485FrameReceived(...)`。

4. `components/settings/mqtt/ic_mqtt_gateway.h`
- 增加槽函数 `onRs485FrameReceived(...)` 声明。

5. `components/settings/mqtt/ic_mqtt_gateway.cpp`
- 接入白名单统一逻辑（注册/删除/刷卡上报前校验）。
- `getTrafficFloor` 回包 Floor 改 128 位字符串。
- 新增 RS485 回包上报 `methon=getjdq`。

6. `components/features/command_dialog.cpp`
- 增加并映射以下配置入口：
`network_interface`、`cardinfo_output`、`server_output`、`relay_num`、`relay_times`。

7. `qt_ycest.pro`
- 注册新增源码：
`ic_board/core_migration_audit/core_migration_sync.cpp/.h`

说明：
- 新增源码均放在 `ic_board/core_migration_audit/` 目录下。
- 详细代码级说明见 `core_migration_apply_notes.md`。
