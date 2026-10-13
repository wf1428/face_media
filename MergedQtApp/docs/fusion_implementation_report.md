# 融合实施报告

## 本轮目标

建立单进程、单 `QApplication`、单 EGLFS 根窗口的融合外壳，并接入生命周期、统一软件鼠标和左键长按 3 秒临时人体感应驱动。

## 主要修改

- `main.cpp`：使用唯一 `AppShell`；平台从外部环境读取，未设置时默认 EGLFS；接入虚拟键盘焦点过滤器。
- `shell/*`：新增应用外壳、模态控制器、模块接口、人体感应接口和调试鼠标驱动。
- `modules/multimedia/*Adapter*`：包装原多媒体窗口生命周期。
- `mainwindow.*`：移除业务窗口内软件鼠标所有权，增加播放激活/失活包装。
- `modules/facegate/*Adapter*`：包装 FaceGate 生命周期和配置加载。
- `modules/facegate/FaceGateQt/ui/MainWindow.*`：类名消冲突；摄像头/活体链路改为激活时启动、失活时停止；管理面板改为根窗口内子控件。
- `components/cursoroverlay/CursorOverlay.*`：保留原实现，增加外壳配置启停接口。
- `qt_ycest.pro`、`MergedQtApp.pro`：合并两套源文件与依赖，但不编译 FaceGate 独立入口。
- `config/app_shell.ini`：新增独立外壳配置。

## 业务逻辑影响

未修改人脸识别、活体判定、数据库结构、人员管理、打卡、多媒体播放顺序或图片轮播业务规则。调整仅限入口、窗口层级、生命周期和平台设置。

## 验证状态

- 已完成源文件存在性、工程清单、QRC XML、唯一光标实例、XCB 强制设置、FaceGate 类名冲突和顶层 `showFullScreen()` 静态检查。
- 已从两个未修改原工程重建融合前基线，生成 `docs/MergedQtApp_fusion.patch`，并通过 `git apply --check --binary` 校验。
- 当前 Windows 工作环境没有 Qt 5.12.8/qmake、RK3566 sysroot、GStreamer/librga、InspireFace 与 RKNN 目标库，因此不能在本机完成目标二进制编译和 EGLFS 硬件运行。
- 目标机仍需执行 `qmake MergedQtApp.pro && make -j4`，并完成摄像头、NPU、视频层、虚拟键盘及 100 次切换测试。

## 剩余风险

- FaceGate SDK 路径沿用原工程的绝对路径，目标构建环境不一致时需调整 `qt_ycest.pro`。
- 摄像头与 NPU 只能在 RK3566 实机验证。
- 原多媒体和 FaceGate 的业务弹窗虽都归属于统一根对象体系，仍需在 EGLFS 实机验证 Qt 5.12.8 的弹窗合成与软件光标层级。
- 本阶段没有人体感应硬件，`presenceLost` 仅提供开发调用路径。

## 回退点

两个原工程目录未被修改。删除融合交付目录即可完整回退；原多媒体与 FaceGateQt 仍可分别按原工程构建。
