# FaceGate 人脸机工程逻辑复核报告

- 复核日期：2026-08-06
- 当前工程：`F:\rk3566-proj\face_media\MergedQtApp\modules\facegate\FaceGateQt`
- 对照基线：`F:\rk3566-proj\face_media\MergedQtApp_V2_板坏后\MergedQtApp\modules\facegate\FaceGateQt`
- 复核边界：只读检查；未编译、未链接、未运行；未修改任何工程代码

## 1. 结论

人脸机部分与参考目录相比只有 8 个工程/源码文件存在差异：

| 文件 | 差异 | 对业务逻辑的影响 |
|---|---|---|
| `core/CameraCaptureBackend.cpp` | 增加 `<memory>` | 补齐智能指针直接依赖 |
| `core/CameraService.cpp` | 增加 `<memory>` | 补齐智能指针直接依赖 |
| `database/MysqlFaceRepository.cpp/.h` | 删除 `openLocalStore`、`isLocalFile` | 两函数在基线为 0 调用；当前入口仍是 `openSqliteStore` |
| `ui/MainWindow.cpp/.h` | 删除 `promptAdminPasswordChange` | 基线为 0 调用；管理面板信号直接连接 `handleAdminPasswordChange` |
| `FaceGateQt.pro` | `resources.qrc` 改为 `facegate_resources.qrc` | 前者是重复文件并已删除，后者物理存在且内容有效 |
| `resources.qrc` | 删除重复 QRC | 独立工程已切换到同内容的现存 QRC |

没有差异触及相机采集、推理线程、检测阈值、活体判断、身份匹配、重复通行抑制、开闸调用、日志写入、人员录入或管理员功能。因此，从源码差异看，最近清理没有改变人脸机原有活动逻辑。

## 2. 构建与融合方式

人脸机有两种使用方式：

1. 由根 `qt_ycest.pro` 编进融合程序，使用共享 `main.cpp` 和 `AppShell` 生命周期。
2. 由 `FaceGateQt.pro` 独立构建，使用子工程自己的 `main.cpp`。

独立工程检查结果：

| 检查项 | 引用/资源数 | 缺失数 |
|---|---:|---:|
| `FaceGateQt.pro` 源码、头文件和资源引用 | 54 | 0 |
| `facegate_resources.qrc` 物理资源 | 40 | 0 |

独立工程 QRC 的 40 项图标、QSS 和 QML 均存在。删除的旧 `resources.qrc` 在工程清单和代码中没有残留引用。

## 3. 模块组成与职责

| 部分 | 文件 | 功能与作用 |
|---|---|---|
| 配置 | `config/AppConfig.*`、`CameraProfile.h` | 加载相机、模型、阈值、活体、数据库、管理员、音频、日志和维护配置；生成相机活动 profile |
| 相机后端 | `core/CameraCaptureBackend.*` | Linux V4L2 相机枚举、打开、格式协商、缓冲区和帧读取；区分 USB/MIPI profile |
| 相机服务 | `core/CameraService.*` | 独立采集线程、最新帧邮箱、RGA/图像转换、错误和状态信号 |
| 人脸引擎 | `core/FaceEngine.*` | InspireFace 初始化、检测、质量评估、特征提取、相似度匹配、底库更新和重复人脸比较 |
| 推理工作线程 | `core/FaceInferenceWorker.*` | 持有 FaceEngine 与 VerificationController；处理验证帧、管理页帧和录入特征 |
| 验证状态机 | `core/VerificationController.*`、`VerificationTypes.h` | 多帧采集、最佳帧、活体批次、身份匹配、通过/失败结果和保持时间 |
| 活体 | `core/LivenessWorker.*` | RKNN 活体模型初始化、任务队列、真人/纸张/屏幕分数和结果信号 |
| 截图 | `core/SnapshotService.*` | 保存通过或按配置保存失败的验证截图，生成安全文件名 |
| 维护 | `core/MaintenanceWorker.*` | SQLite 备份、历史截图/日志清理和维护结果 |
| 数据库工作线程 | `database/DatabaseWorker.*` | 将仓储操作放入独立线程；图库、人员、通行记录、审计、系统事件、同步任务和存储统计 |
| 数据仓储 | `MysqlFaceRepository.*`、接口头 | QSQLITE 本地库或可移植 SQL 操作；表结构、人员、人脸特征、日志、软删除和重复检查 |
| 主界面 | `ui/MainWindow.*` | 模块生命周期、预览、验证结果、开闸、声音、数据库和管理面板总协调 |
| 管理面板 | `ui/AdminPanel.*` | 本地/网络人员、录入、日志、同步任务、维护、设备信息、阈值、网络、音频和密码设置 |
| 录入界面 | `ui/FaceEnrollWidget.*`、`EnrollPreviewWidget.*` | 手动/自动采集、预览、人脸质量/活体提示和人员信息输入 |
| 对话框 | `AdminLoginDialog`、`AppPasswordDialog`、`AppMessageDialog` | 管理员登录、修改密码、统一提示和虚拟键盘适配 |
| 预览 | `CameraPreviewWidget.*` | 图像、检测框、状态文字、结果颜色和布局绘制 |
| 音频 | `audio/AudioService.*` | 成功、失败、陌生人、活体失败和录入结果音频；音量、冷却时间、播放器进程 |
| 闸机抽象 | `gate/GateOutputService.*` | 验证通过后发出开门拉起/释放状态，保留硬件脉冲接口 |

