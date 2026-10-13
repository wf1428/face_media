# 修改意见文档

> **RK3566 移植说明**：本文档记录的是原 T113 基线，设备路径和播放器实现已不再作为当前代码依据。RK3566 + Qt 5.12.8 的实际配置请参见 [`rk3566_porting.md`](rk3566_porting.md)。


**项目名称**: qt_ycest (电梯IC卡控制与多媒体管理系统)  
**文档日期**: 2026-04-24  
**作者**: Dulin  
**项目路径**: `F:\T113-proj\liu_work\qt_ycest_1024x768_v2\qt_ycest_1024x768\qt_ycest_v4\V2\qt_ycest\`

---

## 一、架构层面修改意见

### 1.1 数据库操作应移至工作线程

**问题文件**: `V2/qt_ycest/common/sql/dbstore.cpp`

**当前问题**: 所有 `DbStore` 静态方法在主线程执行，`insertSwipeLog()`、`markUploaded()` 等写操作在高频刷卡场景下会阻塞 UI 线程。

**修改建议**: 创建 `DbWorker` 类，将数据库操作移至独立线程，通过信号槽异步通信。

```cpp
/* 建议的 DbWorker 类 */
class DbWorker : public QObject {
    Q_OBJECT
public slots:
    void doInsertSwipeLog(const QString& ts, const QString& cardId,
                          const QString& cardType, int result,
                          const QString& reason, const QByteArray& raw);
    void doMarkUploaded(const QList<qint64>& ids);
signals:
    void insertSwipeLogResult(bool ok);
    void markUploadedResult(bool ok);
};
```

**优先级**: 🔴 高

---

### 1.2 MQTT 消息解析应异步化

**问题文件**: `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp#L1071-L1147`

**当前问题**: `handleIpcLine()` 在主线程执行 JSON 解析，大消息或容错解析（`extractPayloadObjectFromBrokenMsg`）可能阻塞事件循环。

**修改建议**: 将 IPC 数据读取和 JSON 解析移至工作线程，解析完成后通过信号将结果传递到主线程。

**优先级**: 🟡 中

---

### 1.3 事件桥接应支持多订阅者过滤

**问题文件**: `V2/qt_ycest/ic_board/ic_event_bridge.h`

**当前问题**: `IcEventBridge` 是全局单例，所有信号广播给所有订阅者，无法按条件过滤。例如 `cardPassed` 信号同时被 `IcOffline` 和 `IcMqttGateway` 接收，但两者关注的数据不同。

**修改建议**: 增加事件过滤机制或使用 Qt 的 `QSignalMapper` / Lambda 按条件分发。

**优先级**: 🟢 低

---

## 二、安全层面修改意见

### 2.1 修复命令注入漏洞

**问题文件**: `V2/qt_ycest/ic_board/ic_mqtt_gateway.cpp#L496-L515`

**当前问题**: `setSystemTime()` 使用 `QProcess::execute("/bin/sh", ...)` 执行 `date -s` 命令，存在命令注入风险。

**修改建议**: 使用参数化方式设置时间，避免 shell 解释器展开。

```cpp
/* 修改前 */
const QString cmd = QString("date -s '%1'").arg(trimmed);
const int rc = QProcess::execute("/bin/sh", QStringList() << "-lc" << cmd);

/* 修改后 - 使用 clock_settime 系统调用 */
bool IcMqttGateway::setSystemTime(const QString &t, QString *err) const
{
    const QDateTime dt = QDateTime::fromString(t.trimmed(), "yyyy-MM-dd HH:mm:ss");
    if (!dt.isValid()) {
        if (err) *err = QString("时间格式非法: %1").arg(t);
        return false;
    }
    timespec ts{};
    ts.tv_sec = dt.toSecsSinceEpoch();
    ts.tv_nsec = 0;
    if (::clock_settime(CLOCK_REALTIME, &ts) != 0) {
        if (err) *err = QString("clock_settime 失败: %1").arg(strerror(errno));
        return false;
    }
    return true;
}
```

**注意**: `MqttService::setSystemTimeFromLocal()` 已经使用了 `clock_settime`，应统一采用相同方式。

**优先级**: 🔴 高

---

### 2.2 MQTT 密码加密存储

**问题文件**: `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp#L331-L343`

