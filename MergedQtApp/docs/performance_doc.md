# 性能文档

> **RK3566 移植说明**：本文档记录的是原 T113 基线，设备路径和播放器实现已不再作为当前代码依据。RK3566 + Qt 5.12.8 的实际配置请参见 [`rk3566_porting.md`](rk3566_porting.md)。


**项目名称**: qt_ycest (电梯IC卡控制与多媒体管理系统)  
**文档日期**: 2026-04-24  
**作者**: Dulin  
**项目路径**: `F:\T113-proj\liu_work\qt_ycest_1024x768_v2\qt_ycest_1024x768\qt_ycest_v4\V2\qt_ycest\`

---

## 一、性能概述

本项目运行在 Allwinner T113-S4 (双核 ARM Cortex-A7 @ 1.2GHz, 64MB DDR2) 嵌入式平台上，资源极为有限。性能优化对系统稳定性至关重要。

---

## 二、关键性能指标

### 2.1 定时器与轮询频率

| 定时器 | 间隔 | CPU 开销评估 | 来源文件 |
|--------|------|-------------|---------|
| 心跳写入 | 8000ms | 极低 (文件IO) | `main.cpp#L100` |
| IPC 重连 | 1000ms | 低 (条件判断) | `mqttmanager.cpp#L383` |
| 信号板在线检测 | 800ms | 低 (时间比较) | `signalboard_worker.cpp` |
| IC 网关保活 | 60000ms | 极低 (MQTT publish) | `ic_mqtt_gateway.cpp#L267` |
| U盘检测 | 300ms | 中 (文件系统遍历) | `mainwindow.cpp#L523` |
| 磁盘使用监控 | 30000ms | 中 (statfs 系统调用) | `disk_usage_monitor.cpp` |
| 离线刷卡等待通过帧 | 600ms | 低 (定时器等待) | `icboard_splitter.cpp` |
| 二维码回包超时 | 500ms | 低 (定时器等待) | `ic_mqtt_gateway.cpp#L88` |
| 旧式刷卡楼层等待 | 10ms | 极低 (定时器等待) | `ic_mqtt_gateway.cpp#L272` |

---

## 三、性能瓶颈分析

### 3.1 串口数据处理

**文件路径**: `V2/qt_ycest/common/clients/icboard_client.cpp#L53-L58`

```cpp
void IcBoardClient::onReadyRead() {
    const QByteArray data = m_port.readAll();
    qDebug() << "rx" << data.toHex(' ');
    qDebug() << "rx_ascii" << data;
    if (!data.isEmpty()) {
        m_splitter.feed(data);
    }
}
```

**问题**:
1. **调试日志开销**: `qDebug()` 输出 hex 和 ASCII 两种格式的原始数据，在 9600 波特率下每帧约 100 字节，hex 格式约 300 字符。高频刷卡场景下日志输出会阻塞事件循环。
2. **`readAll()` 阻塞**: 虽然串口数据量小，但 `readAll()` 在主线程执行，若数据量大可能影响 UI 响应。

**建议**: 生产版本应移除或条件编译调试日志。

---

### 3.2 MQTT IPC 消息处理

**文件路径**: `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp#L1071-L1147`

```cpp
void MqttManager::handleIpcLine(const QByteArray &line)
{
    QByteArray raw = line.trimmed();
    emit logMessage(QString("RX: %1").arg(QString::fromUtf8(raw)));
    // ... JSON 解析 ...
}
```

**问题**:
1. **每条消息都 emit logMessage**: 日志信号连接到 UI 更新，频繁触发可能导致 UI 卡顿。
2. **JSON 解析在主线程**: `QJsonDocument::fromJson()` 在主线程执行，大消息可能阻塞事件循环。
3. **容错解析开销**: `extractPayloadObjectFromBrokenMsg()` 进行字符级扫描（brace matching），对于大消息开销显著。

**来源文件**: `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp#L33-L109`

---

### 3.3 SQLite 数据库操作

**文件路径**: `V2/qt_ycest/common/sql/dbstore.cpp`

**已有优化**:
- WAL 模式: 提高并发读写性能
- `synchronous=NORMAL`: 减少 fsync 开销
- `temp_store=MEMORY`: 减少磁盘 IO
- 批量更新使用事务: `markUploaded()` 使用 `execBatch()`

**潜在问题**:
1. **主线程数据库操作**: 所有 `DbStore` 方法在主线程执行，`insertSwipeLog()` 等写操作在高频刷卡场景下可能阻塞 UI。
2. **`openIfNeeded()` 重复调用**: 每个数据库操作都调用 `openIfNeeded()`，包含 `QSqlDatabase::contains()` 和 `isOpen()` 检查，虽然开销小但可优化。
3. **`query()` 返回 QVariantMap 列表**: 大量数据时内存占用较高。

---

### 3.4 视频解码

**文件路径**: `V2/qt_ycest/components/multimedia/tplayer_decode.cpp`

**已有优化**:
- 使用全志 CedarX 硬件加速 (`libtplayer`, `libxplayer`)
- 避免使用 Qt Multimedia 的软件解码

**潜在问题**:
1. **帧回调频率**: 视频帧回调可能在解码线程触发，若回调中有 UI 操作需注意线程安全。
2. **截图 base64 编码**: `updateCurrentPlayImage()` 将整帧图像编码为 base64 字符串存储在内存中，1024×768 分辨率的 JPEG 约 50-100KB，base64 后约 70-140KB。

---

### 3.5 U盘状态检测

**文件路径**: `V2/qt_ycest/components/udisk_status/udisk_status.cpp`

