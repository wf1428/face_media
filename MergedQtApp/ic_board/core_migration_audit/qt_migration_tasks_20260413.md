# Qt 移植任务清单（2026-04-13）

## 范围与约束
- 已排除：消防/人脸/消防继电器/串口5替换显示、RS485联动继电器、跨天时间判断按 Qt 现逻辑、二维码重复刷按服务端最新码策略、限行/开放逻辑按现 Qt 解释。

## 任务状态（实时更新）
- [DONE] T1：`CommandDialog` 已增加 Core 对齐按键入口（KEY1/KEY2/KEY3/KEY4），并将结果输出到 `appendLog`。其中 KEY1 保留了 Core 注释逻辑说明。
- [DONE] T2：已移除 `fan_area`（刷卡扇区）UI 设置项及 `fan_area -> sector_num` 映射，Qt 侧不再提供扇区修改入口。
- [DONE] T3：已增加数据库保障：应用退出时 `aboutToQuit -> DbStore::checkpoint + DbStore::close`；命令窗口初始化后增加 DB 读写自检日志。
- [DONE] T4：完成关键路径静态核对（按键动作/DB收尾/扇区入口），本轮未执行完整编译链路。
- [DONE] T5：设备号按钮改为“读取 MQTT 已有设备号”（`mqtt/device_name`，回退 `mqtt/client_id`），不再本地拼接生成。
- [DONE] T6：移除“检测在线情况”按钮和 `detect_online` 逻辑分支。
- [DONE] T7：移除“同步时间”按钮和 `sync_time` 逻辑分支（保留“插入二维码接口”按钮）。
- [DONE] T8：补充 Core 对照结论：在线二维码通过后 Core 是否蜂鸣/继电器/RS485 的真实行为链路。
- [DONE] T9：按 `feature2/feature3` 二选一控制 MQTT 路由（禁止双选冲突）。
- [DONE] T10：`feature2` 固定 `host=www.ycest.vip` 且只走 `device/ycLinux/...` 主题链路。
- [DONE] T11：`feature3` 固定 `host=192.168.3.11` 且只走 `device/yc/...` 主题链路。
- [DONE] T12：保持自动连接、IPC 自动重连、网络重连逻辑不变，仅替换路由决策入口。
- [DONE] T13：`net_cfg.ini` 保持原读取逻辑，并新增双路由配置键（第二服务器 host/topic）。
- [DONE] T14：修复本轮改动文件中的乱码注释/提示文本，统一为中文可读。
- [DONE] T15：按最新反馈二次修复乱码并统一中文：`mqttpage.cpp`、`mainwindow.cpp` 注释与提示文本全量清理；修复“注释吞代码”导致的事件过滤安装、重启命令执行、ESC 退出全屏等风险点。
- [DONE] T16：MQTT 双路由读取改为“严格按配置文件取值”，去掉 host/topic 的硬编码初始回填；仅保留 `feature2` 主键 + `feature3` 第二套键（`host_feature3/sub_topics_feature3/publish_topic_feature3/username_feature3/password_feature3`）。
- [DONE] T17：新增双路由 `net_cfg.ini` 示例文件：`net_cfg_mqtt_dual_route_example.ini`。

## 变更记录
- 2026-04-13 任务文档创建。
- 2026-04-13 完成 KEY1~KEY4 按钮迁移（与 Core `key.c` 对齐），并补充 Core 注释项迁移说明。
- 2026-04-13 移除 `fan_area` UI 入口与映射，仅保留内部兼容字段 `sector_num`（非 UI 配置项）。
- 2026-04-13 增加数据库保障：`main.cpp` 退出收尾 + `CommandDialog` 启动读写自检。
- 2026-04-13 设备号按钮改为读取 MQTT 配置（当前路径为 `/mnt/UDISK/res/static/demoResources/images/logo/net_cfg.ini`），不再本地生成。
- 2026-04-13 移除“检测在线情况”“同步时间”按钮及对应 action 逻辑。
- 2026-04-13 新增路由改造任务：基于 `feature2/feature3` 单选控制 MQTT 服务器与 Topic 前缀。
- 2026-04-13 已完成 feature 路由改造：`FeaturesDialog` 互斥选择；`MqttPage` 手动连接/自动连接统一按 feature 决策 host/topic；`feature2->ycLinux@www.ycest.vip`，`feature3->yc@192.168.3.11`。
- 2026-04-13 补充 `net_cfg.ini` 路由扩展键：`mqtt/host_feature2`、`mqtt/subscribe_topic_feature2`、`mqtt/publish_topic_feature2`、`mqtt/stream_url_topic_feature2`、`mqtt/host_feature3`、`mqtt/subscribe_topic_feature3`、`mqtt/publish_topic_feature3`、`mqtt/stream_url_topic_feature3`；并修正 `mqttpage.cpp` 顶部乱码注释为可读文本。
- 2026-04-13 按反馈修复中文可读性：`mqttpage.cpp`、`featuresdialog.cpp` 中本轮改动产生的英文/乱码注释与提示文案改回中文。
- 2026-04-13 按“全部改动文件中文化”要求二次清理：`mqttpage.cpp` 与 `mainwindow.cpp` 的乱码注释/字符串改为中文，并修复多处因乱码合并造成的注释吞代码问题。
- 2026-04-13 按新要求调整 MQTT 路由读取：默认从配置文件读取，不再写入 host/topic 初始值；缺少关键键时直接返回错误并记录日志。
- 2026-04-13 新增双路由配置示例文件：`ic_board/core_migration_audit/net_cfg_mqtt_dual_route_example.ini`。

## Core 对照结论（在线二维码）
- Core 扫码上报阶段（`my_qr_code_online`）只做上报，不蜂鸣：`Core/Src/my_qr_code.c:128-153`。
- Core 收到服务端 `sendFloorQr` 后会执行：`beep()` + `rs485_send_data(...)` + `relay_ctrl(...)`：`Core/Src/floor.c:512-535`。
- Qt 当前链路：
  - 扫码上报：`IcMqttGateway::onOnlineQrScanned` -> `publishEvent("sendQrCode",...)`。
  - 下行执行：`executeSendFloorQr` 仅 `requestRs485Send(frame)`，未实现蜂鸣器和本地继电器联动。
