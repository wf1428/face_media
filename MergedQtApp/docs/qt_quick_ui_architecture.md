# Qt Quick 界面迁移与模块管理约定

## 目标

界面采用 Qt Quick 与原生 OpenGL/QWidget 的分层混合架构，播放器、人脸算法、权限校验、数据库、485、MQTT 和
U 盘导入等业务逻辑继续保留在 C++。QML 只负责呈现、动画和用户操作转发。

所有 Quick 页面共用 `QuickUiRuntime` 持有的一个 `QQmlEngine`。整页 Quick 和
覆盖层由统一宿主装载；复杂 QWidget 页面允许使用受管理、长期存活的小型 Quick
导航区，但禁止业务页面自行创建 `QQmlEngine`、`QQuickView` 或新的全屏
`QQuickWidget`。

## 固定界面层与模块 ID

| 层 | 模块 ID | 当前呈现方式 | 迁移约束 |
| --- | --- | --- | --- |
| 程序最外层 | `application.shell` | Quick 主菜单 + QWidget 宿主 | 外壳只负责生命周期和模块路由，菜单点击转发给原 C++ 槽 |
| 多媒体主界面 | `multimedia.main` | 原生 OpenGL + 按需 Quick 覆盖层 | 视频区域不得覆盖常驻全屏 Quick FBO |
| 人脸识别界面 | `face.recognition` | 原生 OpenGL + 按需 Quick 覆盖层 | 摄像头、识别框与推理链路保持不变 |
| 人脸管理后台 | `face.administration` | Quick 导航 + QWidget 内容 | 表格、录入和导入导出逻辑保持 C++ |
| 系统设置 | `system.settings` | Quick 导航 + QWidget 内容 | 继续调用原网络、MQTT、FTP 保存逻辑 |
| 功能页面 | `feature.pages` | Qt Quick | 通过 C++ ViewModel 保留互斥和持久化语义 |
| 文件管理 | `file.manager` | Quick 操作栏 + QWidget 文件树 | 独立登记路由，文件操作逻辑保持不变 |
| 调试工具 | `debug.tools` | Quick 标题栏 + QWidget 内容 | 独立登记路由，设备命令和日志逻辑保持不变 |
| 弹窗与覆盖层 | `overlay.global` | Qt Quick | 消息框、Toast、人员导入进度仅显示期间参与合成；输入键盘保留原控件 |

## 呈现策略

- `NativeWidgetMode`：尚未迁移或必须保留的 QWidget 页面。
- `NativeOpenGlMode`：视频、摄像头等现有 OpenGL 页面。该模式下统一 Quick 宿主
  默认隐藏，不产生持续的全屏 FBO 合成。
- `ManagedQuickIslandMode`：复杂 QWidget 业务页中的 Quick 导航或动画区域。所有
  实例共享统一引擎、长期复用，并且只在非视频后台显示。
- `QuickOpaqueMode`：后台、设置等不透明整页。进入后统一 Quick 宿主显示，退出时
  隐藏；页面是否常驻由 `keepAlive` 决定。
- `QuickOverlayMode`：弹窗、Toast、加载层等短时覆盖内容，不作为整页路由。

## 生命周期

1. `QApplication` 创建后初始化唯一 `QuickUiRuntime`。
2. `AppShell` 注册所有界面模块并安装唯一 Quick 宿主。
3. 原 `ModeController` 继续执行播放器、摄像头和识别模块切换。
4. 稳定业务状态产生后，只把当前模块 ID 同步给 `QuickUiRuntime`。
5. Quick 页首次加载后按 `keepAlive` 选择长期复用或离开时释放。
6. 退出程序时先关闭覆盖层，再按原顺序停止业务模块。

## 新增 Quick 页面

1. 先在 `QuickUiRuntime::UiModule` 和 `moduleId()` 中登记模块，再为页面建立独立
   QML 文件并将资源加入 qrc；业务代码不得直接散写模块 ID 字符串。
2. C++ 中建立只暴露页面所需属性、信号和槽的 ViewModel；不得把数据库或设备对象
   直接暴露给 QML。
3. 向 `QuickUiRuntime` 注册对应模块枚举、所属层、`QuickOpaqueMode`、QML URL 和
   `keepAlive`。
4. 仍由原业务状态机决定何时切换，只调用 `synchronizeActiveModule()` 同步界面。
5. 验证进入、退出、反复切换、视频恢复、键盘、弹窗层级和 30 分钟持续运行。

## 性能与层级硬约束

- 多媒体和人脸视频继续走现有 GStreamer/RGA/OpenGL 数据链路。
- 视频常态运行时不显示全屏 Quick 宿主，避免额外 FBO 和持续 GPU 带宽消耗。
- 不允许每个内容页各建一个全屏 QQuickWidget；混合迁移只允许边缘导航等有限区域，
  并且必须共享统一引擎、长期复用。消息弹窗必须走统一覆盖层。
- 不允许通过频繁销毁 Quick 页面解决页面切换问题；轻量页使用 `keepAlive=true`。
- 覆盖层顺序固定为：页面、进度、Toast、模态遮罩与弹窗卡片；覆盖层关闭后恢复原焦点。
- 人员导入进度使用 Quick 模态层。可能长时间与视频并行的视频下载进度保留原
  QWidget 小卡片，避免全屏 Quick FBO 长时间参与视频合成。
- QML 不直接决定通行结果、模块启停或硬件下发，避免界面改造改变业务逻辑。

## 六层迁移完成边界

1. 程序最外层：主控制菜单已由长期复用的 Quick 页面呈现，原按钮槽继续处理业务。
2. 多媒体主界面：菜单、提示和弹窗已接入 Quick；持续播放区保留原生 OpenGL。
3. 人脸识别界面：全局提示和弹窗已接入 Quick；摄像头、识别框、结果保持原生 OpenGL。
4. 人脸管理后台：导航、动画、提示和人员导入进度已接入 Quick；表格和录入表单保留 QWidget。
5. 系统设置与功能页：设置导航和文件/调试页操作栏已接入 Quick，功能开关页已完整
   Quick 化；原保存、文件操作和设备命令逻辑不变。
6. 弹窗和覆盖层：项目消息框、确认框、Toast 和人员导入进度由唯一 Quick 宿主统一管理。

以上是本轮迁移的完成状态。视频纹理、复杂表格、输入表单和虚拟键盘不继续强制
QML 化；在 Qt 5.12/RK3568 上，这些边界用于避免额外全屏 FBO、纹理拷贝、上下文
抖动和不可控的透明层级，同时不改变原业务逻辑。后续新增界面仍按本文件的模块目录
和呈现策略接入，不再自行创建 QML 引擎。
