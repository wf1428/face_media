# Qt 多媒体播放与人脸机双模态融合 Agent 实施文档

## 1. 文档目的

本文件用于指导代码 Agent 将以下两个现有 Qt 工程融合为一个可运行程序：

1. **多媒体播放工程**
2. **人脸机工程（FaceGateQt）**

融合后的程序必须在同一个 Qt 进程、同一个 `QApplication`、同一个 EGLFS 显示环境下运行，并通过“人体存在状态”切换两个业务模态。

核心原则：

> 只融合程序入口、模态切换控制、人体感应接口和 EGLFS 软件鼠标。  
> 两个工程原有业务逻辑、配置、资源、线程、数据库和功能模块不得相互混用。

---

## 2. 当前环境与已知条件

目标平台和运行环境按当前工程现状处理：

- 平台：RK3566
- 系统：Ubuntu 20.04
- Qt：Qt 5.12.8
- 显示平台：EGLFS / EGLFS KMS
- 屏幕分辨率：1024 × 768
- 多媒体播放：GStreamer 播放链路
- 人脸机：摄像头、人脸检测、人脸识别、活体检测、SQLite 管理界面等现有功能
- 当前多媒体工程已经存在 EGLFS 软件鼠标实现
- 当前人脸机工程仍主要按 XCB 环境运行，尚未统一到多媒体工程的软件鼠标体系

禁止退回 LinuxFB，也禁止在融合程序中继续依赖 XCB。

---

## 3. 最终运行行为

### 3.1 默认状态

程序启动后：

1. 只创建一个 `QApplication`
2. 只创建一个全屏主窗口
3. 使用 EGLFS 平台启动
4. 默认显示多媒体播放界面
5. 多媒体播放逻辑按照原工程正常运行
6. 全局只存在一套 EGLFS 软件鼠标

### 3.2 检测到人体

人体感应模块报告“有人”时：

1. 模态控制器收到 `presenceDetected` 事件
2. 多媒体模态进入非活动状态
3. 切换显示到人脸机界面
4. 启动或恢复人脸机需要的摄像头、检测和识别链路
5. 软件鼠标继续使用多媒体工程现有的 EGLFS 软件鼠标实例
6. 不允许人脸机重新创建系统鼠标、XCB 鼠标或第二套软件鼠标

### 3.3 人体离开

人体感应模块未来报告“无人”时，应支持切回多媒体模态。

本阶段人体感应硬件尚未接入，因此：

- 必须预留 `presenceLost` 接口和返回多媒体模态的完整代码路径
- 不得自行增加未经确认的自动返回时间
- 可以保留仅供开发测试调用的 `simulatePresenceLost()` 方法
- 当前验收重点是从多媒体模态切换到人脸机模态

### 3.4 当前临时触发方式

人体感应模块尚未到位时，使用以下临时模拟方式：

> 在多媒体播放界面中，鼠标右键连续点击 3 次，切换到人脸机 Demo。

建议默认判定规则：

- 监听鼠标右键释放事件
- 3 次点击必须在 2000 ms 内完成
- 超时后计数清零
- 只在“多媒体模态处于活动状态”时生效
- 已经处于人脸机模态时，不重复执行切换
- 切换过程中忽略新的切换请求
- 参数必须可配置，不得写死在业务代码中

该模拟触发器必须实现为人体感应接口的一种临时驱动，不得直接把切换代码写入多媒体业务类。

---

## 4. 强制架构要求

## 4.1 单进程、单 QApplication

融合后必须采用：

- 一个可执行程序
- 一个 `main()`
- 一个 `QApplication`
- 一个 EGLFS 全屏根窗口
- 一个统一的软件鼠标覆盖层

禁止采用：

- 同时启动两个独立 Qt 可执行程序
- 多媒体程序运行 EGLFS、人脸机程序运行 XCB
- 通过杀进程、拉起进程完成界面切换
- 创建两个 `QApplication`
- 创建两个全屏顶层窗口争抢 DRM/EGLFS
- 两个工程各自维护一套鼠标

原因：EGLFS、DRM、输入事件、全屏窗口和软件鼠标都应由统一外壳管理，两个独立进程无法可靠共享同一软件鼠标和同一显示上下文。

---

## 4.2 模块化外壳结构

建议新增一个融合外壳层：