**当前问题**: MQTT 密码明文存储在 INI 文件中。

**修改建议**: 使用 AES-256 加密存储密码，运行时解密。密钥可基于设备唯一 ID 派生。

```cpp
/* 建议的加密存储方案 */
QString encryptPassword(const QString &password, const QByteArray &key)
{
    /* 使用 AES-256-CBC 加密 */
    /* 密钥来源: 设备ID的SHA256哈希前32字节 */
    /* 返回 Base64 编码的密文 */
}

QString decryptPassword(const QString &encrypted, const QByteArray &key)
{
    /* 使用 AES-256-CBC 解密 */
}
```

**优先级**: 🔴 高

---

### 2.3 RS485 指令白名单

**问题文件**: `V2/qt_ycest/ic_board/ic_mqtt_gateway.cpp#L580-L591`

**当前问题**: `executeSetJdq()` 无任何输入校验，任意数据可直接发送到 RS485 总线。

**修改建议**: 实现指令白名单或正则校验。

```cpp
/* 建议的白名单校验 */
static bool isValidJdqCommand(const QByteArray &frame)
{
    /* 允许的指令格式:
     * - "getjdq" (查询)
     * - "#@...#!" (36字节开梯帧)
     * - "aa<3位楼层号>" (单层开放)
     * - "bb<3位楼层号>" (单层限行)
     * - "OH<32位HEX>" (批量开放)
     * - "CL<32位HEX>" (批量限行)
     * - "OPEN<128位0/1>" (全量同步)
     */
    static const QRegularExpression re(
        R"(^(getjdq|#@.{32}#!|aa-?\d{3}|bb-?\d{3}|OH[0-9A-Fa-f]{32}|CL[0-9A-Fa-f]{32}|OPEN[01]{128})$)"
    );
    return re.match(QString::fromLatin1(frame)).hasMatch();
}
```

**优先级**: 🔴 高

---

## 三、代码质量修改意见

### 3.1 消除 `methon` 拼写错误

**问题文件**: `V2/qt_ycest/ic_board/ic_mqtt_gateway.cpp#L409-L415`

**当前问题**: `buildBasePayload()` 和 `publishEvent()` 中使用 `methon` 而非 `method`，这是为了兼容服务端协议中的拼写错误。但在代码中同时存在 `method` 和 `methon` 两种写法，容易混淆。

**修改建议**: 
1. 在协议层保持 `methon` 兼容
2. 在代码内部统一使用 `method`
3. 仅在序列化/反序列化时进行字段名映射

```cpp
/* 建议的统一方案 */
QJsonObject IcMqttGateway::buildBasePayload(const QString &method) const
{
    QJsonObject payload;
    payload["methon"] = method;  /* 协议兼容: 服务端使用 methon */
    payload["id"] = nextMessageId();
    payload["Time"] = currentTimeString();
    return payload;
}
```

**优先级**: 🟡 中

---

### 3.2 消除重复的 `qobject_cast`

**问题文件**: `V2/qt_ycest/mainwindow.cpp#L116-L268`

**当前问题**: `qobject_cast<MultimediaDemo*>(app3Page)` 在 `MainWindow` 构造函数中出现了 8 次以上，代码冗余。

**修改建议**: 在构造函数开头缓存 cast 结果。

```cpp
/* 修改前 */
auto *cast = qobject_cast<MultimediaDemo*>(app3Page);
if (cast) { /* ... */ }
// ... 多次重复
auto *player = qobject_cast<MultimediaDemo*>(app3Page);
if (player) { /* ... */ }

/* 修改后 */
auto *mmDemo = qobject_cast<MultimediaDemo*>(app3Page);
/* 后续统一使用 mmDemo */
```

**优先级**: 🟡 中

---

### 3.3 统一配置路径管理

**问题文件**: `V2/qt_ycest/mainwindow.cpp#L530`、`V2/qt_ycest/components/settings/mqtt/mqttservice.h#L116-L117`

**当前实现**: 配置路径由平台层统一解析为 `/mnt/UDISK/res/static/demoResources/images/logo/net_cfg.ini`；启动时缺失的磁盘文件从 qrc 模板创建，运行期间不再回退到 qrc。

**修改建议**: 创建统一的配置路径管理类。

