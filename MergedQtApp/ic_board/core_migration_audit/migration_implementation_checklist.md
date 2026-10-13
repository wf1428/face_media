# Core -> qt_ycest_v3 移植实施清单（刷卡/二维码）

更新时间：2026-04-09 17:36
范围：`qt_ycest` 全工程中与刷卡、二维码、IC-MQTT、RS485联动相关代码（排除人脸机、消防）

## 实施清单

- [x] Step 1：实现刷卡白名单校验（M1）
- [x] Step 2：二维码密钥改为读取运行配置（M2）
- [x] Step 3：补齐 getjdq 回包上报链路（M3）
- [x] Step 4：getTrafficFloor 的 Floor 字段改为 128 位位图（M4）
- [x] Step 5：补齐 set_to_zkb 对应配置入口（M5）
- [x] Step 6：全工程刷卡/二维码链路复核与一致性说明文档

## 约束确认

- M6（sector_num 修改后下发刷卡器串口）按需求不实施。