```text
MergedQtApp/
├── app/
│   └── main.cpp
├── shell/
│   ├── AppShell.h
│   ├── AppShell.cpp
│   ├── ModeController.h
│   ├── ModeController.cpp
│   ├── IApplicationModule.h
│   ├── IPresenceSensor.h
│   ├── DebugMousePresenceSensor.h
│   └── DebugMousePresenceSensor.cpp
├── common/
│   └── cursor/
│       ├── SoftwareCursorManager.h
│       └── SoftwareCursorManager.cpp
├── modules/
│   ├── multimedia/
│   │   ├── MultimediaModuleAdapter.h
│   │   ├── MultimediaModuleAdapter.cpp
│   │   └── 原多媒体工程代码
│   └── facegate/
│       ├── FaceGateModuleAdapter.h
│       ├── FaceGateModuleAdapter.cpp
│       └── 原人脸机工程代码
└── config/
    └── app_shell.ini
```

目录名称可根据现有仓库调整，但模块边界必须保持一致。

---

## 4.3 两个业务模块不得直接依赖彼此

多媒体模块不得直接：

- 包含人脸机业务头文件
- 调用人脸识别、活体检测或数据库代码
- 读取人脸机配置
- 操作人脸机摄像头
- 管理人脸机线程

人脸机模块不得直接：

- 调用多媒体播放列表、GStreamer 播放控制或图片轮播逻辑
- 读取多媒体配置
- 操作多媒体播放器对象
- 复制或重新实现软件鼠标

两个模块只能通过融合外壳提供的标准生命周期接口被控制。

---

## 5. 模块生命周期接口

新增统一模块接口，建议形式如下：

```cpp
class IApplicationModule
{
public:
    virtual ~IApplicationModule() = default;

    virtual QWidget *rootWidget() = 0;

    virtual bool initialize() = 0;
    virtual void activate() = 0;
    virtual void deactivate() = 0;
    virtual void shutdown() = 0;

    virtual bool isActive() const = 0;
};
```

### 5.1 MultimediaModuleAdapter

该适配器只负责把原多媒体工程包装成统一模块，不改写其内部业务逻辑。

职责：

- 返回多媒体根界面
- 初始化原多媒体工程
- 激活时恢复原播放逻辑
- 失活时暂停或挂起前台播放
- 关闭程序时执行原有资源释放

不得：

- 将人脸机代码放入多媒体类
- 改写播放列表业务
- 改写图片、视频、楼层、箭头、配置文件和串口业务
- 删除原多媒体逻辑以迁就融合外壳

### 5.2 FaceGateModuleAdapter

该适配器只负责把原人脸机工程包装成统一模块。

职责：

- 返回人脸机根界面
- 初始化人脸机数据库、界面和必要服务
- 激活时启动或恢复摄像头、检测、识别和活体链路
- 失活时停止或挂起摄像头与识别任务
- 关闭程序时按原逻辑释放线程、摄像头、数据库等资源

不得：

- 改动识别阈值
- 改动活体判定逻辑
- 改动录入、查询、删除、打卡记录逻辑
- 改动数据库表结构
- 改动管理界面登录规则
- 改动摄像头处理算法
- 将多媒体逻辑复制到人脸机内部

---

## 6. 根窗口和界面容器

建议 `AppShell` 使用 `QStackedWidget` 或等价的单窗口堆叠容器：

```text
AppShell
├── ModuleStack
│   ├── MultimediaRootWidget
│   └── FaceGateRootWidget
└── SoftwareCursorOverlay
```

要求：

1. `AppShell` 是唯一顶层全屏窗口
2. 两个业务模块的根界面只能作为子控件加入 `AppShell`
3. 原工程中的 `showFullScreen()`、独立主窗口创建和独立 `main()` 必须移除或由适配器屏蔽
4. 软件鼠标覆盖层必须始终位于两个业务界面之上
5. 切换模态时只切换堆叠页面，不重建软件鼠标
6. 切换模态时不得创建第二个 EGLFS 窗口
7. 虚拟键盘、弹窗和管理界面必须仍处于统一根窗口体系内

---

## 7. EGLFS 软件鼠标统一方案

## 7.1 唯一鼠标实现

以多媒体播放工程现有 EGLFS 软件鼠标为唯一实现来源。

需要将其从多媒体业务窗口中抽离为外壳级公共组件，例如：