```cpp
/* 建议的 AppConfig 类 */
class AppConfig {
public:
    static QString configDir() { return "/mnt/app"; }
    static QString configFilePath() { return configDir() + "/net_cfg.ini"; }
    static QString videoDir() { return "/mnt/UDISK/res/static/demoResources/images/video"; }
    static QString dbPath() { return configDir() + "/demo.db"; }
    static QString heartbeatFile() { return "/run/app_stack/qt_heartbeat"; }
    static QString ipcSocketPath() { return "/tmp/mqttd.sock"; }
};
```

**优先级**: 🟡 中

---

### 3.4 消除 `QProcess` 重复 include

**问题文件**: `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp#L6-L8`

```cpp
#include <QProcess>
#include <QTimer>
#include <QProcess>   /* 重复 include */
```

**修改建议**: 移除重复的 `#include <QProcess>`。

**优先级**: 🟢 低

---

### 3.5 `splitTopics()` 使用已弃用 API

**问题文件**: `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp#L224`

```cpp
const QStringList parts = s.split(';', QString::SkipEmptyParts);
```

**当前问题**: `QString::SkipEmptyParts` 在 Qt 5.15 中已弃用，应使用 `Qt::SkipEmptyParts`。

**修改建议**:

```cpp
/* 修改前 */
const QStringList parts = s.split(';', QString::SkipEmptyParts);

/* 修改后 */
const QStringList parts = s.split(';', Qt::SkipEmptyParts);
```

**优先级**: 🟢 低

---

### 3.6 DbStore 线程安全

**问题文件**: `V2/qt_ycest/common/sql/dbstore.cpp`

**当前问题**: `m_lastError` 是静态成员变量，多线程访问时存在数据竞争。注释中也承认了这一点：

```cpp
/* 线程不安全：如果多线程访问同一 DbStore，需要额外同步或做 TLS */
QString DbStore::m_lastError;
```

**修改建议**: 使用 `QThreadStorage` 或 `QMutex` 保护。

```cpp
/* 方案1: 使用 QMutex */
static QMutex s_errorMutex;
static void setErr(const QString& s) {
    QMutexLocker locker(&s_errorMutex);
    DbStore::m_lastError = s;
}
```

**优先级**: 🟡 中

---

## 四、功能层面修改意见

### 4.1 增加离线刷卡日志上报机制

**问题文件**: `V2/qt_ycest/common/sql/dbstore.cpp`

**当前问题**: `swipe_log` 表有 `uploaded` 字段，`queryUnuploadedLogs()` 方法也已实现，但缺少定时上报逻辑。

**修改建议**: 在 `IcMqttGateway` 或 `MqttManager` 中增加定时上报未上传刷卡日志的逻辑。

```cpp
/* 建议的定时上报逻辑 */
void MqttManager::startSwipeLogUploadTimer()
{
    QTimer *timer = new QTimer(this);
    timer->setInterval(60000);  /* 每60秒检查一次 */
    connect(timer, &QTimer::timeout, this, [this]() {
        auto logs = DbStore::queryUnuploadedLogs(50);
        if (logs.isEmpty()) return;
        /* 构建上报报文并发送 */
        /* 成功后调用 DbStore::markUploaded() */
    });
    timer->start();
}
```

**优先级**: 🟡 中

---

### 4.2 增加 MQTT 连接状态 UI 指示

**问题文件**: `V2/qt_ycest/mainwindow.cpp`

**当前问题**: `mqttConnMarkChanged` 信号已发出但主窗口未使用它来显示连接状态指示器。

**修改建议**: 在主界面添加 MQTT 连接状态图标，根据 `mqttConnMarkChanged(int mark)` 的值显示不同状态：
- 0: IPC 已连接 (灰色)
- 1: IPC 断开 (红色)
- 2: MQTT 已连接 (绿色)
- 3: MQTT 断开 (红色)

**优先级**: 🟡 中

---

### 4.3 增加磁盘空间不足保护

**问题文件**: `V2/qt_ycest/components/multimedia/disk_usage_monitor.cpp`

**当前问题**: 已有磁盘使用监控，但缺少在磁盘空间不足时自动停止 FTP 下载的保护机制。

**修改建议**: 当磁盘使用率超过 90% 时，自动暂停 FTP 下载并通知服务端。

**优先级**: 🟡 中

---