## 4. 生命周期复核

### 4.1 初始化

`FaceGateMainWindow` 构造时先建立 UI，再启动长期后台服务：

- 注册跨线程元类型；
- 启动人脸推理线程；
- 配置音频；
- 启动数据库线程并加载图库；
- 启动维护线程；
- 建立相机、活体、推理、数据库、开闸和管理面板信号。

构造阶段不会直接把模块标记为活动，实际相机和识别由 `activateModule()` 控制。

### 4.2 激活

1. 阻塞调用推理线程的 `FaceInferenceWorker::activate`，初始化/恢复 FaceEngine。
2. 根据配置启动或确认活体线程就绪。
3. 标记模块活动，并根据当前 UI 是否需要相机调用 `updateCameraPowerState`。
4. 如果引擎、活体或相机失败，反向停用已启动资源并返回失败。

### 4.3 停用

切回多媒体时：

- 关闭管理面板；
- 停相机；
- 阻塞停用推理控制器；
- 清除待处理帧和预览；
- 停活体线程；
- 保留数据库、维护线程等可恢复服务。

### 4.4 关闭

关闭顺序为：幂等标记 → 停用前台 → 推理 worker shutdown → 退出推理线程 → 关闭数据库连接 → 退出维护线程和数据库线程。最近清理未修改这些函数。

## 5. 人脸验证状态机

主验证顺序明确为“检测/质量 → 活体 → 身份匹配”，非活体人员不会先被归类成陌生人。

| 状态 | 进入条件和作用 |
|---|---|
| `Idle` | 等待人脸；无人脸或结果保持结束后回到此状态 |
| `CollectingFrames` | 活体启用时累计 `collectFrameCount` 个可用人脸快照 |
| `SelectBestFrame` | 按人脸质量、检测置信度等选择最佳快照 |
| `LivenessChecking` | 逐张提交活体检测，累计通过次数 |
| `FeatureMatching` | 活体满足 `livenessPassRequired` 后提取特征并匹配图库 |
| `Passed` | 相似度达到阈值，发出 `verificationPassed` |
| `Failed` | 质量失败、活体失败、低于阈值或陌生人，发出 `verificationFailed` |
| `Cooldown` | 类型中保留；当前结果保持主要由 Passed/Failed 与 `resultHoldMs` 控制 |

具体判断：

- 人脸不可用、检测置信度不足或质量不足：`failed`。
- 活体通过数不足：`liveness_failed`。
- 图库匹配成功：`passed`。
- 有相似候选但未过正式阈值：`failed`，提示正对相机。
- 图库非空但没有有效相似候选：`stranger`。

验证通过后还会执行同一人员时间窗抑制；抑制期内不重复写日志和重复触发开门。

## 6. 通过、失败和录入链路

### 6.1 验证通过

1. 补齐人员名称、设备序列号、方向和事件类型。
2. 保存通过截图。
3. 播放成功音频。
4. 更新预览和状态文字；隐私模式不显示人员姓名。
5. 调用 `gate_.pulseOpen()`。
6. 记录最后通过人员用于重复抑制。
7. 异步写入验证日志。

### 6.2 验证失败

失败按 `stranger`、`liveness_failed`、普通 `failed` 选择语音；按配置决定是否保存失败截图，并异步写入失败原因和验证日志。

### 6.3 人脸录入

1. 管理页只处理检测和活体，不进入主验证/开闸链。
2. 活体启用时，录入必须使用有效且未过期的活体结果。
3. 截图保存到 `enrollments`。
4. 推理线程提取特征并与当前图库比较重复相似度。
5. 重复人脸拒绝并删除临时图片；非重复才进入数据库工作线程。
6. 数据库成功后重载图库，更新 FaceEngine。

## 7. 数据库和配置

当前 `AppConfig::load` 明确把 `databaseDriver` 设置为 `QSQLITE`，融合工程的数据库路径为 `/home/cat/face_media/access_control.db`。Repository 的行为是：

- `QSQLITE`：调用 `openSqliteStore`；
- `LOCAL/FGDB`：如果有 QSQLITE，强制转为真实 SQLite；没有 QSQLITE 则失败，不再创建独立 FGDB；
- 其他驱动：代码仍保留 MySQL 连接和建库能力，但当前配置加载路径不会选择它。

被删除的 `openLocalStore` 是旧 FGDB 文件入口，基线没有任何调用。旧格式迁移仍由 `prepareSqliteFile`、`loadLocalStoreFromFile`、`migrateLocalStoreToSqlite` 等活动函数完成，因此删除没有切断 SQLite 打开或旧库迁移入口。

主要数据包括人员、特征、验证日志、管理员审计、系统事件、同步任务和存储统计。数据库操作均投递到专用线程。

## 8. 管理员功能

管理面板支持：