```cpp
class SoftwareCursorManager : public QObject
{
    Q_OBJECT

public:
    explicit SoftwareCursorManager(QWidget *overlayRoot, QObject *parent = nullptr);

    bool initialize();
    void setEnabled(bool enabled);
    void setVisible(bool visible);
    void raiseOverlay();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};
```

这里的“公共”仅表示外壳层公用，不代表两个业务工程可以互相调用。

## 7.2 鼠标约束

必须满足：

- 全程序只有一个软件鼠标实例
- 启动时隐藏 EGLFS 原生鼠标
- 多媒体界面和人脸机界面均由同一个软件鼠标显示
- 模态切换时软件鼠标位置保持
- 模态切换时软件鼠标不闪烁、不重建
- 弹出人脸机虚拟键盘后软件鼠标不能消失
- 弹出对话框后软件鼠标仍位于最上层
- 软件鼠标不得被视频层、摄像头画面或 OpenGL 控件覆盖
- 不允许人脸机继续执行 XCB 专用鼠标代码

## 7.3 人脸机 XCB 清理范围

只清理或隔离与平台、窗口和鼠标相关的 XCB 依赖，不改动人脸业务逻辑。

需要检查：

- `QX11Info`
- `XOpenDisplay`
- `XFixes`
- X11 Cursor API
- `DISPLAY` 环境变量依赖
- `QT_QPA_PLATFORM=xcb`
- XCB 专用窗口属性
- X11 全局坐标或屏幕抓取逻辑
- XCB 条件分支中的鼠标显示、隐藏和修复代码

若这些代码只用于桌面环境兼容，应使用平台条件编译或运行时分支隔离：

```cpp
if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
    // 原 XCB 兼容逻辑
}
```

融合后的 RK3566 主运行路径必须是 EGLFS，不得进入该分支。

---

## 8. 人体感应接口设计

## 8.1 统一抽象接口

必须新增独立接口，不得把 GPIO、串口或鼠标判断写入 `ModeController`：

```cpp
class IPresenceSensor : public QObject
{
    Q_OBJECT

public:
    explicit IPresenceSensor(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    ~IPresenceSensor() override = default;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

signals:
    void presenceDetected();
    void presenceLost();
    void sensorFault(const QString &message);
};
```

后续实际硬件只需要新增具体实现，例如：

- `GpioPresenceSensor`
- `SerialPresenceSensor`
- `Rs485PresenceSensor`
- `UsbPresenceSensor`

不得修改模态控制器和两个业务工程的核心代码。

## 8.2 当前调试实现

当前实现：

```cpp
class DebugMousePresenceSensor : public IPresenceSensor
{
    // 监听多媒体根界面右键三连击
};
```

其职责仅是把右键三连击转换为：

```cpp
emit presenceDetected();
```

不得直接调用：

```cpp
stackedWidget->setCurrentWidget(...);
faceGate->startCamera();
multimedia->stop();
```

所有切换必须统一交给 `ModeController`。

---

## 9. 模态控制器

## 9.1 状态定义

建议状态机：

```cpp
enum class AppModeState
{
    MultimediaActive,
    SwitchingToFaceGate,
    FaceGateActive,
    SwitchingToMultimedia,
    ShuttingDown
};
```

## 9.2 切换到人脸机

处理顺序建议：

```text
presenceDetected
    ↓
确认当前状态为 MultimediaActive
    ↓
状态改为 SwitchingToFaceGate
    ↓
禁止重复切换请求
    ↓
multimediaModule.deactivate()
    ↓
切换 QStackedWidget 当前页面
    ↓
faceGateModule.activate()
    ↓
软件鼠标覆盖层 raise
    ↓
状态改为 FaceGateActive
```

## 9.3 切换回多媒体

预留处理顺序：

```text
presenceLost
    ↓
确认当前状态为 FaceGateActive
    ↓
状态改为 SwitchingToMultimedia
    ↓
faceGateModule.deactivate()
    ↓
切换 QStackedWidget 当前页面
    ↓
multimediaModule.activate()
    ↓
软件鼠标覆盖层 raise
    ↓
状态改为 MultimediaActive
```

## 9.4 防抖和幂等

必须处理：

- 连续重复的 `presenceDetected`
- 连续重复的 `presenceLost`
- 切换过程中再次收到传感器事件
- 传感器抖动
- 模块激活失败
- 程序退出过程中收到切换事件

同一状态下重复事件不得重复启动摄像头、重复创建线程或重复初始化播放器。

---

## 10. 资源管理要求

