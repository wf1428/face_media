# 工程冗余代码清理报告

- 清理日期：2026-08-05
- 工程：`MergedQtApp`
- 执行方式：以 `MergedQtApp_V2_板坏后/MergedQtApp` 为清理前基线，逐符号对照定义、调用、构建清单及 Qt 隐式调用风险
- 验证边界：按任务要求未编译、未链接、未运行

## 1. 工程概况

工程包含主融合程序和可独立构建的 `FaceGateQt` 子工程。清理后保留 87 个 `.cpp`、94 个 `.h`、4 个 `.ui`、2 个 `.qrc` 和 3 个 `.pro` 文件。

本轮不改变业务流程、协议字段、信号槽顺序、线程模型、硬件初始化或配置格式。复核后恢复了被普通调用或可能由 Qt 元对象动态调用的函数；仅对项目内可闭环验证的私有/内部参数调整签名。

当前目录下的 `.git` 是空目录，无法取得 Git 基线或生成可靠的独立补丁。因此本轮直接修改工作区，并通过本报告记录变更；未声称执行 `git diff --check` 或补丁 dry-run。

## 2. 低风险清理项

| 编号 | 文件（清理前位置） | 类型 | 符号/内容 | 判断依据 | 操作 |
|---|---|---|---|---|---|
| L01 | `components/multimedia/multimediademo.cpp:49`（清理前） | 静态函数 | `isSingleLetterFloorFrame` | 基线中仅有 1 处定义、0 处调用；同文件的 `isFloorUpperLetterAscii` 有实际调用，已恢复保留 | 仅删除 `isSingleLetterFloorFrame` |
| L02 | `components/settings/networkpage.cpp:177` | 静态函数 | `pingHostOnce` | 删除重复目录后全工程仅剩定义，无调用 | 删除 |
| L03 | `common/protocols/signalboard_decoder.h:50` | 类型 | `FloorParsed` | 类型名全工程只出现一次，未实例化、未注册元类型 | 删除 |
| L04 | `components/multimedia/gst_player_widget.h:92-366` | 私有成员 | 两个未启动诊断定时器及 12 个从未读写的 `lastReported*` 字段 | 每个字段全工程只有声明 | 删除 |
| L05 | `components/multimedia/multimediademo.h:311,358` | 私有成员 | `m_liveRetrying`、`pixmap_floor`、`pixmap_state` | 全工程只有声明 | 删除 |
| L06 | `components/settings/networkpage.h:122-123` | 私有成员 | `lastDhcpHealthCheckMs_`、`dhcpHealthFailCount_` | 删除重复副本后确认全工程只有声明 | 删除 |
| L07 | `components/settings/mqtt/ic_mqtt_gateway.h:122` | 私有成员 | `ioStarted_` | 全工程只有声明 | 删除 |
| L08 | `components/udisk_status/udisk_status.h:72` | 私有成员 | `m_doneTipToken` | 全工程只有声明 | 删除 |
| L09 | `components/settings/ftppage.h:49,354` | 前置声明/成员 | `FtpWorker`、`worker`、`workerThread` | 没有类定义、对象创建、方法调用或线程启动 | 删除 |
| L10 | 多个头文件和源文件 | 重复/无效 include | 重复的 `QPixmap`、`QDateTime`、`errno.h`、`QSettings`、`QPushButton`、`QMessageBox`、`QProcess`，以及无调用点的 `QDebug` 等 | 同文件重复，或对应声明在文件中没有直接用途；`volumepopup.cpp` 自身调用 `qDebug()`，因此在实现文件中保留直接包含 | 删除无效项并补齐直接依赖 |
| L11 | `common/clients/icboard_client.cpp` 等 | 调试输出 | 原始串口帧、连接结果、MQTT `[DBG]`、离线校验时间与次数字节输出 | 仅产生日志，不改变状态或返回值 | 删除 |
| L12 | `common/debug/probe_log.h`、`auto_connect_manager.cpp`、`snapshot_processor.cpp`、`qt_ycest.pro` | 注释旧代码 | 停用的探针文件输出、旧 MQTT 自动连接实现、旧截图副本、旧 T113/GST 构建片段 | 已注释且存在当前替代实现 | 删除 |

