# qt_ycest 主工程与多媒体逻辑复核报告

- 复核日期：2026-08-06
- 当前工程：`F:\rk3566-proj\face_media\MergedQtApp`
- 对照基线：`F:\rk3566-proj\face_media\MergedQtApp_V2_板坏后\MergedQtApp`
- 复核边界：只读检查源码、工程清单、资源和配置；未编译、未链接、未运行；未修改任何工程代码

## 1. 结论

本次重新逐文件核对后，未发现最近几次代码清理改变 `qt_ycest.pro` 主工程的有效业务流程。多媒体本地轮播、图片混播、直播切换、FTP 下载、MQTT 控制、音量、截图、网络/磁盘告警、IC/二维码/RS485 等活动链路均仍存在。

清理产生的有效代码差异可以归为四类：

1. 删除仅有定义而没有调用的旧函数、声明或状态字段。
2. 删除日志输出和已注释代码，不改变状态、分支、返回值或信号发射。
3. 删除三个仅在函数体中 `Q_UNUSED` 的内部参数，并同步全部调用点。
4. 补齐 `<QDebug>`、`<memory>`、`<utility>` 等直接依赖。

此前误删、但实际仍被调用或可能由 Qt 动态调用的符号已经保留：

| 符号 | 当前静态证据 | 结论 |
|---|---:|---|
| `isFloorUpperLetterAscii` | 1 个定义、3 个调用，共 4 次文本命中 | 必须保留，当前存在 |
| `bytesOf` | 1 个定义、1 个调用，共 2 次命中 | 必须保留，当前存在 |
| `restoreResourcesOnPlay` | 声明、定义均存在，且属于 Qt 槽 | 必须保留，当前存在 |
| `testVideoOptimizations` | 声明、定义和定时调用均存在 | 必须保留，当前存在 |
| `requestImageSync` / `imageSyncRequested` | 公共槽、signal、实现均存在 | Qt 动态接口，当前存在 |

静态复核不能代替编译和目标板运行，因此结论限定为“源码和工程清单层面未发现清理导致的活动逻辑断裂”。

## 2. 工程范围与构建关系

`MergedQtApp.pro` 只包含 `qt_ycest.pro`。`qt_ycest.pro` 构建的不是单一播放器，而是融合应用，包含：

- 应用外壳与模式切换；
- 多媒体主窗口、GStreamer 播放器和设置页；
- MQTT/FTP/网络配置；
- IC 板、信号板、二维码、RS485 和 SQLite；
- 人脸机模块；
- RK3566 平台路径和硬件抽象。

Qt 模块包括 Core、Gui、Widgets、OpenGL、SerialPort、Network、Sql、VirtualKeyboard、Qml、Quick、QuickWidgets。Linux 目标还要求 GStreamer、RGA；人脸模块按工程路径启用 InspireFace/RKNN 相关库。

工程文件引用检查结果：

| 检查项 | 引用数 | 缺失数 |
|---|---:|---:|
| `MergedQtApp.pro` | 1 | 0 |
| `qt_ycest.pro` | 187 | 0 |
| 根 `resources.qrc` | 78 | 0 |

当前目录共有 87 个 `.cpp`、94 个 `.h`、1 个 `.c`、4 个 `.ui`、2 个 `.qrc`、3 个 `.pro`。两个 Qt 工程清单合计覆盖 181 个 C/C++ 源码和头文件；唯一未进入 Qt 清单的是独立 Linux 内核模块 `rs485_dir.c`，它提供 `/dev/rs485_dir` 收发方向控制接口，不属于 Qt 可执行程序源文件。

## 3. 主程序和融合外壳

### 3.1 启动链路

`main.cpp` 的主要顺序为：

1. 设置 EGLFS/OpenGL ES、虚拟键盘和日志处理。
2. 创建 `QApplication`、心跳和 `AppShell`。
3. 初始化融合外壳和两个业务模块。
4. 显示启动画面及唯一顶层窗口。
5. 外壳显示后启动 IC 串口 I/O。
6. 退出时按模块生命周期关闭线程、播放器、相机和数据库。

`AppShell` 是 EGLFS 下唯一顶层窗口，内部 `QStackedWidget` 容纳多媒体 `MainWindow` 与 `FaceGateMainWindow`，避免两个全屏顶层窗口争抢 EGLFS。