**问题**: U盘检测定时器间隔 300ms，每次检测可能涉及文件系统遍历，在 T113-S4 的慢速 Flash 上开销不可忽略。

---

### 3.6 资源文件加载

**文件路径**: `V2/qt_ycest/resources.qrc`

**问题**: Qt 资源系统将所有资源编译到二进制中，包含大量图片（字母/数字图片 36 张、空调图标 7 张、楼层状态图标 8 张等），增加启动时间和内存占用。

---

## 四、内存使用分析

### 4.1 主要内存消耗点

| 组件 | 预估内存 | 说明 |
|------|---------|------|
| Qt 资源系统 | 2-5 MB | 所有图片、音频、配置文件 |
| SQLite WAL | 1-5 MB | WAL 文件大小取决于写入频率 |
| 视频解码缓冲 | 3-6 MB | 1024×768 NV12 格式约 1.5MB/帧 |
| MQTT 消息缓冲 | < 1 MB | IPC 接收缓冲 |
| 串口数据缓冲 | < 100 KB | 分帧器缓冲区 |
| base64 截图 | 70-140 KB | 当前播放画面截图 |

### 4.2 内存泄漏风险

**文件路径**: `V2/qt_ycest/mainwindow.cpp#L343`

```cpp
FeaturesDialog *featuresDialog = new FeaturesDialog(this);
// ...
delete featuresDialog;
```

**说明**: `FeaturesDialog` 使用 `new` 创建并在 `exec()` 后手动 `delete`，虽然功能正确，但建议使用 `QScopedPointer` 或设置 `WA_DeleteOnClose` 属性。

---

## 五、启动性能

### 5.1 启动流程

**文件路径**: `V2/qt_ycest/main.cpp#L104-L143`

```
1. 安装消息处理器
2. 设置 UTF-8 编码
3. 创建 QApplication
4. 连接 aboutToQuit 信号 (DB 收尾)
5. 启动心跳定时器
6. 创建 SplashScreen (全屏显示)
7. 创建 MainWindow (初始化所有组件)
8. 连接 splash → mainwindow 切换
9. IcToast::instance()
10. IcIoBootstrap::startAll()
11. 进入事件循环
```

### 5.2 启动耗时分析

| 阶段 | 预估耗时 | 优化空间 |
|------|---------|---------|
| QApplication 创建 | 100-200ms | 低 |
| 资源加载 | 200-500ms | 中 (可延迟加载) |
| MainWindow 初始化 | 300-600ms | 高 (可延迟初始化) |
| DB bootstrap | 50-100ms | 低 |
| 串口打开 | 50-100ms | 低 |
| MQTT IPC 连接 | 100-300ms | 中 (异步即可) |

---

## 六、网络性能

### 6.1 MQTT 消息延迟

**来源文件**: `V2/qt_ycest/components/settings/mqtt/mqttmanager.cpp`

**延迟组成**:
1. IPC 传输: < 1ms (Unix Domain Socket 本地通信)
2. mqttd 处理: 1-5ms
3. 网络传输: 10-500ms (取决于网络质量)
4. 消息解析: < 1ms (JSON 解析)

**总延迟**: 约 12-507ms

### 6.2 FTP 下载性能

**来源文件**: `V2/qt_ycest/components/settings/ftppage.cpp`

**影响因素**:
- 网络带宽
- T113-S4 的 TCP/IP 栈性能
- 存储写入速度 (NAND Flash 约 5-15 MB/s)

---

## 七、性能优化建议

### 7.1 高优先级

| 编号 | 优化项 | 预期收益 | 来源文件 |
|------|--------|---------|---------|
| P1 | 移除/条件编译串口调试日志 | 减少 30% 主线程 IO 开销 | `icboard_client.cpp#L55-L56` |
| P2 | 数据库写操作移至工作线程 | 消除 UI 卡顿 | `dbstore.cpp` |
| P3 | MQTT 日志限流/异步输出 | 减少 UI 更新频率 | `mqttmanager.cpp#L1074` |
| P4 | 延迟加载非关键组件 | 缩短启动时间 200-400ms | `mainwindow.cpp` |

### 7.2 中优先级

| 编号 | 优化项 | 预期收益 | 来源文件 |
|------|--------|---------|---------|
| P5 | 资源文件按需加载 | 减少内存占用 1-3MB | `resources.qrc` |
| P6 | U盘检测降频至 1000ms | 减少 CPU 开销 | `mainwindow.cpp#L523` |
| P7 | base64 截图按需生成 | 减少内存占用 70-140KB | `mqttservice.cpp` |
| P8 | IPC 消息解析移至工作线程 | 消除大消息阻塞 | `mqttmanager.cpp` |

### 7.3 低优先级

| 编号 | 优化项 | 预期收益 | 来源文件 |
|------|--------|---------|---------|
| P9 | 合并心跳文件写入 | 减少 Flash 写入次数 | `main.cpp#L86-L94` |
| P10 | 优化 DbStore::openIfNeeded() 缓存 | 减少重复检查 | `dbstore.cpp` |
| P11 | 使用 QScopedPointer 管理 FeaturesDialog | 防止内存泄漏 | `mainwindow.cpp#L343` |

---

## 八、性能监控建议

1. **添加帧率监控**: 在 `MultimediaDemo` 中添加 FPS 计数器
2. **添加事件循环延迟监控**: 使用 `QTimer` 检测事件循环阻塞
3. **添加内存使用监控**: 定期读取 `/proc/self/status` 的 VmRSS 字段
4. **添加数据库操作耗时统计**: 在 `DbStore` 中添加耗时日志
5. **添加 MQTT 消息处理耗时统计**: 在 `handleIpcLine()` 中添加耗时日志
