# 融合基线审计

## 原多媒体工程

- 入口：`main.cpp`
- 根界面：`MainWindow`
- 播放界面：`MultimediaDemo`
- 播放失活接口：`leaveAndStopVideo()`
- 播放恢复接口：`resumeLocalPlaylistFromStart()`
- 软件鼠标：`components/cursoroverlay/CursorOverlay`
- 平台：入口默认 EGLFS/KMS，已禁止 LinuxFB

## 原 FaceGateQt 工程

- 原入口：`modules/facegate/FaceGateQt/main.cpp`（不进入融合工程 SOURCES）
- 根界面：原 `MainWindow`，融合副本中仅为消除同名冲突改名为 `FaceGateMainWindow`
- 摄像头：`CameraService`
- 人脸引擎：`FaceEngine`
- 活体线程：`LivenessWorker`
- 数据库线程：`DatabaseWorker`
- 运维线程：`MaintenanceWorker`
- 管理界面：`AdminPanel`

## XCB 依赖清单

FaceGate 业务源码未发现 `QX11Info`、`XOpenDisplay`、`XFixes` 或 X11 Cursor API。唯一正式问题是原独立入口主动设置：

- `QT_XCB_GL_INTEGRATION=xcb`
- `QT_OPENGL=software`

融合副本已移除这两项设置，平台由外部环境或融合入口默认值选择。

## 模块边界

- 多媒体适配器只调用多媒体窗口的生命周期包装，不包含 FaceGate 头文件。
- FaceGate 适配器只调用 FaceGate 生命周期，不包含多媒体业务头文件。
- 两者只通过 `IApplicationModule` 受 `ModeController` 控制。
- `IPresenceSensor` 与 `DebugMousePresenceSensor` 位于外壳层，不直接操作页面、摄像头或播放器。
- `CursorOverlay` 只由 `AppShell` 创建一次。

## 未修改业务规则

未修改人脸识别阈值、活体阈值、录入/查询/删除/打卡规则、数据库表结构、多媒体播放列表规则、图片轮播规则及两套原配置键。