### 3.2 模式切换

当前配置 `config/app_shell.ini` 的初始模式是 `multimedia`。调试传感器以鼠标长按模拟人员出现/离开。

切换顺序固定为：停用旧模块 → 切换栈页 → 激活新模块。如果人脸模块激活失败，控制器会回退到多媒体；如果多媒体恢复失败，则保留明确的失败状态。最近的清理没有修改 `AppShell`、`ModeController` 或两个适配器的活动代码。

## 4. qt_ycest 各部分功能与作用

| 部分 | 主要文件/目录 | 功能与作用 |
|---|---|---|
| 多媒体外层窗口 | `mainwindow.cpp/.h/.ui` | 菜单、页面栈、多媒体页、设置页、文件管理页、U 盘状态层；连接 MQTT/FTP 设置页与播放器 |
| 多媒体业务页 | `components/multimedia/multimediademo.*` | 本地视频和图片混播、录播/直播切换、播放列表、楼层和信号状态显示、时间日期、下载期间暂停/恢复、播放状态持久化 |
| GStreamer 播放 | `gst_player_widget.*` | 创建播放管线、appsink 取帧、总线状态、时长/位置、暂停/恢复/停止、预加载、截图 |
| 视频显示 | `video_canvas.*` | OpenGL 纹理显示、RGA 转换后的帧交换和渲染 |
| 直播探测与保护 | `network_status_monitor.*`、`multimediademo.*` | 检查网卡 carrier、默认路由和联网状态；探测 UDP/HTTP/RTSP/RTMP；直播重试、看门狗和本地播放保护窗口 |
| 磁盘保护 | `disk_usage_monitor.*` | 用 `statvfs` 计算 U 盘使用率并显示覆盖告警；FTP 页另有 90% 下载门槛 |
| 截图 | `snapshot_processor.*` | 将播放器视频帧贴回界面截图的播放区域，异步编码为响应所需图片 |
| 音量 | `volumepopup.*`、平台音频配置 | 本地调音量、持久化、系统 mixer 控制，并将执行结果反馈到 MQTT 回执链 |
| 网络信息 | `netinfo_popup.*`、`components/settings/networkpage.*` | 显示 IP/网关/DNS；支持 DHCP 和静态地址配置、链路状态更新 |
| FTP | `components/settings/ftppage.*` | 服务器测试、目录/文件解析、curl 子进程下载、历史记录、自动下载预检查、录播下载结果分类 |
| MQTT | `components/settings/mqttpage.*`、`components/settings/mqtt/*` | 选择 online_v1/online_v2 路由，通过本机 mqttd IPC 连接代理，分发播放、系统、人员、二维码和 IC 消息 |
| 自动连接 | `auto_connect_manager.*` | 网络可用后执行配置允许的自动任务；已注释的旧 MQTT 反射调用已删除，当前 MQTT 自身连接链未改 |
| 功能设置 | `components/features/*` | 功能模式、串口命令、按键服务和相关配置界面 |
| 文件管理 | `components/file_manager/*` | 浏览和管理本地媒体文件 |
| U 盘状态 | `components/udisk_status/*` | U 盘插拔、扫描、状态提示和完成提示 |
| 公共串口层 | `common/clients`、`common/workers`、`common/protocols` | IC 板/信号板串口客户端、工作线程、分帧和协议解码 |
| IC 业务层 | `ic_board/*` | 离线卡和访客卡校验、离线/在线二维码、刷卡事件、RS485 发送、提示框和 I/O 启停 |
| 数据库 | `common/sql/*` | 统一 SQLite、刷卡日志、配置、网络人员、二维码事务和消息幂等记录 |
| 平台抽象 | `platform/rk3566_platform.*` | U 盘、静态资源、视频、数据库、串口、RS485、音频、重启和设备序列号路径 |
| 设备配置 | `device/*` | 从 INI 同步设备和 I/O 参数，向运行模块提供当前配置 |

## 5. 多媒体主要逻辑链

### 5.1 本地视频/图片混播