## 3. 中风险清理项

下列函数已直接对照清理前副本。普通函数只有在基线中确认为“声明 + 定义、0 调用”，并复核 `connect`、`SIGNAL/SLOT`、`invokeMethod`、自动槽命名、函数指针和字符串引用后才删除。

| 编号 | 文件（清理前位置） | 符号 | 潜在风险复核 | 操作 |
|---|---|---|---|---|
| M01 | `components/multimedia/multimediademo.cpp:1033-1108` | `pruneMissingVideos`、`isCurrentVideoRectPicture`、`reloadVideoRectPictureFileList` | 非槽、非虚函数、无调用 | 删除声明和定义 |
| M02 | `components/settings/mqttpage.cpp:422` | `normalizeTopicText` | 非槽、无调用 | 删除声明和定义 |
| M03 | `components/settings/mqtt/ic_mqtt_gateway.cpp:354-367` | `currentFloorString`、`extractCardId` | 私有普通函数、无调用 | 删除声明和定义 |
| M04 | `ic_board/ic_offline_qr.cpp:23-67` | `isFloorQrFormat`、`extractMd5Date`、`checkNotExpiredByDate`、`extractPayload32Fixed` | 不在当前二维码解析入口调用，无元对象/回调用途 | 删除声明和定义；`bytesOf` 被 `handleFrame()` 调用，已保留 |
| M05 | `MysqlFaceRepository.cpp:1274-1370,2368` | `openLocalStore`、`isLocalFile` | 私有函数、当前存储入口不调用 | 删除声明和定义 |
| M06 | `FaceGateQt/ui/MainWindow.cpp:880-894` | `promptAdminPasswordChange` | 私有普通函数；当前流程直接调用 `handleAdminPasswordChange` | 删除声明和定义 |

## 4. 删除函数与同职责逻辑对照

“基线引用”按符号名在清理前副本中的实际出现位置统计；静态函数必须在其所属 `.cpp` 内单独判断，其他源文件中的同名函数不能作为其定义。

