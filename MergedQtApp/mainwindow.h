/**
 * @file mainwindow.h
 * @brief 管理融合应用页面、功能模块生命周期和跨模块信号路由。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

// Qt 核心头文件
#include <QMainWindow>      // 主窗口基类
#include <QStackedWidget>    // 堆栈窗口组件，用于页面切换
#include <QResizeEvent>      // 窗口大小改变事件
#include <QApplication>      // 应用程序类
#include <QMessageLogContext> // 消息日志上下文
#include <QWidget>           // 基础窗口组件
#include <QMouseEvent>       // 鼠标事件
#include <QByteArray>        // 字节数组
#include <QString>           // 字符串类
#include <QtGlobal>          // Qt 全局定义
#include <QKeyEvent>         // 键盘事件
#include <QProcess>

// 自定义组件头文件
#include "components/settings/settingsdialog.h"         // 设置对话框
#include "components/features/featuresdialog.h"         // 功能演示对话框
#include "components/features/command_dialog.h"         // 调试对话框
#include "components/features/key_service.h"            // 按键业务
#include "components/multimedia/multimediademo.h"       // 多媒体演示组件
#include "components/udisk_status/udisk_status.h"       // U盘状态显示
#include "components/file_manager/file_manager_page.h"  // 文件管理页面
#include "components/settings/ftppage.h"
#include "ic_board/ic_offline.h"
#include "ic_board/ic_offline_checker.h"
#include "ic_board/device_config_sync.h"
#include "ic_board/ic_offline_qr.h"
#include "ic_board/serial_init.h"



QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

/**
 * @class MainWindow
 * @brief 应用顶层页面容器和功能模块协调器。
 *
 * 创建多媒体、设置、IC、文件管理等页面，依据功能配置切换在线/离线协议模块，
 * 并集中转发下载、播放、音量、状态提示和返回菜单信号。
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    /** @brief 创建全部页面、运行模块并建立跨页面信号连接。 */
    MainWindow(QWidget *parent = nullptr);
    
    /** @brief 停止活动模块并释放 Qt Designer UI。 */
    ~MainWindow();

    /** @brief 激活多媒体模块，并恢复模块切换前的 LIVE 或本地轮播状态。 */
    void activateModule();

    /** @brief 停用多媒体模块并暂停播放，同时保留返回时需要恢复的状态。 */
    void deactivateModule();

    /** @return 多媒体模块当前处于活动状态时返回 true。 */
    bool isModuleActive() const;

    /** @return true only while the MultimediaDemo main page is unobstructed. */
    bool allowsPresenceSwitch() const;

protected:
    /** @brief 将设备键盘快捷键转换为页面或调试入口操作。 */
    void keyPressEvent(QKeyEvent *event) override;
    
    /** @brief 窗口尺寸变化后同步页面栈和 U 盘覆盖层几何。 */
    void resizeEvent(QResizeEvent *event) override;

private:
    Ui::MainWindow *ui;                             /**< Qt Designer 生成的界面对象。 */
    QStackedWidget *stack = nullptr;                /**< 菜单、媒体和文件页面的容器。 */
    QWidget *menuPage = nullptr;                    /**< 从原 centralWidget 接管的菜单页。 */
    QWidget *app3Page = nullptr;                    /**< 多媒体播放页面。 */
    UdiskStatusOverlay *udiskOverlay = nullptr;     /**< U 盘及业务消息覆盖层。 */
    FileManagerPage *filePage = nullptr;            /**< 可移动存储文件管理页面。 */

    CommandDialog *cmdDialog = nullptr;             /**< 串口/协议调试工具窗口。 */
    KeyService *key = nullptr;                      /**< 设备物理按键服务。 */

    SettingsDialog *settingsDialog = nullptr;       /**< 网络、媒体等综合设置窗口。 */
    bool moduleActive_ = false;                     /**< 外壳记录的多媒体模块活动状态。 */

    /** @brief 归一化并保存 [feature] 开关，同时同步 MQTT 协议模式。 */
    void saveFeatureSettings(const FeatureSettings &settings);

    /** @return 从平台配置读取并归一化后的功能开关。 */
    FeatureSettings loadFeatureSettings() const;

private slots:
    /**
     * @brief 延迟设置U盘覆盖层
     * 
     * 延迟创建覆盖层，确保主窗口和页面栈已经完成初始布局。
     */
    void setupUdiskOverlayLater();

};

#endif // MAINWINDOW_H
