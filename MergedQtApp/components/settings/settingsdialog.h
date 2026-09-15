/**
 * @file settingsdialog.h
 * @brief 网络、蓝牙、MQTT、系统、显示、待机和 FTP 页面容器及信号中转器。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QLabel>
#include <QWidget>

#include "components/multimedia/gst_player_widget.h"
#include "components/features/featuresdialog.h"
#include "auto_connect_manager.h"
#include "platform/rk3566_platform.h"


/** 设置对话框各子页面类型。 */
class NetworkPage;
class BluetoothPage;
class MqttPage;
class SystemInfoPage;
class StandbyLockPage;
class FtpPage;
class DisplayRotationPage;
class QBoxLayout;
class QEvent;
class QGridLayout;
class QResizeEvent;
class QShowEvent;


/** @brief 网络、蓝牙、MQTT、系统、显示、待机和 FTP 页面容器及信号中转器。 */
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    /** @brief 创建导航按钮、页面栈和自动连接管理器。 */
    explicit SettingsDialog(QWidget *parent = nullptr);

    /** @brief 释放设置子页面和 UI 资源。 */
    ~SettingsDialog();

    /** @brief 将功能开关同步给网络和 MQTT 页面。 */
    void setFeatureSettings(const FeatureSettings &settings);

    /** @brief 立即执行保存配置对应的网络自动连接。 */
    void startNetworkAutoConnectNow();

protected:
    /** @brief 宿主尺寸变化时，按配置优先的参考分辨率重排导航。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

    /** @brief 显示时将窗口几何限制到宿主实际可用区域。 */
    void showEvent(QShowEvent *event) override;

    /** @brief 窗口尺寸变化时立即重排导航。 */
    void resizeEvent(QResizeEvent *event) override;

private slots:
    /** @brief 切换到网络页面。 */
    void onNetworkButtonClicked();

    /** @brief 切换到蓝牙页面。 */
    void onBluetoothButtonClicked();

    /** @brief 切换到 MQTT 页面。 */
    void onMqttButtonClicked();

    /** @brief 切换到系统信息页面。 */
    void onSystemInfoButtonClicked();

    /** @brief 切换到待机锁屏页面。 */
    void onStandbyLockButtonClicked();

    /** @brief 切换到 FTP 页面。 */
    void onFtpButtonClicked();

    /** @brief 切换到显示旋转页面。 */
    void onDisplayButtonClicked();

private:
    /** @brief 创建导航栏和内容页面栈。 */
    void setupUI();

    /** @brief 建立各子页面与多媒体主窗口之间的转发连接。 */
    void setupConnections();

    /** @brief 按当前宽高重排设置导航。 */
    void applyResponsiveLayout();

    /** @brief 创建带滚动兜底的普通设置页容器。 */
    QWidget *wrapScrollablePage(QWidget *page);

    /** @brief 将窗口限制到当前宿主的实际区域。 */
    void fitToHost();

    QPushButton *networkButton = nullptr;       /**< 网络连接按钮。 */
    QPushButton *bluetoothButton = nullptr;     /**< 蓝牙连接按钮。 */
    QPushButton *mqttButton = nullptr;          /**< MQTT 连接按钮。 */
    QPushButton *systemInfoButton = nullptr;    /**< 系统信息按钮。 */
    QPushButton *standbyLockButton = nullptr;   /**< 待机和锁屏按钮。 */
    QPushButton *ftpButton = nullptr;           /**< FTP 设置按钮。 */
    QPushButton *displayButton = nullptr;       /**< 显示方向设置按钮。 */

    QBoxLayout *mainLayout_ = nullptr;          /**< 可切换方向的设置窗口根布局。 */
    QGridLayout *navigationLayout_ = nullptr;   /**< 横屏单列/窄屏网格导航。 */
    QWidget *sidebarWidget_ = nullptr;          /**< 导航容器。 */
    bool topNavigation_ = false;                /**< 当前是否使用顶部导航。 */

    QStackedWidget *contentStack; /**< 设置子页面内容栈。 */

    // 各个子页面
    NetworkPage *networkPage = nullptr;
    BluetoothPage *bluetoothPage = nullptr;
    MqttPage *mqttPage = nullptr;
    SystemInfoPage *systemInfoPage = nullptr;
    StandbyLockPage *standbyLockPage = nullptr;
    FtpPage *ftpPage = nullptr;
    DisplayRotationPage *displayPage = nullptr;
    QWidget *networkContainer_ = nullptr;
    QWidget *bluetoothContainer_ = nullptr;
    QWidget *mqttContainer_ = nullptr;
    QWidget *ftpContainer_ = nullptr;
    QWidget *standbyLockContainer_ = nullptr;
    QWidget *systemInfoContainer_ = nullptr;
    QWidget *displayContainer_ = nullptr;

    // 自动连接管理
    AutoConnectManager *autoConnectMgr = nullptr;

    QString configDir_ = Rk3566Platform::netConfigDir();
    QString configFileName_ = "net_cfg.ini";


public slots:
    /** @brief 把当前录播文件转交 MQTT 页面。 */
    void onCurrentRecordedFileChanged(const QString &fullPath);
    /** @brief 把当前播放截图转交 MQTT 页面。 */
    void onCurrentPlayImageChanged(const QString &base64Image);
    /** @brief 把当前播放器音量转交 MQTT 页面。 */
    void onCurrentPlayVolumeChanged(int volume);
    /** @brief 上报本地手动音量变化。 */
    void onLocalManualVolumeChanged(int volume, const QString &reason);


signals:
    /** @brief 手动请求多媒体页播放指定地址。 */
    void requestOpenApp3Player(const QString& url, const QString& kind);
    /** @brief 自动连接完成后请求多媒体页播放指定地址。 */
    void requestAutoOpenApp3Player(const QString& url, const QString& kind);

    /** @brief 请求停止多媒体页直播。 */
    void requestStopApp3Live();
    /** @brief 请求停止多媒体页全部播放。 */
    void requestStopApp3Playback();

    /** @brief 服务端音量命令已到达。 */
    void mqttVolumeChanged(int volume);

    /** @brief 新视频下载完成，可加入播放列表。 */
    void downloadedVideoReady(const QString& filePath);

    /** @brief FTP 下载开始。 */
    void ftpDownloadStarted(const QString& fileName);
    /** @brief FTP 下载完成。 */
    void ftpDownloadFinished(const QString& filePath);
    /** @brief FTP 下载失败。 */
    void ftpDownloadFailed(const QString& fileName, const QString& reason);
    /** @brief FTP 下载进度更新。 */
    void ftpDownloadProgress(const QString& fileName, int percent);

    /** @brief 请求媒体页抓取当前画面。 */
    void requestCapturePlayImage();

    /** @brief 请求恢复多媒体页录播轮播。 */
    void requestResumeApp3RecordedPlayback();

    /** @brief MQTT 连接状态标志变化。 */
    void mqttConnMarkChanged(int mark);
    /** @brief 请求显示视频控制提示。 */
    void videoControlNotice(const QString &text);

};

#endif // SETTINGSDIALOG_H
