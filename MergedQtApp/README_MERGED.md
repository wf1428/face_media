# MergedQtApp

本目录是多媒体工程与 FaceGateQt 的单进程融合工程。正式运行结构为：

```text
AppShell（唯一 EGLFS 顶层窗口）
├── QStackedWidget
│   ├── 原多媒体 MainWindow
│   └── FaceGateMainWindow
├── ModeController
├── Sr505PresenceSensor（/dev/sr505 应用适配层）
├── LedFillLight（/dev/led 人脸识别补光灯模块）
└── CursorOverlay（全程序唯一实例）
```

## 关键行为

- 只有 `main.cpp` 被融合工程编译为程序入口，只创建一个 `QApplication`。
- 未设置平台变量时默认使用 `eglfs`；若外部已设置 `QT_QPA_PLATFORM`，入口尊重该值。
- 禁止 `linuxfb` 的约束仍保留。
- 默认显示多媒体模块。
- 仅在 `MultimediaDemo` 主界面中，SR505 高电平事件会切换到 FaceGate；启动时驱动提供的初始高电平也会立即生效。
- SR505 恢复低电平时，仅在主识别画面没有检测到人脸且没有结果保持时返回多媒体；识别过程中、密码输入和管理面板期间忽略下降沿。
- 最终识别成功或失败后保留结果 5 秒；保持期间忽略 SR505 下降沿。到期时高电平继续留在人脸识别，低电平返回多媒体。
- FaceGate 识别流程成功激活时向 `/dev/led` 写入 `1` 打开补光灯；最终通行成功时写入 `0` 关闭。若结果保持结束时人体仍为高电平并继续识别，则重新打开补光灯；识别失败且仍停留在人脸界面时不关灯。返回媒体播放界面、应用启动和退出时都会主动关闭补光灯。
- FaceGate 激活时启动人脸引擎、活体线程和摄像头；失活时停止摄像头和活体线程。数据库线程按原工程策略保留。
- FaceGate 的管理面板是 `AppShell` 中的子控件，不再创建第二个全屏窗口。
- FaceGate 独立入口不再设置 `QT_XCB_GL_INTEGRATION` 或强制 OpenGL 后端。

## 配置

- 外壳配置：`config/app_shell.ini`
- SR505 设备节点：`modeSwitch/sr505Device`，默认 `/dev/sr505`
- LED 补光设备节点：`fillLight/device`，默认 `/dev/led`
- FaceGate 配置：`facegate.ini`
- 多媒体原配置保持原路径与语义

可选外部覆盖：

```sh
export APP_SHELL_CONFIG=/path/to/app_shell.ini
export QT_QPA_PLATFORM=eglfs
```

## RK3566 构建

在已经配置 Qt 5.12.8、GStreamer、librga、InspireFace 和 RKNN sysroot 的目标机或交叉编译环境中：

```sh
qmake MergedQtApp.pro
make -j4
```

SDK 路径沿用 FaceGateQt 原工程，在 `qt_ycest.pro` 顶部可按实际交叉编译环境调整。

## 运行

将二进制放在本目录，确保 FaceGate 模型、数据库、音频和原多媒体静态资源仍使用各自原路径，然后执行：

```sh
chmod +x deploy/rk3566/run_merged_qt_app.sh
deploy/rk3566/run_merged_qt_app.sh
```

桌面调试可由外部选择平台，例如：

```sh
QT_QPA_PLATFORM=xcb ./MergedQtApp
```

入口不会为 FaceGate 强制设置 XCB。