| 候选符号 | 清理前基线证据 | 当前同职责逻辑及实际调用 | 最终处理 |
|---|---|---|---|
| `isFloorUpperLetterAscii` | 1 处定义、5 处调用；其中 3 处独立业务调用仍存在 | 无跨文件实现可以替代本文件的 `static` 定义 | **已恢复保留** |
| `isSingleLetterFloorFrame` | 1 处定义、0 调用 | `signalboard_decoder.cpp` 的 `isSingleLetterFloor` 有调用，但属于另一翻译单元和解码流程，不能替代本函数；本函数自身仍为 0 调用 | 删除 |
| `pruneMissingVideos` | 1 声明 + 1 定义、0 调用 | `pruneMissingVideoRectMedia` 有 5 处调用，维护当前视频/图片混播列表 | 删除旧的纯视频列表清理函数 |
| `isCurrentVideoRectPicture` | 1 声明 + 1 定义、0 调用 | 状态字段 `m_currentVideoRectIsImage` 在当前流程中被直接读取 10 次以上 | 删除无调用包装函数 |
| `reloadVideoRectPictureFileList` | 1 声明 + 1 定义、0 调用 | `rebuildVideoRectMediaList` 有 6 处调用，同时重建视频和图片列表 | 删除旧的仅图片扫描函数 |
| `restoreResourcesOnPlay` | 1 声明 + 1 定义、0 普通调用，但它位于 `private slots` | `applyVideoQualitySettings` 有显式调用，但不能排除槽的动态调用 | **已恢复保留** |
| `testVideoOptimizations` | 1 声明 + 1 定义 + 1 个 `QTimer::singleShot` 调用 | 无替代函数；原调用仍属于初始化流程 | **已恢复函数和定时调用** |
| `normalizeTopicText` | 1 声明 + 1 定义、0 调用 | 当前配置解析中的局部 `splitTopics` lambda 有 2 处调用；基线里的 `mqttmanager.cpp` 同名静态函数也为 0 调用且作用域独立 | 删除未调用成员函数 |
| `mqttmanager.cpp` 静态 `splitTopics` | 1 处定义、0 调用 | `mqttpage.cpp` 的局部 `splitTopics` 有 2 处调用；`mqttmanager.cpp` 当前持久化流程使用的是 `joinTopicsForIni` | 删除未调用静态函数 |
| `currentFloorString`、`extractCardId` | 各 1 声明 + 1 定义、0 调用 | `onCardPassed(cardId, floor, rawFrame)` 由信号连接触发，直接使用上游传入的卡号和楼层 | 删除 |
| `bytesOf` | 1 定义 + `handleFrame()` 1 处调用 | 无替代定义；其他文件即使存在同名静态函数也不可见 | **已恢复保留** |
| `isFloorQrFormat`、`extractMd5Date`、`checkNotExpiredByDate`、`extractPayload32Fixed` | 各 1 声明 + 1 定义、0 调用 | `OfflineQr::handleFrame` 当前直接执行分段、日期、MD5 和 payload 校验 | 删除未进入入口的旧辅助函数 |
| `openLocalStore`、`isLocalFile` | 各 1 声明 + 1 定义、0 调用 | `openSqliteStore` 有 2 处入口调用，`isSqlite` 有 3 处调用；当前已明确禁用独立 FGDB | 删除旧本地文件库入口/判断 |
| `promptAdminPasswordChange` | 1 声明 + 1 定义、0 调用 | `adminPasswordChangeRequested` 直接连接 `handleAdminPasswordChange` | 删除无调用包装函数 |
| `pingHostOnce` | 两个重复源文件中各 1 定义，均为 0 调用 | 其他组件存在异步 `QProcess` ping，但不是 `NetworkPage` 的直接替代调用；本函数自身确为未使用 | 删除 |
| `FaceImageSyncBridge::requestImageSync` / `imageSyncRequested` | 槽为 1 声明 + 1 定义，signal 由槽发出 | 源码无普通调用，但 `public slot`/signal 属于 Qt 动态接口 | **已恢复保留** |
| `ProbeLog::defaultProbeFilePath` | 1 定义，仅在已注释代码内出现 | 当前 `writeLineRaw` 不再写独立 probe 文件 | 删除停用日志路径函数 |
| `FtpPage::testTcpConnect` | 仅 1 处声明，无定义、无调用 | 当前 FTP 流程使用现有异步连接/进程逻辑 | 删除孤立声明 |

## 5. 未使用参数复核

| 位置 | 基线/调用证据 | 操作 |
|---|---|---|
| `DiskUsageMonitor(QWidget*, QObject*, QString, double)` 的 `parent` | 构造函数仅 1 个创建点；参数在函数体仅 `Q_UNUSED`，实际 QWidget 父对象由 `mainWindow` 设置 | 删除参数并同步唯一调用点 |
| `NetworkStatusMonitor(QWidget*, QObject*, QString)` 的 `parent` | 构造函数仅 1 个创建点；参数在函数体仅 `Q_UNUSED`，实际 QWidget 父对象由 `mainWindow` 设置 | 删除参数并同步唯一调用点 |
| `IcToast::startPassEdgeFlash(int totalMs)` 的 `totalMs` | 私有函数仅 2 个调用；参数只进入 `Q_UNUSED`，实际时长固定由 `passPulsePeriodMs_ * passPulseCount_` 计算 | 改为无参函数并同步 2 个调用点 |
| 预加载错误 lambda 的 `msg` | 信号签名要求接收 `QString`，lambda 不使用值 | 保留类型、去掉形参名 |
| 直播探测 lambda 的 `scheme`、`desiredKind` 捕获 | lambda 内仅 `Q_UNUSED`，业务判断在外层/其他回调完成 | 删除无效捕获 |
| `localFile`、`url`、`kind` | 参数在函数后续实际使用，只是残留了错误的 `Q_UNUSED` | **保留参数**，仅删除错误标记 |
| Qt 事件、虚函数、信号槽、GStreamer/硬件回调及接口实现参数 | 签名由框架、函数指针或对外接口约束 | 全部保留 |

## 6. 文件与资源清理

