/**
 * @file AppShell.h
 * @brief 应用外壳的模块生命周期和活动页面接口。
 * @author Du lin
 * @note 最近修改：2026-10-24；修改人：Du lin；补充初始化时读取补光灯功能开关的配置来源、
 *       默认值及重启生效约束；接口签名及成员布局未更改。
 */
#ifndef APPSHELL_H
#define APPSHELL_H

#include <QWidget>

class FaceGateModuleAdapter;
class LedFillLight;
class ModeController;
class MultimediaModuleAdapter;
class QuickUiRuntime;
class QStackedWidget;
class QTimer;
class Sr505PresenceSensor;

class AppShell final : public QWidget
{
    Q_OBJECT

public:
    explicit AppShell(QWidget *parent = nullptr);
    ~AppShell() override;

    /**
     * @brief 初始化模块、人体感应和模式切换，并绑定补光灯状态控制。
     * @return 模块和感应适配器初始化成功或已初始化时返回 true；初始化失败返回 false。
     * @note 在 GUI 线程调用；首次初始化读取 Rk3566Platform::netConfigPath() 对应的
     *       net_cfg.ini 中 fillLight/enabled，缺省 true；false 禁止开灯，仍保留启动和退出关灯。
     *       本开关不影响摄像头、推理及原前台计时；修改配置后重启程序生效，不自动写回配置。
     */
    bool initialize();
    void shutdown();
    QWidget *multimediaRootWidget() const;
    QWidget *activeRootWidget() const;

private:
    QString shellConfigPath() const;
    QString faceGateConfigPath() const;

    QStackedWidget *stack_ = nullptr;
    MultimediaModuleAdapter *multimediaModule_ = nullptr;
    FaceGateModuleAdapter *faceGateModule_ = nullptr;
    ModeController *modeController_ = nullptr;
    QuickUiRuntime *quickUiRuntime_ = nullptr;
    Sr505PresenceSensor *presenceSensor_ = nullptr;
    LedFillLight *fillLight_ = nullptr;
    QTimer *presenceReconcileTimer_ = nullptr;
    bool initialized_ = false;
    bool shutdown_ = false;
};

#endif // APPSHELL_H