## 10.1 多媒体模态失活

不得直接销毁整个多媒体模块，优先调用现有暂停接口。

需要保证：

- 当前播放进度是否保留，由原多媒体逻辑决定
- 不重复创建 GStreamer pipeline
- 不重复加载播放列表
- 不重复加载图片缓存
- 不重新初始化楼层、箭头、串口等业务
- 切回时能恢复原状态

如果现有播放器不支持安全暂停，可由适配器封装“停止并按原状态恢复”，但不得改变业务规则。

## 10.2 人脸机模态失活

需要停止高负载前台任务，避免隐藏后仍持续占用摄像头和 NPU。

至少检查：

- 摄像头采集线程
- 人脸检测线程
- 人脸识别线程
- 活体检测线程
- 图像刷新定时器
- 打卡记录触发
- 管理界面后台任务

数据库连接可以按原工程策略保留，但不能因反复切换重复创建无效连接。

## 10.3 资源所有权

建议所有权：

```text
AppShell
├── ModeController
├── IPresenceSensor
├── SoftwareCursorManager
├── MultimediaModuleAdapter
└── FaceGateModuleAdapter
```

两个模块自身继续管理各自内部对象。

禁止一个模块释放另一个模块创建的对象。

---

## 11. 配置文件

新增独立外壳配置文件，例如：

```ini
[application]
initialMode=multimedia

[display]
platform=eglfs
width=1024
height=768

[modeSwitch]
presenceDriver=debug_mouse
debugTripleRightClick=true
tripleClickCount=3
tripleClickWindowMs=2000

[cursor]
enabled=true
implementation=multimedia_eglfs_software
hideNativeCursor=true
```

要求：

- 原多媒体配置文件保持不变
- 原人脸机配置文件保持不变
- 外壳配置只管理公共外壳行为
- 不得把两套配置键合并到一个巨大配置文件
- 不得重命名两个工程已有配置项
- 后续人体感应模块只修改 `presenceDriver` 和对应驱动参数

---

## 12. 日志要求

新增统一日志前缀，方便排查：

```text
[SHELL]
[MODE]
[PRESENCE]
[CURSOR]
[MULTIMEDIA-ADAPTER]
[FACEGATE-ADAPTER]
```

关键日志至少包括：

- 当前 Qt 平台名称
- 当前屏幕和分辨率
- 软件鼠标初始化结果
- 人体感应驱动类型
- 右键三连击计数和超时重置
- 模态切换开始和完成
- 模块激活、失活结果
- 摄像头启动和停止结果
- 播放器暂停和恢复结果
- 重复事件被忽略
- 切换失败后的状态恢复

日志不得在每帧视频或每帧摄像头图像中高频刷屏。

---

## 13. Agent 禁止事项

Agent 不得执行以下修改：

1. 不得修改人脸识别阈值、活体阈值和底库匹配规则
2. 不得修改人脸录入、管理、查询、删除和打卡逻辑
3. 不得修改多媒体播放顺序、图片轮播规则和配置语义
4. 不得把两个工程的配置文件直接合并
5. 不得把两个工程的业务类互相 include
6. 不得使用 XCB 作为融合程序正式运行平台
7. 不得使用 LinuxFB
8. 不得启动两个独立 Qt 进程完成切换
9. 不得创建第二套软件鼠标
10. 不得让视频层或摄像头层覆盖软件鼠标
11. 不得为了融合而大规模重写两个工程
12. 不得修改与本次融合无关的 UI 风格
13. 不得删除原工程功能
14. 不得在没有依据的情况下增加自动返回时间
15. 不得一次性重构所有线程、播放器或数据库代码

---

## 14. 推荐实施顺序

## 第一阶段：建立基线

1. 分别确认两个原工程能够独立编译
2. 记录各自入口、根窗口和初始化流程
3. 记录多媒体软件鼠标相关类
4. 记录人脸机 XCB 相关代码位置
5. 记录两个工程启动和退出时创建的线程与硬件资源
6. 不修改业务代码

输出：

- 文件清单
- 入口调用链
- 模块边界说明
- XCB 依赖清单
- 软件鼠标依赖清单

## 第二阶段：创建融合外壳

1. 新建唯一 `main.cpp`
2. 新建 `AppShell`
3. 新建 `ModeController`
4. 新建 `IApplicationModule`
5. 创建两个模块适配器
6. 把两个根界面加入统一堆叠容器
7. 保持默认显示多媒体界面