### 4.4 增加看门狗喂狗机制

**问题文件**: `V2/qt_ycest/main.cpp#L82-L101`

**当前问题**: 心跳文件写入 `/run/app_stack/qt_heartbeat`，但未与硬件看门狗联动。如果应用死锁但心跳定时器仍在触发，看门狗无法检测到。

**修改建议**: 增加事件循环健康检查，只有事件循环正常运行时才更新心跳文件。

```cpp
/* 建议的改进 */
static void setupQtHeartbeat(QCoreApplication *app)
{
    auto updateHeartbeat = []() {
        /* 检查事件循环是否阻塞 */
        static qint64 lastUpdateTime = 0;
        qint64 now = QDateTime::currentSecsSinceEpoch();
        if (lastUpdateTime > 0 && (now - lastUpdateTime) > 15) {
            /* 事件循环可能阻塞，不更新心跳让看门狗重启 */
            return;
        }
        lastUpdateTime = now;
        /* 写入心跳文件 */
        QFile file(kHeartbeatFile);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            QTextStream out(&file);
            out << now << "\n";
            file.flush();
            file.close();
        }
    };
    updateHeartbeat();
    QTimer *heartbeatTimer = new QTimer(app);
    QObject::connect(heartbeatTimer, &QTimer::timeout, app, updateHeartbeat);
    heartbeatTimer->start(8000);
}
```

**优先级**: 🟡 中

---

## 五、代码规范修改意见

### 5.1 统一命名风格

**当前问题**: 项目中混用了多种命名风格：
- `ic_board` 目录使用下划线命名
- `CursorOverlay` 使用 PascalCase
- `ic_mqtt_gateway` 使用下划线命名
- `MqttManager` 使用 PascalCase
- `buildBasePayload` 使用 camelCase
- `kLenVisitor` 使用 k 前缀常量

**修改建议**: 制定统一的命名规范文档，新代码遵循规范，旧代码逐步迁移。

**优先级**: 🟢 低

---

### 5.2 增加错误码体系

**当前问题**: 错误处理主要使用字符串描述，缺少统一的错误码体系。

**修改建议**: 定义枚举型错误码，便于上层处理和日志统计。

```cpp
/* 建议的错误码定义 */
enum class AppError {
    Success = 0,
    DbOpenFailed = 1001,
    DbQueryFailed = 1002,
    SerialOpenFailed = 2001,
    SerialReadError = 2002,
    MqttIpcConnectFailed = 3001,
    MqttPublishFailed = 3002,
    Rs485SendFailed = 4001,
    FtpDownloadFailed = 5001,
    CardCheckFailed = 6001,
};
```

**优先级**: 🟢 低

---

## 六、修改优先级汇总

| 优先级 | 编号 | 修改项 | 类型 |
|--------|------|--------|------|
| 🔴 高 | 2.1 | 修复命令注入漏洞 | 安全 |
| 🔴 高 | 2.2 | MQTT密码加密存储 | 安全 |
| 🔴 高 | 2.3 | RS485指令白名单 | 安全 |
| 🔴 高 | 1.1 | 数据库操作移至工作线程 | 架构 |
| 🟡 中 | 1.2 | MQTT消息解析异步化 | 架构 |
| 🟡 中 | 3.1 | 消除methon拼写错误 | 代码质量 |
| 🟡 中 | 3.2 | 消除重复qobject_cast | 代码质量 |
| 🟡 中 | 3.3 | 统一配置路径管理 | 代码质量 |
| 🟡 中 | 3.6 | DbStore线程安全 | 代码质量 |
| 🟡 中 | 4.1 | 离线刷卡日志上报 | 功能 |
| 🟡 中 | 4.2 | MQTT连接状态UI指示 | 功能 |
| 🟡 中 | 4.3 | 磁盘空间不足保护 | 功能 |
| 🟡 中 | 4.4 | 看门狗喂狗机制 | 功能 |
| 🟢 低 | 1.3 | 事件桥接过滤机制 | 架构 |
| 🟢 低 | 3.4 | 消除重复include | 代码质量 |
| 🟢 低 | 3.5 | 修复弃用API | 代码质量 |
| 🟢 低 | 5.1 | 统一命名风格 | 规范 |
| 🟢 低 | 5.2 | 增加错误码体系 | 规范 |