- 人员查询、编号修改、启停、软删除、CSV 导入、重新录入；
- 网络人员查看和人脸图片同步状态；
- 通行日志筛选；
- 同步任务、系统事件和存储统计；
- SQLite 备份、截图/日志清理；
- 人脸、重复人脸、质量、检测、活体阈值设置；
- DHCP/静态网络配置；
- 音频开关、音量、提示文件和试听；
- 管理员密码修改、日期时间和设备信息。

删除的 `promptAdminPasswordChange` 没有调用。当前真实链路是：

`AdminPanel::adminPasswordChangeRequested` → `FaceGateMainWindow::handleAdminPasswordChange` → 校验旧密码 → 更新带盐哈希 → 保存 INI → 写管理员审计。

## 9. 与 MQTT/网络人员的关系

FaceGate 自身不直接连接 MQTT。融合程序通过 `FaceImageSyncBridge` 与主工程 `MqttManager` 交互：

- 管理面板请求网络人员列表；
- MqttManager 从 `NetworkPersonnelStore` 返回 JSON 列表；
- `face.responseImages` 写入统一数据库和 `network_faces` 图片目录；
- FaceGate 推理线程对已落盘同步图片做单人脸、检测置信度、人脸质量和特征提取校验，明确跳过活体检测；
- 同步结果和网络人员列表回到管理面板。

必须区分两套数据用途：

1. FaceEngine 的识别图库来自 FaceGate Repository 的本地人员/人脸特征表。
2. MQTT 同步的网络人员来自 `network_person`、`network_face` 等网络表；同步图片通过质量校验后，特征和模型版本写入 `network_face`，并继续显示在管理面板的“网络人员”视图。

同步图片现已复用 FaceEngine 的静态图片检测和特征提取能力，但没有把 `network_face` 记录加入 FaceEngine 的本地 gallery。因此“平台图片质量校验成功”仍不等同于“该人员已经进入闸机识别图库”；这是当前实现刻意保留的数据边界。

## 10. 最近清理的零调用复核

| 被删除符号 | 基线证据 | 当前替代/入口 | 复核结论 |
|---|---|---|---|
| `MysqlFaceRepository::openLocalStore` | 1 声明 + 1 定义，0 调用 | `open()` 调用 `openSqliteStore` | 删除安全 |
| `MysqlFaceRepository::isLocalFile` | 1 声明 + 1 定义，0 调用 | 当前分支使用 `localFileMode_`/`isSqlite` 和明确入口 | 删除安全 |
| `FaceGateMainWindow::promptAdminPasswordChange` | 1 声明 + 1 定义，0 调用 | 直接 signal → `handleAdminPasswordChange` | 删除安全 |

三者在当前工程全文检索均为 0 命中。相机、活体、验证和录入部分没有删除函数或参数。

## 11. 现有注意项

以下问题或边界在参考目录中同样存在，不是本轮清理引入：

1. `GateOutputService::pulseOpen` 当前只发出“拉起/释放”状态信号，没有实际 GPIO、继电器或串口写操作。人脸验证通过会调用它，但不会仅靠该类直接驱动硬件闸机。
2. 网络人员图片会进行质量校验和特征提取，但不会自动进入 FaceEngine 识别图库；若后续需要直接用于闸机识别，仍需明确“网络人员转本地图库”的业务规则。
3. 当前配置加载强制 QSQLITE；INI 中的 MySQL 参数仍可编辑/保存，但不会改变当前实际驱动。
4. 融合模式的人脸配置解析顺序为：`FACEGATE_CONFIG` 环境变量 → 可执行目录 `facegate.ini` → 可执行目录模块配置 → 当前目录 `facegate.ini` → 当前目录模块配置。根配置与模块配置内容不同，部署时应明确使用哪一个。
5. 静态检查无法验证目标板上的 V4L2 格式、RGA、InspireFace、RKNN 活体模型、QSQLITE 插件和音频设备。

## 12. 建议的手工验证顺序

1. 分别编译融合工程和独立 `FaceGateQt.pro`，确认 QRC、MOC、QSQLITE 和外部模型库。
2. MIPI/USB 相机 profile、帧格式、断开重连和模块切换后的再次启动。
3. 无人脸、低质量、多脸、活体失败、陌生人、相似但未过阈值、匹配成功。
4. 同一人员抑制时间、开门状态、通过/失败截图和日志。
5. 手动与自动录入、重复人脸、活体过期、数据库失败回滚。
6. 管理员登录、默认密码强制修改、密码持久化和审计日志。
7. 人员启停/删除、CSV 导入、备份清理、网络和音频配置。
8. MQTT 人员及图片同步后，分别核实“网络人员页面可见”和“识别图库是否按产品要求更新”。

## 13. 最终判断

最近清理对 FaceGate 的活动业务逻辑没有可见改变。相机、推理、活体、数据库、录入、管理员和生命周期函数与参考目录一致；三项删除均是基线 0 调用私有函数，QRC 修改是对重复资源文件的正确归一化。

需要重点关注的 GateOutputService 硬件占位和网络人员未进入识别图库，是现有架构边界，不能归因于本次代码整理。
