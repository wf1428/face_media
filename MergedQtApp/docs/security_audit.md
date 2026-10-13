# 安全审查报告

> **RK3566 移植说明**：本文档记录的是原 T113 基线，设备路径和播放器实现已不再作为当前代码依据。RK3566 + Qt 5.12.8 的实际配置请参见 [`rk3566_porting.md`](rk3566_porting.md)。


**项目名称**: qt_ycest (电梯IC卡控制与多媒体管理系统)  
**审查日期**: 2026-04-24  
**审查人**: Dulin  
**项目路径**: `F:\T113-proj\liu_work\qt_ycest_1024x768_v2\qt_ycest_1024x768\qt_ycest_v4\V2\qt_ycest\`

---

## 一、审查概述

本项目是一个运行在 T113-S4 嵌入式 Linux 平台上的 Qt 应用程序，主要功能包括：电梯IC卡离线/在线刷卡控制、RS485分层板通信、MQTT远程管理、多媒体播放、FTP文件下载等。本报告从代码层面进行安全审查，识别潜在的安全风险。

---

## 二、高危安全问题

### 2.1 MQTT 密码明文存储与传输

**文件路径**: `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp#L630`

```cpp
mqtt["password"] = cfg_.password;
```

**文件路径**: `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp#L331-L343`

```cpp
ini.setValue("password_online_v1", password);
ini.setValue("password_online_v2", password);
```

**风险描述**: MQTT 密码以明文形式存储在 `/mnt/UDISK/res/static/demoResources/images/logo/net_cfg.ini` 配置文件中，同时通过 IPC (Unix Domain Socket) 以明文 JSON 传输给 mqttd 守护进程。任何有文件系统访问权限的进程或用户均可读取密码。

**风险等级**: 🔴 高危  
**影响范围**: MQTT Broker 凭据泄露，可能导致未授权访问消息队列

---

### 2.2 FTP 凭据明文存储

**文件路径**: `V2/qt_ycest/components/settings/mqtt/mqttservice.cpp#L163-L168`

```cpp
set.beginGroup("ftp");
set.setValue("host", host);
set.setValue("port", portStr);
set.setValue("username", user);
set.setValue("password", pwd);
set.endGroup();
```

**风险描述**: FTP 用户名和密码以明文写入 `/mnt/UDISK/res/static/demoResources/images/logo/net_cfg.ini` 的 `[ftp]` 分组。FTP 凭据来自 MQTT 远程下发的 `videoControl` 消息中的 `ftpUser`/`ftpPwd` 字段，整个链路无加密保护。

**风险等级**: 🔴 高危  
**影响范围**: FTP 服务器凭据泄露，可能导致未授权文件上传/下载

---

### 2.3 远程重启无认证机制

**文件路径**: `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp#L876-L879`

```cpp
QTimer::singleShot(1000, this, []() {
    QProcess::startDetached("/sbin/reboot");
});
```

**文件路径**: `V2/qt_ycest/ic_board/ic_mqtt_gateway.cpp#L728-L731`

```cpp
if (msg.method == "reboot") {
    publishSuccess("rebootSuccess", msg.id, QJsonObject{}, "ic/reboot");
    QProcess::startDetached("/sbin/reboot", QStringList());
    return true;
}
```

**风险描述**: 通过 MQTT 消息 `linuxReboot` 和 `reboot` 方法即可远程重启设备，无任何认证/授权校验。只要能向设备订阅的 topic 发送消息，即可触发重启。

**风险等级**: 🔴 高危  
**影响范围**: 远程拒绝服务攻击（DoS），设备可被恶意重启

---

### 2.4 远程时间设置存在命令注入风险

**文件路径**: `V2/qt_ycest/ic_board/ic_mqtt_gateway.cpp#L496-L515`

```cpp
bool IcMqttGateway::setSystemTime(const QString &t, QString *err) const
{
    const QString cmd = QString("date -s '%1'").arg(trimmed);
    const int rc = QProcess::execute("/bin/sh", QStringList() << "-lc" << cmd);
    // ...
}
```

**风险描述**: `setTime` 方法通过 `QProcess::execute` 执行 `date -s` 命令，参数来自 MQTT 消息的 `time` 字段，未对输入进行充分过滤。如果 `trimmed` 中包含 shell 元字符（如 `'; rm -rf /;`），可能导致命令注入。使用 `/bin/sh -lc` 执行会展开命令。

**风险等级**: 🔴 高危  
**影响范围**: 潜在的命令注入漏洞，可能导致系统被完全控制

---

### 2.5 RS485 远程指令执行无限制

**文件路径**: `V2/qt_ycest/ic_board/ic_mqtt_gateway.cpp#L580-L591`

```cpp
bool IcMqttGateway::executeSetJdq(const QString &cmd, QString *err)
{
    const QByteArray frame = cmd.toLatin1();
    IcEventBridge::instance()->requestRs485Send(frame);
    // ...
}
```

**风险描述**: `setjdq` 方法将 MQTT 消息中的 `data` 字段直接转换为 RS485 帧发送，无任何内容校验或白名单限制。攻击者可构造任意 RS485 帧，直接控制电梯分层板硬件。

**风险等级**: 🔴 高危  
**影响范围**: 电梯分层板被恶意控制，可能导致电梯运行安全问题

---

## 三、中危安全问题

### 3.1 SQLite 数据库无访问控制

**文件路径**: `V2/qt_ycest/common/sql/dbstore.cpp#L45-L50`