| 文件 | 依据 | 操作 |
|---|---|---|
| `components/multimedia/networkpage.cpp/.h` | 与 `components/settings/networkpage.cpp/.h` SHA-256 完全一致，且未进入任一构建清单 | 删除重复副本 |
| `components/multimedia/tplayer_decode.cpp/.h` | 未进入任一构建清单；全工程没有 `TPlayerWidget` 外部引用；当前播放器为 `GstPlayerWidget` | 删除已替代实现 |
| `components/features/offline_dialog.h` | `StatusWindow` 仅在本文件定义，未创建、未引用；内容为调试状态窗口 | 从构建清单移除并删除 |
| `modules/facegate/FaceGateQt/resources.qrc` | 与 `facegate_resources.qrc` SHA-256 完全一致 | 独立子工程改用 `facegate_resources.qrc` 后删除重复文件 |

`resources.qrc` 中的楼层图片存在 `%1` 动态拼接和 `:/static` 根目录组合，不能按字面量零命中删除，因此全部保留。两个现存 QRC 的所有物理资源文件均已确认存在。

## 7. 高风险保留项

| 项目 | 保留原因 |
|---|---|
| `rs485_dir.c` | 虽未进入 Qt qmake 清单，但它是独立 Linux 内核模块，提供 `/dev/rs485_dir` ABI，属于硬件控制代码 |
| `modules/facegate/FaceGateQt/main.cpp` 与 `FaceGateQt.pro` | 仍是可独立构建的人脸模块入口，不属于冗余源文件 |
| Qt signals/slots、`eventFilter`、事件函数、虚函数和回调 | 可能通过元对象、框架或外部接口隐式调用 |
| `ic_offline_checker.cpp` 中协议迁移对照说明 | 涉及 103/115 字节卡协议、楼层权限和时段校验；作为协议审计依据保留，未修改活动逻辑 |
| 公共访问器、协议结构体、序列化字段、线程与硬件状态字段 | 无普通调用点不足以证明无用，按高风险默认保留 |
| 根 QRC 中通过动态路径加载的图标、字母、数字和状态图片 | 字符串由 `%1`、`QDir(":/static")` 等方式构造，不能仅靠精确文本搜索判断 |

## 8. 构建系统核对

- `qt_ycest.pro` 中列出的源文件、头文件、UI 和 QRC 均存在。
- `FaceGateQt.pro` 已改为引用现存的 `facegate_resources.qrc`。
- 删除文件在 `.pro`、源码 include、UI 和 QRC 中均无残留引用。
- 清理后的工程仅有 `rs485_dir.c` 未进入 Qt 构建；该文件为有意保留的独立内核模块。
- 未发现 `.bak`、`.old`、`.orig`、`.tmp`、`.rej`、`*_backup`、`*_copy` 或 `*_test` 文件。

## 9. 静态验证结果

已完成：

1. 以清理前副本逐项复核所有被删除函数：已完成；发现并恢复 6 个相关符号（含 signal）及 1 个定时调用。
2. 翻译单元内静态函数复核：`isFloorUpperLetterAscii` 1 定义 + 3 个现存调用，`bytesOf` 1 定义 + 1 调用。
3. 普通成员函数复核：保留项检查声明/定义/调用，删除项均在基线中为 0 调用。
4. Qt 隐式入口复核：恢复 `FaceImageSyncBridge` 公共槽/signal 和 `restoreResourcesOnPlay` 私有槽。
5. 内部参数签名复核：3 个被删除参数的全部调用点已同步；框架和接口参数未改。
6. 主工程 qmake 文件存在性检查：通过。
7. QRC 物理资源存在性检查：通过。
8. 已删除文件引用检查：通过。
9. 直接依赖检查：所有调用 `qDebug/qWarning/qInfo/qCritical` 的源码均直接包含 `<QDebug>`；使用标准智能指针或 `std::move` 的源码均直接包含 `<memory>` 或 `<utility>`。

未执行：编译、链接、运行、目标板测试。原因是用户明确要求由其手动测试。

## 10. 建议手动验证顺序

