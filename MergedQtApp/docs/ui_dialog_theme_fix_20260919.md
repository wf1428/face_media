# 两套后台配色及共用弹窗修正（2026-09-19）

## 范围

多媒体后台和人脸后台使用同一个 Quick 弹窗宿主，本轮同时处理两个后台的
信息、确认、选择弹窗。业务操作、返回值映射、页面切换、横竖屏导航尺寸规则、
播放器、人脸校验、人员导入及设备通信不作调整。

## 修正内容

- 多媒体菜单、系统设置导航、功能页、文件和调试操作栏恢复浅色背景、深色文字，
  配色取自原多媒体 QWidget 界面，并直接保存在各自 QML 页面内，避免 Qt 5.12
  Quick Compiler 对新增共享 QML 类型产生缓存链接缺失。
- 人脸导航通过实例属性明确启用原深色配色。弹窗和短时提示由调用入口明确
  指定所属后台，避免根据当前路由猜测主题而发生串色。
- 共用弹窗根节点移除 `anchors.fill: parent`，加载完成、宿主尺寸改变和每次
  弹出之前，均由 C++ 将宿主实际尺寸同步给 QML 根节点。
- 卡片内容使用单向尺寸计算；按实际按钮换行高度预留操作区，长文本仅滚动正文。
- 弹窗关闭时立即移除模态遮罩；宿主隐藏、销毁或 QML 加载失败时退出等待循环。
  快速关闭发生在事件循环启动之前时，也不会再次进入等待。
- 原生模态窗口屏蔽外壳输入时使用现有 QWidget 弹窗回退；Esc 与关闭按钮采用
  相同取消路径，确认和取消对应的业务行为不变。

## 验证与限制

使用临时安装的 PyQt5 / Qt 5.15.2，以离屏软件渲染方式加载工程实际 QML，
通过仅模拟界面属性和回调的桥接对象执行测试：

- 1024×768、768×1024、600×600、1280×720 四种尺寸。
- 多媒体、人脸两套弹窗配色和同一导航实例的主题切换。
- 短消息、150 行长结果、多个选项按钮，检查卡片居中且按钮在可见范围。
- 实际发送鼠标点击验证确认、指定选项、取消、关闭的回调。
- 弹窗覆盖进度和 Toast 时仍可点击。
- 全部 Quick 页面加载，没有非图片资源缺失类 QML 警告。

桌面 Qt 5.15.2 未复现旧根节点锚点在设备上的显示异常，因此上述尺寸修改属于
针对该路径的修正和防护，不能以桌面测试认定设备故障根因已完全复现。
此测试不执行 C++ 业务，不代替 RK3568 / Qt 5.12 的整机编译、EGL/Wayland 合成
和真实触控验证。测试依赖仅安装在电脑临时目录，不进入设备部署依赖。

## 本轮全部文件（16 个）

1. `shell/QuickUiRuntime.h`
2. `shell/QuickUiRuntime.cpp`
3. `shell/qml/QuickSceneHost.qml`
4. `shell/qml/QuickNavigationRail.qml`
5. `shell/qml/QuickMainMenu.qml`
6. `shell/qml/QuickFeaturesPanel.qml`
7. `shell/qml/QuickPageHeader.qml`
8. `components/quick/QuickNavigationWidget.h`
9. `components/quick/QuickNavigationWidget.cpp`
10. `components/quick/QuickMainMenuWidget.cpp`
11. `components/quick/QuickPageHeaderWidget.cpp`
12. `components/features/featuresdialog.cpp`
13. `modules/facegate/FaceGateQt/ui/AdminPanel.cpp`
14. `modules/facegate/FaceGateQt/ui/AppMessageDialog.cpp`
15. `quick_ui_resources.qrc`
16. `docs/ui_dialog_theme_fix_20260919.md`（本文，新增）