**风险描述**: 数据库文件 `demo.db` 存储在可访问路径，无文件权限设置。刷卡日志（含卡号、原始帧数据）可被其他进程读取。

**风险等级**: 🟡 中危  
**影响范围**: 刷卡记录泄露，用户隐私数据暴露

---

### 3.2 串口通信无加密

**文件路径**: `V2/qt_ycest/common/clients/icboard_client.cpp#L26-L34`

**风险描述**: 所有串口通信（IC卡读卡器 `/dev/ttyAS2`、二维码扫描器 `/dev/ttyAS1`、RS485 `/dev/ttyAS4`）均为明文传输，无加密、无校验和、无防重放机制。

**风险等级**: 🟡 中危  
**影响范围**: 刷卡数据可被窃听或伪造，RS485 指令可被重放攻击

---

### 3.3 IPC 通信无认证

**文件路径**: `V2/qt_ycest/components/settings/mqtt/mqttipcclient.h`

**风险描述**: 应用与 mqttd 守护进程之间通过 Unix Domain Socket 通信，无任何认证机制。同一设备上的其他进程可连接该 socket 并发送伪造的 MQTT 消息。

**风险等级**: 🟡 中危  
**影响范围**: 本地提权，消息伪造

---

### 3.4 调试日志泄露敏感信息

**文件路径**: `V2/qt_ycest/common/clients/icboard_client.cpp#L55-L56`

```cpp
qDebug() << "rx" << data.toHex(' ');
qDebug() << "rx_ascii" << data;
```

**文件路径**: `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp#L688-L689`

```cpp
emit logMessage(QString("消息：topic=%1 payload=%2")
                .arg(topic, QString::fromUtf8(QJsonDocument(payloadObj).toJson(QJsonDocument::Compact))));
```

**风险描述**: 大量调试日志输出包含原始串口数据、MQTT 消息内容（含密码、卡号等），在生产环境中可能泄露敏感信息。

**风险等级**: 🟡 中危  
**影响范围**: 敏感信息通过日志泄露

---

### 3.5 离线刷卡校验密钥存储风险

**文件路径**: `V2/qt_ycest/ic_board/ic_offline_checker.cpp`

**风险描述**: `OfflineChecker::check()` 方法接收 `secret8` 参数进行密钥校验，该密钥存储在数据库中，无加密保护。

**风险等级**: 🟡 中危  
**影响范围**: 离线刷卡验证可被绕过

---

## 四、低危安全问题

### 4.1 配置文件路径硬编码

**文件路径**: `V2/qt_ycest/mainwindow.cpp#L530`、`V2/qt_ycest/components/settings/mqtt/mqttservice.h#L116-L117`

**风险描述**: 所有配置路径硬编码为嵌入式设备的固定路径，缺乏灵活性。

**风险等级**: 🟢 低危

---

### 4.2 心跳文件权限未显式设置

**文件路径**: `V2/qt_ycest/main.cpp#L86-L94`

**风险描述**: 心跳文件创建时未显式设置文件权限，依赖系统 umask。

**风险等级**: 🟢 低危

---

### 4.3 重启命令包含危险操作

**文件路径**: `V2/qt_ycest/mainwindow.cpp#L436-L449`

```cpp
"sync; echo b > /proc/sysrq-trigger"
```

**风险描述**: 重启命令列表中包含 `sysrq-trigger`，这是内核级别的强制重启，可能导致文件系统损坏。

**风险等级**: 🟢 低危

---

## 五、安全建议汇总

| 编号 | 风险等级 | 问题描述 | 建议措施 |
|------|---------|---------|---------|
| 2.1 | 🔴 高危 | MQTT密码明文存储 | 使用AES加密存储，运行时解密 |
| 2.2 | 🔴 高危 | FTP凭据明文存储 | 使用证书认证替代密码 |
| 2.3 | 🔴 高危 | 远程重启无认证 | 增加挑战-应答认证机制 |
| 2.4 | 🔴 高危 | 命令注入风险 | 使用参数化执行，禁止shell展开 |
| 2.5 | 🔴 高危 | RS485指令无限制 | 实现指令白名单机制 |
| 3.1 | 🟡 中危 | 数据库无访问控制 | 设置文件权限0600，加密敏感字段 |
| 3.2 | 🟡 中危 | 串口通信无加密 | 增加CRC校验和防重放机制 |
| 3.3 | 🟡 中危 | IPC无认证 | 使用SO_PEERCRED验证连接进程 |
| 3.4 | 🟡 中危 | 日志泄露敏感信息 | 生产版本关闭调试日志 |
| 3.5 | 🟡 中危 | 离线密钥存储风险 | 使用硬件安全模块或加密存储 |
| 4.1 | 🟢 低危 | 路径硬编码 | 使用配置文件管理路径 |
| 4.2 | 🟢 低危 | 心跳文件权限 | 显式设置文件权限 |
| 4.3 | 🟢 低危 | 重启方式风险 | 移除sysrq-trigger |

---

## 六、安全架构建议

1. **引入 TLS/SSL**: MQTT 连接应启用 TLS 加密（`cfg_.tls` 字段已预留但默认为 `false`）
2. **实现访问控制列表**: 对 MQTT 下行指令实现基于角色的访问控制
3. **增加审计日志**: 对所有远程控制操作（重启、开梯、限行等）记录审计日志
4. **固件签名验证**: OTA 更新或远程下载的视频文件应进行签名校验
5. **最小权限原则**: 应用进程应以非 root 用户运行，仅保留必要的 capabilities