本阶段不接入人脸摄像头切换，只先完成两个页面可被外壳切换。

## 第三阶段：统一软件鼠标

1. 从多媒体根界面抽离软件鼠标
2. 将鼠标挂到 `AppShell` 顶层覆盖层
3. 全局隐藏 EGLFS 原生鼠标
4. 移除人脸机正式运行路径中的 XCB 鼠标处理
5. 验证多媒体、人脸机、弹窗和虚拟键盘场景

## 第四阶段：接入人体感应抽象

1. 新建 `IPresenceSensor`
2. 新建 `DebugMousePresenceSensor`
3. 在多媒体活动状态监听右键三连击
4. 触发 `presenceDetected`
5. 由 `ModeController` 完成切换
6. 预留 `presenceLost`

## 第五阶段：接入生命周期

1. 多媒体适配器实现 `activate/deactivate`
2. 人脸机适配器实现 `activate/deactivate`
3. 处理摄像头、识别线程和播放器的暂停恢复
4. 防止重复初始化和重复释放
5. 增加错误恢复

## 第六阶段：稳定性验证

至少完成：

- 连续切换 100 次
- 软件鼠标持续移动测试
- 人脸机虚拟键盘测试
- 人脸机管理弹窗测试
- 多媒体视频播放测试
- 摄像头启动停止测试
- 程序退出资源释放测试
- EGLFS 无桌面启动测试

---

## 15. 验收标准

### 15.1 启动验收

- 程序在无 X11/XCB 桌面环境下启动
- `QGuiApplication::platformName()` 为 `eglfs`
- 屏幕正确显示 1024 × 768
- 默认进入多媒体模态
- 多媒体原有功能正常
- 软件鼠标正常显示

### 15.2 切换验收

- 多媒体界面右键连续点击 3 次后进入人脸机 Demo
- 3 次点击超时后不会误触发
- 切换只执行一次
- 切换过程中界面不出现第二个窗口
- 不启动第二个 Qt 进程
- 不出现 DRM、EGLFS 或窗口资源争抢
- 人脸机摄像头和识别逻辑正常启动

### 15.3 鼠标验收

- 两个模态使用同一个软件鼠标
- 不依赖 XCB 鼠标
- 切换前后鼠标位置连续
- 鼠标不闪烁
- 鼠标不被视频覆盖
- 鼠标不被摄像头画面覆盖
- 弹出虚拟键盘后鼠标不消失
- 弹出对话框后鼠标仍可见

### 15.4 业务隔离验收

- 多媒体配置和人脸机配置彼此独立
- 多媒体业务不调用人脸业务
- 人脸业务不调用多媒体业务
- 人脸识别和活体逻辑没有被修改
- 多媒体播放逻辑没有被修改
- 两个模块只通过外壳生命周期接口切换

### 15.5 稳定性验收

- 多次切换无崩溃
- 无明显内存持续增长
- 无线程重复创建
- 无摄像头占用未释放
- 无 GStreamer pipeline 重复创建
- 无数据库重复打开错误
- 程序退出时所有资源正常释放

---

## 16. Agent 输出要求

Agent 每一轮修改都必须输出：

1. 本轮修改目标
2. 修改文件清单
3. 每个文件的修改说明
4. 是否修改业务逻辑
5. 编译结果
6. 运行验证结果
7. 剩余风险
8. 可回退点
9. 对应 Git diff 或增量补丁

修改必须分阶段进行，不允许一次性把两个工程大规模混合。

推荐每轮都先执行：

```bash
git status
git diff --stat
```

生成补丁后执行：

```bash
patch -p1 --dry-run < xxx.patch
```

只有 dry-run 通过后，才视为补丁可交付。

---

## 17. 最终架构结论

本次融合不是把两个工程的代码和业务逻辑混成一个工程，而是建立一个统一运行外壳：

```text
统一 EGLFS 应用外壳
    ├── 唯一软件鼠标
    ├── 唯一模态控制器
    ├── 唯一人体感应接口
    ├── 多媒体业务模块
    └── 人脸机业务模块
```

允许共享的内容只有：

- `QApplication`
- EGLFS 主窗口
- 模态切换控制
- 人体感应抽象接口
- 多媒体工程现有 EGLFS 软件鼠标

除上述内容外，两个工程的业务逻辑、配置、资源、数据、线程和功能均保持隔离。
