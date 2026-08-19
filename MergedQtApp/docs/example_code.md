# 示例代码文档

> **RK3566 移植说明**：本文档记录的是原 T113 基线，设备路径和播放器实现已不再作为当前代码依据。RK3566 + Qt 5.12.8 的实际配置请参见 [`rk3566_porting.md`](rk3566_porting.md)。


**项目名称**: qt_ycest (电梯IC卡控制与多媒体管理系统)  
**文档日期**: 2026-04-24  
**作者**: Dulin  
**项目路径**: `F:\T113-proj\liu_work\qt_ycest_1024x768_v2\qt_ycest_1024x768\qt_ycest_v4\V2\qt_ycest\`

---

## 一、安全修复示例代码

### 1.1 命令注入修复 - setSystemTime

**问题文件**: `V2/qt_ycest/ic_board/ic_mqtt_gateway.cpp#L496-L515`

**修改前** (存在命令注入风险):

```cpp
bool IcMqttGateway::setSystemTime(const QString &t, QString *err) const
{
    const QString trimmed = t.trimmed();
    if (trimmed.isEmpty()) {
        if (err) *err = "时间为空";
        return false;
    }
    const QDateTime dt = QDateTime::fromString(trimmed, "yyyy-MM-dd HH:mm:ss");
    if (!dt.isValid()) {
        if (err) *err = QString("时间格式非法: %1").arg(trimmed);
        return false;
    }
    const QString cmd = QString("date -s '%1'").arg(trimmed);
    const int rc = QProcess::execute("/bin/sh", QStringList() << "-lc" << cmd);
    if (rc != 0) {
        if (err) *err = QString("date -s 执行失败: rc=%1").arg(rc);
        return false;
    }
    return true;
}
```

**修改后** (使用系统调用，安全):

```cpp
#include <time.h>
#include <errno.h>
#include <string.h>

bool IcMqttGateway::setSystemTime(const QString &t, QString *err) const
{
    const QString trimmed = t.trimmed();
    if (trimmed.isEmpty()) {
        if (err) *err = "时间为空";
        return false;
    }
    const QDateTime dt = QDateTime::fromString(trimmed, "yyyy-MM-dd HH:mm:ss");
    if (!dt.isValid()) {
        if (err) *err = QString("时间格式非法: %1").arg(trimmed);
        return false;
    }

    timespec ts{};
    ts.tv_sec = dt.toSecsSinceEpoch();
    ts.tv_nsec = 0;

    if (::clock_settime(CLOCK_REALTIME, &ts) != 0) {
        if (err) *err = QString("clock_settime 失败: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    return true;
}
```

---

### 1.2 RS485 指令白名单校验

**问题文件**: `V2/qt_ycest/ic_board/ic_mqtt_gateway.cpp#L580-L591`

**修改后** (增加白名单校验):

```cpp
#include <QRegularExpression>

bool IcMqttGateway::executeSetJdq(const QString &cmd, QString *err)
{
    const QByteArray frame = cmd.toLatin1();
    if (frame.isEmpty()) {
        if (err) *err = "setjdq 指令为空";
        return false;
    }

    if (!isValidRs485Frame(frame)) {
        if (err) *err = QString("setjdq 指令格式非法: %1").arg(QString::fromLatin1(frame));
        return false;
    }

    IcEventBridge::instance()->requestRs485Send(frame);
    emit logMessage(QString("setjdq -> RS485 cmd=%1").arg(QString::fromLatin1(frame)));
    return true;
}

bool IcMqttGateway::isValidRs485Frame(const QByteArray &frame) const
{
    if (frame.size() > 256) return false;

    if (frame == "getjdq") return true;

    if (frame.size() == 36 &&
        frame[0] == '#' && frame[1] == '@' &&
        frame[34] == '#' && frame[35] == '!') {
        return true;
    }

    if (frame.startsWith("aa") || frame.startsWith("bb")) {
        if (frame.size() == 5) return true;
    }

    if (frame.startsWith("OH") || frame.startsWith("CL")) {
        if (frame.size() == 34) {
            QByteArray hex = frame.mid(2, 32);
            bool valid = true;
            for (char c : hex) {
                if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
                    valid = false;
                    break;
                }
            }
            return valid;
        }
    }

    if (frame.startsWith("OPEN") && frame.size() == 132) {
        for (int i = 4; i < frame.size(); ++i) {
            if (frame[i] != '0' && frame[i] != '1') return false;
        }
        return true;
    }

    return false;
}
```

---

## 二、架构改进示例代码

### 2.1 数据库异步工作线程

**问题文件**: `V2/qt_ycest/common/sql/dbstore.cpp`

**建议新增文件**: `common/sql/db_worker.h` / `common/sql/db_worker.cpp`