1. 从 RK3566 平台视频目录扫描媒体。
2. `rebuildVideoRectMediaList` 建立视频和图片统一列表。
3. `pruneMissingVideoRectMedia` 清除运行中消失的媒体。
4. 视频交给当前播放器，下一视频可由第二播放器预加载。
5. 图片按定时器时长显示，结束后推进统一列表。
6. `videoFinished` 的两个信号连接仍在，只删除了保存连接返回值和调试打印。

被删除的 `pruneMissingVideos`、`reloadVideoRectPictureFileList` 是旧的单类型实现，当前混播链实际调用的是统一版本；删除没有切断现用列表维护。

### 5.2 直播链路

1. MQTT 或配置提供 URL。
2. 根据 `rtsp/rtmp/srt/udp/hls/http-ts` 推断直播类型。
3. 网络探测完成后，只有当请求仍为当前目标时才切换播放器。
4. 播放器错误、超时或无帧时触发重试/看门狗。
5. 本地视频切文件保护窗口内，后台直播探测不会抢占 GStreamer。
6. 收到停止直播、录播或 NONE 指令时，走对应停止/恢复链。

清理只删除了 lambda 中没有使用的两个捕获变量，没有改变探测结果或切换条件。

### 5.3 远程录播下载

1. `videoControl` 的 `RECORDED + videoPath` 生成下载任务键。
2. 相同任务重复消息合并；不同下载任务并发时返回 `download_busy`。
3. FTP 页执行目录、空间、目标文件和网络预检查，再下载到本地目录。
4. 最终结果统一回 MQTT：成功、已存在、可重试错误、本地拒绝、空间限制或初始化失败。
5. 下载期间不会切换到 LIVE；完成后更新播放器列表和播放状态。

### 5.4 截图和播放信息

`getPlayInfo` 不立即回包，而是请求界面和视频帧合成。合成完成后写入 `base64Image`，再发送 `sendPlayInfo`。截图处理中的旧注释副本被删除，当前新建目标图、绘制 UI、贴入视频帧的活动实现未改。

### 5.5 IC、楼层与状态显示

信号板帧经分帧器和解码器产生楼层、方向、满载、消防、检修等状态。`isFloorUpperLetterAscii` 仍用于字母楼层判断。IC 工作线程根据 feature 模式分流：

- `offline_v1`：本地卡/二维码校验；
- `online_v1`：ycLinux 人员、远程呼梯和二维码授权；
- `online_v2`：IC 在线网关的卡、二维码、楼层和 RS485 协议。

## 6. 最近清理与基线的逐类复核

### 6.1 未改变有效逻辑的差异

| 差异 | 基线调用情况 | 当前同职责实现 | 结论 |
|---|---:|---|---|
| 删除 `isSingleLetterFloorFrame` | 0 调用 | 当前楼层解码直接判断，`isFloorUpperLetterAscii` 保留 | 无活动逻辑变化 |
| 删除 `pruneMissingVideos` | 0 调用 | `pruneMissingVideoRectMedia` 有实际调用 | 旧纯视频路径被统一混播路径替代 |
| 删除 `isCurrentVideoRectPicture` | 0 调用 | 直接读取 `m_currentVideoRectIsImage` | 无活动逻辑变化 |
| 删除 `reloadVideoRectPictureFileList` | 0 调用 | `rebuildVideoRectMediaList` 有实际调用 | 无活动逻辑变化 |
| 删除 `normalizeTopicText` | 0 调用 | `decideRouteByFeature` 内局部 `splitTopics` 有 2 个调用 | MQTT 主题解析仍在 |
| 删除 `mqttmanager.cpp::splitTopics` | 0 调用 | 配置页局部解析器和 `joinTopicsForIni` 仍在 | 作用域独立的死函数 |
| 删除 `currentFloorString`、`extractCardId` | 均 0 调用 | `onCardPassed` 直接使用事件桥上游的卡号/楼层 | IC 上报链未变 |
| 删除 4 个 `OfflineQr` 旧辅助函数 | 均 0 调用 | `handleFrame` 直接完成分段、日期、MD5、payload 校验 | 当前入口未调用旧函数 |
| 删除 `pingHostOnce` | 0 调用 | 当前网络状态使用既有异步检查链 | DHCP 配置活动代码未改 |

### 6.2 参数调整