1. 分别打开主工程和独立 `FaceGateQt.pro`，确认 qmake 能正确读取。
2. 编译主融合工程，重点观察 MOC、头文件直接依赖和未使用参数警告。
3. 验证本地视频轮播、直播切换、截图和音量控制。
4. 验证 IC/信号板串口、离线二维码、RS485 方向控制和楼层显示。
5. 验证 MQTT/FTP 自动连接与下载流程。
6. 验证 FaceGate 摄像头、识别、活体、管理员密码修改和本地数据库读写。
7. 单独构建 `FaceGateQt.pro`，确认共享 QRC 能正常生成资源代码。

## 11. 完整变更文件清单

以下清单逐文件记录本轮实际改动，不包含只读扫描过但未修改的文件。

### 11.1 构建与报告

| 文件 | 变更 |
|---|---|
| `qt_ycest.pro` | 移除 `offline_dialog.h` 构建项和失效的旧 GStreamer/T113 注释配置 |
| `modules/facegate/FaceGateQt/FaceGateQt.pro` | 独立工程改用 `facegate_resources.qrc` |
| `docs/code_cleanup_report_20260805.md` | 新增并持续补充本清理报告 |

### 11.2 common

| 文件 | 变更 |
|---|---|
| `common/clients/icboard_client.cpp` | 删除原始串口帧调试输出及不再需要的 `QDebug` 包含 |
| `common/clients/icboard_client.h` | 删除未使用的 `QTimer` 包含 |
| `common/clients/signalboard_client.h` | 补齐头文件内联 `std::move` 所需的 `<utility>` 直接依赖 |
| `common/debug/probe_log.h` | 删除停用的独立日志文件写入代码、路径函数及其无效包含 |
| `common/face_image_sync_bridge.cpp/.h` | 恢复 Qt 公共槽 `requestImageSync` 及其 `imageSyncRequested` signal，避免动态接口误删 |
| `common/protocols/signalboard_decoder.h` | 删除未使用的 `FloorParsed` 结构体 |
| `common/sql/dbstore.cpp` | 删除未使用的 `QDebug` 包含 |
| `common/workers/icboard_worker.cpp` | 删除未使用的 `QDebug` 包含 |
| `common/workers/signalboard_worker.cpp` | 删除注释掉的逐帧调试输出及未使用的 `QDebug` 包含 |

### 11.3 components

| 文件 | 变更 |
|---|---|
| `components/features/featuresdialog.h` | 删除未使用的 `offline_dialog.h` 和 `QDebug` 包含；实现文件本身已有 `<QDebug>` 直接包含 |
| `components/features/key_service.cpp` | 删除未使用的 `QDebug` 包含 |
| `components/multimedia/gst_player_widget.h` | 删除未使用的诊断定时器和 `lastReported*` 成员 |
| `components/multimedia/disk_usage_monitor.cpp/.h` | 删除未使用的构造参数 `parent`，同步唯一创建点 |
| `components/multimedia/multimediademo.cpp` | 删除经基线确认无调用的辅助函数；恢复 `isFloorUpperLetterAscii`、测试函数及其定时调用；整理内部 lambda 参数/捕获和监视器创建参数 |
| `components/multimedia/multimediademo.h` | 同步删除无引用成员/函数声明，恢复两个 Qt 槽声明，并补齐日志宏所需的 `<QDebug>` 直接依赖 |
| `components/multimedia/netinfo_popup.h` | 删除重复的 `QSettings` 包含 |
| `components/multimedia/network_status_monitor.cpp/.h` | 删除未使用的构造参数 `parent`，同步唯一创建点 |
| `components/multimedia/snapshot_processor.cpp` | 删除已注释的旧截图复制实现 |
| `components/multimedia/volumepopup.h` | 删除不属于头文件接口的 `QDebug` 包含 |
| `components/multimedia/volumepopup.cpp` | 补充实际调用 `qDebug()` 所需的 `<QDebug>` 直接包含，修复间接包含断裂 |
| `components/settings/auto_connect_manager.cpp` | 删除整段停用的旧 MQTT 自动连接实现 |
| `components/settings/ftppage.h` | 删除重复/无效包含、仅声明无定义的 `testTcpConnect`、未定义的 `FtpWorker` 指针和未使用的 worker 线程成员；补齐 `std::shared_ptr`/`std::move` 所需的 `<memory>` 与 `<utility>` 直接依赖 |
| `components/settings/mqttpage.cpp` | 删除无引用私有函数、无效果的 SSL 状态 lambda 和 `[DBG]` 输出；移除实际使用参数上的错误 `Q_UNUSED` |
| `components/settings/mqttpage.h` | 同步删除无引用私有函数声明 |
| `components/settings/mqtt/ic_mqtt_gateway.cpp` | 删除两个无引用私有函数 |
| `components/settings/mqtt/ic_mqtt_gateway.h` | 同步删除声明及未使用状态字段 |
| `components/settings/mqtt/mqttmanager.cpp` | 删除 0 调用的静态 `splitTopics`，并补齐 Qt 日志函数所需的 `<QDebug>` 直接依赖 |
| `components/settings/mqtt/mqttservice.cpp` | 删除未使用的 `QDebug` 包含 |
| `components/settings/networkpage.cpp` | 删除无调用的 `pingHostOnce` |
| `components/settings/networkpage.h` | 删除两个未使用的 DHCP 健康检查字段 |
| `components/settings/settingsdialog.cpp` | 补齐 Qt 日志函数所需的 `<QDebug>` 直接依赖 |
| `components/udisk_status/udisk_status.h` | 删除未使用的 `m_doneTipToken` |
| `mainwindow.cpp` | 删除实际已使用的 `url`、`kind` 参数上的错误 `Q_UNUSED` 标记，并补齐 `<QDebug>` 直接依赖 |