```cpp
/* db_worker.h */
#ifndef DB_WORKER_H
#define DB_WORKER_H

#include <QObject>
#include <QThread>
#include <QList>
#include "dbstore.h"

class DbWorker : public QObject
{
    Q_OBJECT
public:
    explicit DbWorker(QObject *parent = nullptr);

public slots:
    void doInsertSwipeLog(const QString& ts,
                          const QString& cardId,
                          const QString& cardType,
                          int result,
                          const QString& reason,
                          const QByteArray& raw);
    void doMarkUploaded(const QList<qint64>& ids);
    void doCheckpoint();
    void doVacuum();

signals:
    void insertSwipeLogResult(bool ok);
    void markUploadedResult(bool ok);
};

class DbAsync : public QObject
{
    Q_OBJECT
public:
    explicit DbAsync(QObject *parent = nullptr);
    ~DbAsync();

    void insertSwipeLog(const QString& ts,
                        const QString& cardId,
                        const QString& cardType,
                        int result,
                        const QString& reason,
                        const QByteArray& raw = QByteArray());

signals:
    void requestInsertSwipeLog(QString ts, QString cardId,
                               QString cardType, int result,
                               QString reason, QByteArray raw);
    void insertSwipeLogResult(bool ok);

private:
    QThread workerThread_;
    DbWorker *worker_ = nullptr;
};

#endif
```

```cpp
/* db_worker.cpp */
#include "db_worker.h"

DbWorker::DbWorker(QObject *parent)
    : QObject(parent)
{
}

void DbWorker::doInsertSwipeLog(const QString& ts,
                                const QString& cardId,
                                const QString& cardType,
                                int result,
                                const QString& reason,
                                const QByteArray& raw)
{
    bool ok = DbStore::insertSwipeLog(ts, cardId, cardType, result, reason, raw);
    emit insertSwipeLogResult(ok);
}

void DbWorker::doMarkUploaded(const QList<qint64>& ids)
{
    bool ok = DbStore::markUploaded(ids);
    emit markUploadedResult(ok);
}

void DbWorker::doCheckpoint()
{
    DbStore::checkpoint();
}

void DbWorker::doVacuum()
{
    DbStore::vacuum();
}

DbAsync::DbAsync(QObject *parent)
    : QObject(parent)
{
    worker_ = new DbWorker();
    worker_->moveToThread(&workerThread_);

    connect(&workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(this, &DbAsync::requestInsertSwipeLog,
            worker_, &DbWorker::doInsertSwipeLog);
    connect(worker_, &DbWorker::insertSwipeLogResult,
            this, &DbAsync::insertSwipeLogResult);

    workerThread_.start();
}

DbAsync::~DbAsync()
{
    workerThread_.quit();
    workerThread_.wait();
}

void DbAsync::insertSwipeLog(const QString& ts,
                             const QString& cardId,
                             const QString& cardType,
                             int result,
                             const QString& reason,
                             const QByteArray& raw)
{
    emit requestInsertSwipeLog(ts, cardId, cardType, result, reason, raw);
}
```

---

### 2.2 统一配置路径管理

**问题文件**: 多处硬编码路径

**建议新增文件**: `common/app_config.h`

```cpp
/* app_config.h */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <QString>

class AppConfig
{
public:
    static QString configDir() { return QStringLiteral("/mnt/app"); }
    static QString configFilePath() { return configDir() + QStringLiteral("/net_cfg.ini"); }
    static QString videoDir() { return QStringLiteral("/mnt/UDISK/res/static/demoResources/images/video"); }
    static QString dbPath() { return configDir() + QStringLiteral("/demo.db"); }
    static QString heartbeatFile() { return QStringLiteral("/run/app_stack/qt_heartbeat"); }
    static QString ipcSocketPath() { return QStringLiteral("/tmp/mqttd.sock"); }

    static QString qrDevice() { return QStringLiteral("/dev/ttyAS1"); }
    static int qrBaudRate() { return 9600; }

    static QString cardDevice() { return QStringLiteral("/dev/ttyAS2"); }
    static int cardBaudRate() { return 9600; }

    static QString rs485Device() { return QStringLiteral("/dev/ttyAS4"); }
    static int rs485BaudRate() { return 9600; }
    static QString rs485DirDevice() { return QStringLiteral("/dev/rs485_dir"); }

    static QString rtcDevice() { return QStringLiteral("/dev/rtc1"); }
};

#endif
```

---

## 三、功能增强示例代码

### 3.1 刷卡日志定时上报

**问题文件**: `V2/qt_ycest/common/sql/dbstore.cpp` (已有 queryUnuploadedLogs)

**建议在 MqttManager 中增加**:

```cpp
/* 在 MqttManager 构造函数中添加 */
MqttManager::MqttManager(QObject *parent)
    : QObject(parent)
{
    /* ... 现有代码 ... */

    swipeLogUploadTimer_ = new QTimer(this);
    swipeLogUploadTimer_->setInterval(60000);
    connect(swipeLogUploadTimer_, &QTimer::timeout,
            this, &MqttManager::onSwipeLogUploadTimeout);
}

void MqttManager::onSwipeLogUploadTimeout()
{
    if (!ipc_ || !ipc_->isConnected()) return;

    QList<SwipeLogRow> logs = DbStore::queryUnuploadedLogs(50);
    if (logs.isEmpty()) return;

    QJsonArray logArray;
    QList<qint64> ids;

    for (const auto &row : logs) {
        QJsonObject obj;
        obj["id"] = row.id;
        obj["ts"] = row.ts;
        obj["cardId"] = row.cardId;
        obj["cardType"] = row.cardType;
        obj["result"] = row.result;
        obj["reason"] = row.reason;
        logArray.append(obj);
        ids.append(row.id);
    }

    QJsonObject payload;
    payload["methon"] = QStringLiteral("swipeLogUpload");
    payload["id"] = QString::number(QDateTime::currentMSecsSinceEpoch());
    payload["Time"] = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    payload["data"] = logArray;

    QJsonObject root;
    root["cmd"] = "publish";
    root["topic"] = cfg_.publishTopic.trimmed();
    root["payload"] = payload;

    if (ipc_->sendJsonLine(root)) {
        DbStore::markUploaded(ids);
        emit logMessage(QString("刷卡日志上报成功: %1条").arg(logs.size()));
    }
}
```

---

### 3.2 事件循环健康检查

**问题文件**: `V2/qt_ycest/main.cpp#L82-L101`

**修改后** (增加健康检查):

```cpp
static void setupQtHeartbeat(QCoreApplication *app)
{
    static const char *kHeartbeatFile = "/run/app_stack/qt_heartbeat";
    static qint64 lastEventLoopTick = 0;

    auto updateHeartbeat = []() {
        qint64 now = QDateTime::currentSecsSinceEpoch();

        if (lastEventLoopTick > 0) {
            qint64 gap = now - lastEventLoopTick;
            if (gap > 15) {
                qWarning() << "事件循环可能阻塞:" << gap << "秒";
                return;
            }
        }

        lastEventLoopTick = now;

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

---

## 四、代码质量改进示例代码

### 4.1 MainWindow 构造函数简化

**问题文件**: `V2/qt_ycest/mainwindow.cpp#L74-L472`

**修改建议** (提取方法，减少构造函数长度):

```cpp
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    ui->statusbar->hide();

    initStackedPages();
    initCursorOverlay();
    initSettingsDialog();
    initMultimediaConnections();
    initMenuButtons();
    initKeyService();
    initFeatureSettings();

    QTimer::singleShot(200, this, SLOT(setupUdiskOverlayLater()));

    if (cursor) cursor->hide();
}

void MainWindow::initStackedPages()
{
    menuPage = this->takeCentralWidget();
    stack = new QStackedWidget(this);
    this->setCentralWidget(stack);
    stack->addWidget(menuPage);

    app3Page = new MultimediaDemo(this);
    app3Page->setWindowFlags(Qt::Widget);
    stack->addWidget(app3Page);
    stack->setCurrentWidget(app3Page);

    this->showFullScreen();

    this->setMouseTracking(true);
    stack->setMouseTracking(true);
    menuPage->setMouseTracking(true);
    app3Page->setMouseTracking(true);

    filePage = new FileManagerPage(this);
    stack->addWidget(filePage);

    connect(filePage, &FileManagerPage::backToMenuRequested, this, [=](){
        stack->setCurrentWidget(menuPage);
    });
}

void MainWindow::initMultimediaConnections()
{
    auto *mm = qobject_cast<MultimediaDemo*>(app3Page);
    if (!mm) return;

    connect(mm, &MultimediaDemo::backToMenuRequested, this, [=]() {
        stack->setCurrentWidget(menuPage);
    });

    connect(stack, &QStackedWidget::currentChanged, this, [=](int){
        if (stack->currentWidget() == app3Page) {
            QTimer::singleShot(0, mm, [mm]() {
                mm->resumeLocalPlaylistFromStart();
            });
        } else {
            QTimer::singleShot(0, mm, [mm]() {
                mm->leaveAndStopVideo();
            });
        }
    });

    /* ... 其余多媒体相关连接 ... */
}
```

---

### 4.2 调试日志条件编译

**问题文件**: `V2/qt_ycest/common/clients/icboard_client.cpp#L55-L56`

**修改后**:

```cpp
void IcBoardClient::onReadyRead()
{
    const QByteArray data = m_port.readAll();

#ifdef QT_DEBUG
    qDebug() << "rx" << data.toHex(' ');
    qDebug() << "rx_ascii" << data;
#endif

    if (!data.isEmpty()) {
        m_splitter.feed(data);
    }
}
```

或使用更灵活的日志级别控制:

```cpp
void IcBoardClient::onReadyRead()
{
    const QByteArray data = m_port.readAll();

#if defined(ENABLE_SERIAL_DEBUG_LOG)
    qDebug() << "rx" << data.toHex(' ');
#endif

    if (!data.isEmpty()) {
        m_splitter.feed(data);
    }
}
```

在 `.pro` 文件中:

```
# 调试选项 (发布时注释掉)
# DEFINES += ENABLE_SERIAL_DEBUG_LOG
```

---

### 4.3 弃用 API 修复

**问题文件**: `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp#L224`

```cpp
/* 修改前 */
const QStringList parts = s.split(';', QString::SkipEmptyParts);

/* 修改后 */
const QStringList parts = s.split(';', Qt::SkipEmptyParts);
```

---

## 五、测试辅助示例代码

### 5.1 IC板帧模拟器

用于测试 IC 板协议解析的模拟器:

```cpp
/* icboard_frame_simulator.h */
#ifndef ICBOARD_FRAME_SIMULATOR_H
#define ICBOARD_FRAME_SIMULATOR_H

#include <QByteArray>
#include <QString>

class IcBoardFrameSimulator
{
public:
    static QByteArray buildPassFrame();
    static QByteArray buildVisitorCardFrame(const QString &cardId, int relayNum, int relayTimes);
    static QByteArray buildMultiCardFrame(const QString &cardId, int relayNum, int relayTimes);
};

#endif
```

```cpp
/* icboard_frame_simulator.cpp */
#include "icboard_frame_simulator.h"

QByteArray IcBoardFrameSimulator::buildPassFrame()
{
    QByteArray frame(36, '0');
    frame[0] = '#';
    frame[1] = '@';
    frame[34] = '#';
    frame[35] = '!';
    return frame;
}

QByteArray IcBoardFrameSimulator::buildVisitorCardFrame(
    const QString &cardId, int relayNum, int relayTimes)
{
    QByteArray frame(103, '0');
    frame[0] = '#';
    frame[1] = '@';

    QByteArray id = cardId.toLatin1();
    if (id.size() <= 8) {
        for (int i = 0; i < id.size(); ++i) {
            frame[35 + i] = id[i];
        }
    }

    QString hexTimes = QString("%1").arg(relayTimes, 2, 16, QChar('0')).toUpper();
    frame[99] = hexTimes[0].toLatin1();
    frame[100] = hexTimes[1].toLatin1();

    QString decNum = QString("%1").arg(relayNum, 2, 10, QChar('0'));
    frame[101] = decNum[0].toLatin1();
    frame[102] = decNum[1].toLatin1();

    return frame;
}

QByteArray IcBoardFrameSimulator::buildMultiCardFrame(
    const QString &cardId, int relayNum, int relayTimes)
{
    QByteArray frame(115, '0');
    frame[0] = '#';
    frame[1] = '@';

    QByteArray id = cardId.toLatin1();
    if (id.size() <= 8) {
        for (int i = 0; i < id.size(); ++i) {
            frame[35 + i] = id[i];
        }
    }

    QString hexTimes = QString("%1").arg(relayTimes, 2, 16, QChar('0')).toUpper();
    frame[111] = hexTimes[0].toLatin1();
    frame[112] = hexTimes[1].toLatin1();

    QString decNum = QString("%1").arg(relayNum, 2, 10, QChar('0'));
    frame[113] = decNum[0].toLatin1();
    frame[114] = decNum[1].toLatin1();

    return frame;
}
```

---

### 5.2 信号板帧模拟器

```cpp
/* signalboard_frame_simulator.cpp */
#include <QByteArray>

QByteArray buildSignalBoardFrame(char direction, char mode,
                                  char floorTen, char floorOne)
{
    QByteArray frame(12, 0);
    frame[0] = 0x02;   /* STX */
    frame[1] = direction;  /* '0'=停, '1'=上行, '3'=下行 */
    frame[5] = mode;       /* '8'=正常, '1'=满员, '2'=超载, '3'=消防, '4'=检修 */
    frame[7] = floorTen;   /* 十位或前缀 */
    frame[8] = floorOne;   /* 个位 */
    frame[11] = 0x03;  /* ETX */
    return frame;
}

/* 使用示例 */
auto frame = buildSignalBoardFrame('1', '8', '1', '5');  /* 上行, 正常, 15楼 */
```