| 调整 | 完整调用点检查 | 行为 |
|---|---|---|
| `DiskUsageMonitor` 删除 `QObject *parent` | 仅 1 个创建点，已同步 | 参数原来只 `Q_UNUSED`；真实 QWidget 父对象仍是 `mainWindow` |
| `NetworkStatusMonitor` 删除 `QObject *parent` | 仅 1 个创建点，已同步 | 同上 |
| `IcToast::startPassEdgeFlash(int)` 改无参 | 2 个调用点均同步 | 原参数只 `Q_UNUSED`；动画仍由固定周期和次数决定 |

### 6.3 删除文件

| 文件 | 构建/引用复核 | 结论 |
|---|---|---|
| `components/multimedia/networkpage.cpp/.h` | 未进入构建；与 settings 下版本完全重复；现工程 0 引用 | 不影响当前网络设置页 |
| `components/multimedia/tplayer_decode.cpp/.h` | 未进入构建；`TPlayerWidget` 无外部引用 | 当前使用 `GstPlayerWidget` |
| `components/features/offline_dialog.h` | 只定义未创建的调试窗口；现工程 0 引用 | 不影响 feature 页面 |
| `modules/facegate/FaceGateQt/resources.qrc` | 与现存人脸 QRC 重复；独立工程已指向现存文件 | 不影响根工程 |

四组已删除路径在 `.cpp/.h/.pro/.qrc/.ui/.ini` 中的残留引用均为 0。

## 7. 头文件和符号完整性复核

为针对前次出现的头文件遗漏，本次进行了直接依赖扫描：

| 检查 | 缺失文件数 |
|---|---:|
| 使用 `qDebug/qWarning/qInfo/qCritical` 但未直接包含 `<QDebug>` | 0 |
| 使用 `std::shared_ptr/unique_ptr/make_*` 但未直接包含 `<memory>` | 0 |
| 使用 `std::move` 但未直接包含 `<utility>` | 0 |

因此，之前指出的 `volumepopup.cpp`、`ftppage.h`、FaceGate 相机文件的直接依赖问题在当前源码中均已补齐。

删除候选符号也重新按当前工程全文检索：所有确认删除的旧函数均为 0 命中；局部 `splitTopics` 当前为 1 个定义加 2 个调用，不能再删除。

## 8. 发现的现有注意项

以下不是本轮清理引入的问题，但手工联调时应重点确认：

1. `deletePlayFile` 当前只删除本地文件并记日志，没有构造 MQTT 业务回执；平台如果等待确认，需要协议双方明确。
2. 主工程依赖外部 mqttd、GStreamer 插件、RGA、Qt SQL 插件、串口设备和 U 盘部署路径；静态路径存在不代表目标板依赖齐全。
3. 当前活动配置是 `online_v2=true`、`protocol_mode=online`，因此 IC 在线网关会启用；更改 feature 时必须保证三种模式只启用一个。
4. `rs485_dir.c` 不进入 Qt 工程，需作为独立内核模块部署；仅编译 Qt 应用不会生成该设备驱动。

## 9. 建议的手工验证顺序

1. qmake/编译主工程，重点关注 MOC、直接头文件依赖、GStreamer/RGA 和 QSQLITE。
2. 本地视频与图片混播、下一项预加载、文件运行中删除。
3. LIVE 各协议、断网重试、无帧看门狗、直播/录播互斥。
4. FTP 下载成功、已存在、空间不足、网络失败和重复任务。
5. 音量本地调节与 MQTT 调节回执。
6. `getPlayFileList`、`getPlayInfo`、`deletePlayFile`、`videoControl`、`linuxReboot`。
7. IC 卡、信号板楼层、字母楼层、离线二维码、online_v1/online_v2 二维码和 RS485。
8. U 盘插拔、磁盘告警、网络告警、设置页 DHCP/静态地址。

## 10. 最终判断

基于当前源码与参考目录的逐文件差异、符号引用、构建清单、资源和直接依赖检查，最近的清理没有证据表明改变了 `qt_ycest.pro` 主工程的有效业务逻辑。必须保留的调用函数均在；确认删除的函数在清理前没有调用，且当前活动链有明确实现或根本未经过这些函数。

本报告没有以编译和运行结果作保证，最终仍需按目标板环境进行用户计划中的手工测试。