### 11.4 ic_board 与 FaceGate

| 文件 | 变更 |
|---|---|
| `ic_board/core_migration_audit/core_network_card_compat.cpp` | 删除无效时间调试输出、仅供日志使用的临时字符串和 `QDebug` 包含 |
| `ic_board/ic_offline_checker.cpp` | 删除无效时间/次数字节调试输出及日志专用临时字符串；补齐现存日志调用所需的 `<QDebug>` 直接依赖 |
| `ic_board/ic_offline_checker.h` | 删除未使用的 `QDebug` 包含 |
| `ic_board/ic_offline_qr.cpp` | 删除四个无调用辅助函数；保留 `handleFrame()` 实际调用的 `bytesOf` |
| `ic_board/ic_offline_qr.h` | 同步删除四个成员函数声明 |
| `ic_board/ic_toast.cpp/.h` | 删除私有动画函数未使用的 `totalMs` 参数并同步两个调用点 |
| `ic_board/ic_worker.cpp` | 删除未使用的 `cardId` 局部变量及随之无用的直接包含 |
| `modules/facegate/FaceGateQt/database/MysqlFaceRepository.cpp` | 删除两个无调用私有函数 |
| `modules/facegate/FaceGateQt/database/MysqlFaceRepository.h` | 同步删除声明 |
| `modules/facegate/FaceGateQt/core/CameraCaptureBackend.cpp` | 补齐标准智能指针所需的 `<memory>` 直接依赖 |
| `modules/facegate/FaceGateQt/core/CameraService.cpp` | 补齐标准智能指针所需的 `<memory>` 直接依赖 |
| `modules/facegate/FaceGateQt/ui/MainWindow.cpp` | 删除无调用的密码修改包装函数 |
| `modules/facegate/FaceGateQt/ui/MainWindow.h` | 同步删除声明 |

### 11.5 删除文件

| 文件 | 删除原因 |
|---|---|
| `components/features/offline_dialog.h` | 未引用调试窗口 |
| `components/multimedia/networkpage.cpp` | 与 `components/settings/networkpage.cpp` 完全重复且未构建 |
| `components/multimedia/networkpage.h` | 与 `components/settings/networkpage.h` 完全重复且未构建 |
| `components/multimedia/tplayer_decode.cpp` | 已被 GStreamer 播放器替代且未构建 |
| `components/multimedia/tplayer_decode.h` | 已被 GStreamer 播放器替代且未构建 |
| `modules/facegate/FaceGateQt/resources.qrc` | 与 `facegate_resources.qrc` 完全重复 |
