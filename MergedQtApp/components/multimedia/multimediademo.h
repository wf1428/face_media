/**
 * @file multimediademo.h
 * @brief 协调信号板显示、本地轮播、直播、音量、截图和 MQTT 状态。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef MULTIMEDIADEMO_H
#define MULTIMEDIADEMO_H

#include <QDialog>
#include <QDebug>
#include <QLabel>
#include <QTimer>
#include <QHash>
#include <QImage>
#include <QSettings>
#include <QtGui>
#include <QDir>
#include <QList>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScreen>
#include <QGuiApplication>
#include <QPixmap>
#include <QDateTime>
#include <QElapsedTimer>
#include <QThread>
#include <QAtomicInt>
#include <QFontMetrics>

#include "components/cursoroverlay/keyboard_dialog.h"
#include "common/clients/signalboard_client.h"
#include "components/settings/mqtt/mqttmanager.h"


#include "disk_usage_monitor.h"
#include "network_status_monitor.h"
#include "snapshot_processor.h"
#include "gst_player_widget.h"
#include "netinfo_popup.h"
#include "vol_ctrl.h"
#include "media_toast.h"
#include "modules/facegate/FaceGateQt/core/VerificationTypes.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/rtc.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include "platform/rk3566_platform.h"

// 日志输出开关定义 - 可以通过注释/取消注释来控制不同类别的日志输出
#define ENABLE_LOG_INITIALIZATION      1   // 初始化相关日志
#define ENABLE_LOG_CONFIGURATION       0   // 配置相关日志
#define ENABLE_LOG_VIDEO_PLAYBACK      1   // 视频播放相关日志
#define ENABLE_LOG_IMAGE_DISPLAY       0   // 图片显示相关日志
#define ENABLE_LOG_AIR_CONDITIONER     0   // 空调控制相关日志
#define ENABLE_LOG_BUFFERING           0   // 缓冲相关日志
#define ENABLE_LOG_QUALITY             0   // 视频质量相关日志
#define ENABLE_LOG_FILE_OPERATIONS     0   // 文件操作相关日志
#define ENABLE_LOG_USER_INTERFACE      0   // 用户界面相关日志
#define ENABLE_LOG_SYSTEM_STATUS       0   // 系统状态相关日志
#define ENABLE_LOG_DEBUG               1   // 调试相关日志

// 日志宏定义，方便使用
#define LOG_INIT(qmsg)    do { if (ENABLE_LOG_INITIALIZATION) qDebug() << "[初始化]" << qmsg; } while (0)
#define LOG_CONFIG(qmsg)  do { if (ENABLE_LOG_CONFIGURATION) qDebug() << "[配置]" << qmsg; } while (0)       /* 配置加载与保存日志。 */
#define LOG_VIDEO(qmsg)   do { if (ENABLE_LOG_VIDEO_PLAYBACK) qDebug() << "[视频播放]" << qmsg; } while (0) /* 播放器状态与切换日志。 */
#define LOG_IMAGE(qmsg)   do { if (ENABLE_LOG_IMAGE_DISPLAY) qDebug() << "[图片显示]" << qmsg; } while (0) /* 图片轮播日志。 */
#define LOG_AC(qmsg)      do { if (ENABLE_LOG_AIR_CONDITIONER) qDebug() << "[空调控制]" << qmsg; } while (0) /* 空调状态显示日志。 */
#define LOG_BUFFER(qmsg)  do { if (ENABLE_LOG_BUFFERING) qDebug() << "[缓冲]" << qmsg; } while (0)          /* 媒体缓冲状态日志。 */
#define LOG_QUALITY(qmsg) do { if (ENABLE_LOG_QUALITY) qDebug() << "[视频质量]" << qmsg; } while (0)        /* 质量等级调整日志。 */
#define LOG_FILE(qmsg)    do { if (ENABLE_LOG_FILE_OPERATIONS) qDebug() << "[文件操作]" << qmsg; } while (0) /* 媒体文件扫描日志。 */
#define LOG_UI(qmsg)      do { if (ENABLE_LOG_USER_INTERFACE) qDebug() << "[用户界面]" << qmsg; } while (0) /* 页面交互和布局日志。 */
#define LOG_SYSTEM(qmsg)  do { if (ENABLE_LOG_SYSTEM_STATUS) qDebug() << "[系统状态]" << qmsg; } while (0) /* 系统状态监测日志。 */
#define LOG_DEBUG(qmsg)   do { if (ENABLE_LOG_DEBUG) qDebug() << "[调试]" << qmsg; } while (0)              /* 其他调试诊断日志。 */



/** @brief 信号板上报的轿厢运行方向。 */
enum DirectionStatus {
    UP, /**< 上行。 */
    DN, /**< 下行。 */
    LE  /**< 无方向或停层。 */
};

/** @brief 信号板上报的电梯运行状态。 */
enum StateStatus {
    NORMAL,   /**< 正常运行。 */
    FIRE,     /**< 消防状态。 */
    FULL,     /**< 满载。 */
    Overload, /**< 超载。 */
    REPAIR    /**< 检修。 */
};

namespace Ui {
class MultimediaDemo;
}

class CameraPreviewWidget;
class VolumePanel;
class QMovie;

/**
 * @brief 电梯多媒体主页面，协调信号板 UI、本地轮播、直播、音量和 MQTT 状态。
 *
 * 本类同时维护“期望播放模式”和“实际播放器状态”：直播首帧 watchdog、重试播放器
 * 与本地切片保护窗口共同避免网络断流或旧异步回调破坏本地轮播。截图编码放在独立
 * 工作线程，UI 合成仍在主线程完成。
 */
class MultimediaDemo : public QDialog
{
    Q_OBJECT

public:
    /** @brief 创建信号板、多媒体、MQTT、截图和告警子系统。 */
    explicit MultimediaDemo(QWidget *parent = nullptr);
    /** @brief 停止播放器和工作线程后释放页面资源。 */
    ~MultimediaDemo();

    /** @brief 保存直播地址和协议类型（如 rtsp、hls），但不立即切换。 */
    void setStreamUrl(const QString& url, const QString& kind);

    /** @brief 使用独立播放器预探测直播首帧，成功后再切换主播放器。 */
    void probeLiveStream(const QString& url);
    /** @brief 停止本地轮播并让主播放器进入指定直播源。 */
    void switchToLiveStream(const QString& url, const QString& kind);

    /** @brief 把新视频插入当前项之后，使其成为下一播放项。 */
    void insertVideoNext(const QString& filePath);

    /** @brief 下载完成后更新列表并按当前模式决定是否立即播放。 */
    void onDownloadedVideoReady(const QString& filePath);

    /** @brief 清除“期望直播”状态并停止后台探测和重试。 */
    void stopDesiredLive();

    /** @brief 合成 UI 与当前视频帧后异步编码为 Base64。 */
    void captureCompositeSnapshot();
    /** @brief 向 MQTT 管理器上报当前录播文件或清除状态。 */
    void notifyCurrentRecordedFile();

    /** @return false while password/time-setting UI is handling input. */
    bool allowsPresenceSwitch() const;

    /** @brief 为本地 Web 读取实际播放器和音量状态。 */
    QJsonObject localWebPlaybackState() const;

    /** @brief 通过既有入口应用本地播放设置，不经过 MQTT handler。 */
    void applyLocalWebPlayback(const QJsonObject &request);

public slots:
    /** @brief 启动页结束后放行首次媒体播放；重复调用保持幂等。 */
    void startPlaybackAfterSplash();
    /** @brief 模块临时切往人脸识别时停止播放器，但保留当前 LIVE 业务目标。 */
    void suspendPlaybackForModuleSwitch();
    /** @brief 模块重新显示时恢复暂停前的 LIVE 或本地轮播状态。 */
    void resumePlaybackAfterModuleSwitch();
    /** @brief 离开页面时停止全部播放器、探测和定时任务，避免后台占用解码资源。 */
    void leaveAndStopVideo();
    /** @brief 从列表首项恢复本地轮播。 */
    void resumeLocalPlaylistFromStart();

    /** @brief 保持视频播放，在 pictureRect 内以摄像头预览替换 PIC 轮播。 */
    void beginEmbeddedFaceRecognition();
    /** @brief 关闭 pictureRect 内的人脸预览并恢复 PIC 轮播。 */
    void endEmbeddedFaceRecognition();
    /** @brief 更新内嵌人脸预览帧。 */
    void setEmbeddedFaceFrame(const QImage &image);
    /** @brief 更新内嵌人脸框。 */
    void setEmbeddedFaceFaces(const QVector<DetectedFace> &faces);
    /** @brief 更新内嵌识别状态。 */
    void setEmbeddedFaceState(VerifyState state, const QString &message);
    /** @brief 在配置区域显示一名通过人员图片。 */
    void showRecognitionImage(const QImage &image, const QString &name, int durationMs);
    /** @brief 显示摄像头不可用提示。 */
    void setEmbeddedFaceUnavailable(const QString &message);
    /** @brief 通过页面既有覆盖层显示短时业务提示。 */
    void showTransientStatus(const QString &message, int durationMs = 3000);

    /** @brief 应用 MQTT 下发音量并回报执行结果。 */
    void onMqttVolumeChanged(int volume);

    /** @brief 切换到录播模式；已在本地轮播时仅同步业务状态。 */
    void switchToRecordedMode();
    /** @brief 停止直播、本地轮播及预加载播放器。 */
    void stopAllPlayback();

    /** @brief 根据 MQTT 连接标志刷新网络信息显示。 */
    void onMqttConnMarkChanged(int mark);

    /** @brief 下载开始时暂停本地播放并显示进度提示。 */
    void onDownloadStarted(const QString& fileName);
    /** @brief 更新当前下载文件的进度提示。 */
    void onDownloadProgress(const QString& fileName, int percent);
    /** @brief 下载成功后隐藏提示、更新媒体列表并恢复播放。 */
    void onDownloadFinished(const QString& filePath);
    /** @brief 下载失败后显示原因并恢复被暂停的本地播放。 */
    void onDownloadFailed(const QString& fileName, const QString& reason);

private slots:
    /** @brief 更新信号板演示数据或界面轮询状态。 */
    void onTimeout();
    /** @brief 刷新日期、星期和时间文本。 */
    void onTimeoutDate();

    /** @brief 收到播放器视频帧后与 UI 图像一起提交后台编码。 */
    void onVideoSnapshotReady(const QImage& image);
    /** @brief 结束失败的截图请求并上报原因。 */
    void onVideoSnapshotFailed(const QString& reason);

    /** @brief 演示模式下轮换空调工作模式图标。 */
    void onAirCondiModeTimeout();

    /** @brief 演示模式下轮换空调风速图标。 */
    void onAirCondiWindSpeedTimeout();

    /** @brief 演示模式下按当前方向调整空调温度。 */
    void onAirCondiTempTimeout();

    /** @brief 提高视频质量等级并应用相关参数。 */
    void increaseQuality();
    /** @brief 将当前质量等级换算为播放器参数。 */
    void applyVideoQualitySettings();

    /** @brief 使用备用播放器准备列表中的下一段视频。 */
    void preloadNextVideo();

    /** @brief 播放恢复时重新启用被暂停的定时器和资源。 */
    void restoreResourcesOnPlay();

    /** @brief 输出当前播放器优化参数和状态，供现场诊断。 */
    void testVideoOptimizations();

    /** @brief 使用屏幕键盘请求退出密码并与配置值校验。 */
    bool requestPasswordAndCheck();

    /** @brief 将完整信号板状态映射为楼层、方向和状态 UI。 */
    void onSignalBoardState(const SignalBoardState& st);

    /** @brief 处理直播中断，并按业务期望决定回退本地或继续重试。 */
    void onLiveStreamInterrupted(const QString& reason, bool keepLiveRetry);
    /** @brief 强制释放当前直播播放器后回退本地轮播。 */
    void forceStopCurrentLivePlayerForFallback(const QString& reason);
    /** @brief 上游仍有压缩数据但无解码输出时，原地完整重建直播管线。 */
    void restartLivePipelineAfterDecoderStall(const QString& reason);

    /** @brief 启动直播首帧/持续无帧 watchdog。 */
    void startLiveWatchdog();
    /** @brief 停止直播 watchdog 并清空计时状态。 */
    void stopLiveWatchdog();
    /** @brief 判定首帧或持续无帧超时并触发回退。 */
    void onLiveWatchdogTimeout();

    /** @brief 保存后台生成的 Base64 快照并通知 MQTT 状态。 */
    void onSnapshotProcessFinished(const QString &base64);
    /** @brief 释放截图忙状态并记录后台处理失败。 */
    void onSnapshotProcessFailed(const QString &reason);

signals:
    /** @brief 退出密码正确后请求主窗口切回菜单。 */
    void backToMenuRequested();

    /** @brief 当前录播文件发生变化，空字符串表示无录播。 */
    void currentRecordedFileChanged(const QString &fullPath);
    /** @brief 当前播放画面 Base64 快照已更新。 */
    void currentPlayImageChanged(const QString &base64Image);

    /** @brief 当前用户音量发生变化。 */
    void currentPlayVolumeChanged(int volume);

    /** @brief 上报本地手动音量变化及原因。 */
    void localManualVolumeChanged(int volume, const QString &reason);

    /** @brief 请求工作线程合成 UI、视频帧和视频区域。 */
    void processSnapshotAsync(const QImage &uiImage, const QImage &videoImage, const QRect &videoRect);
    /** @brief 无视频帧时请求工作线程仅编码 UI 快照。 */
    void processUiSnapshotAsync(const QImage &uiImage);

    /** @brief 请求页面外层显示指定时长的状态提示。 */
    void statusMessageRequested(const QString &text, int durationMs = 3000);

    /** @brief 多媒体主页面左键持续按住五秒后请求进入人脸管理界面。 */
    void adminLongPressRequested();

    /** @brief 一批等待中的通过图片开始显示，需要重新计算结果保持时间。 */
    void recognitionResultDisplayStarted();


protected:
    /** @brief 处理返回、音量和网络信息热区，并维护光标空闲状态。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

    /** @brief 页面显示时恢复定时器、网络监测和本地播放。 */
    void showEvent(QShowEvent *e) override;
    /** @brief 页面隐藏时暂停不应在后台运行的交互状态。 */
    void hideEvent(QHideEvent *e) override;

private:
    struct RecognitionImageItem {
        QImage image;
        QString name;
        int durationMs = 0;
    };

    Ui::MultimediaDemo *ui;

    QHash<int, QString> m_floorRemap;  /**< 特殊楼层数字到显示文本的映射，如 14 -> "13A"。 */
    char m_floorPrefix = 0; /**< 楼层前缀：0 表示无，其他值为 'B' 或 '-'。 */
    char m_floorLetter = 0; /**< 单字母楼层 A～Z；0 表示未使用。 */

    bool m_floorBlank = false;  // 本帧楼层是否为空（"   "）
    QString m_lastRenderedFloorKey; // 上一次已绘制楼层，避免相同数据重复刷新
    QHash<QString, QPixmap> m_signalUiPixmapCache; // 楼层、方向、状态图片缓存

    // 是否已经收到过信号板完整帧。
    bool m_signalBoardFrameReceived = false;
    bool m_signalUiReady = false; // 配置和图片缓存完成后才允许绘制信号板UI

    // 直播流配置
    bool m_isLiveMode = false;
    bool m_modulePlaybackSuspended = false; /**< 人体感应切换模块期间暂存播放状态。 */

    QTimer* m_liveWatchdogTimer = nullptr;
    qint64 m_lastLiveFrameMs = 0;       // 最近一次真实解码视频帧时间
    qint64 m_liveWatchdogStartMs = 0;   // watchdog 启动时间，用于首次出帧保护
    int m_liveTimeoutMs = 10000;         /**< 连续 10 秒无新帧判定直播中断。 */
    int m_liveStartupGraceMs = 10000;   /**< 启动后 10 秒仍无首帧则回退本地轮播。 */
    int m_liveDecoderInputActiveWindowMs = 3000; /**< 此窗口内仍有解码输入则判为解码器停滞。 */
    bool m_liveSourceValidated = false;  // 当前LIVE地址是否曾经输出过真实解码视频帧
    bool m_liveRecovering = false;

    MqttManager *mqttManager_ = nullptr;
    bool m_snapshotPending = false;         /**< 防止播放器抓图请求重入。 */

    // 截图线程
    QThread *m_snapshotThread = nullptr;
    SnapshotProcessor *m_snapshotProcessor = nullptr;

    QTimer *m_snapshotTimeoutTimer = nullptr;

    //截图工作对象
    bool m_snapshotWorkerBusy = false;
    qint64 m_lastSnapshotMs = 0;
    qint64 m_snapshotRequestMs = 0;
    QString m_lastSnapshotBase64;

    bool m_liveDesired = false;          // MQTT当前是否要求播放直播
    QString m_desiredLiveUrl;
    QString m_desiredLiveKind;

    QTimer *m_liveRetryTimer = nullptr;  /**< 直播探测失败后的重试定时器。 */
    int m_liveRetryIntervalMs = 10000;
    QElapsedTimer m_liveProbeFailureLogElapsed; /**< 持续探测失败时限制状态日志的打印频率。 */
    QString m_liveProbeFailureLogUrl;    /**< 当前受失败日志限流的直播地址。 */
    qint64 m_localPlayerBusyUntilMs = 0; // 本地视频切文件保护窗口，避免 LIVE 后台探测抢占 GStreamer

    // 音量设置
    AmpVolumeController *m_ampCtrl = nullptr;
    VolumePanel *m_volPanel = nullptr;

    // 弹窗交互锁
    bool m_uiModalActive = false;   // 只要有任何“上层窗口”在，就置 true
    QRect m_volumeRect;   /**< 音量弹窗点击热区。 */

    // 鼠标状态变量
    bool m_cursorHidden = false;
    bool m_initialCursorStateApplied = false; /**< 首次显示页面时是否已经应用默认隐藏。 */
    QTimer *m_adminLongPressTimer = nullptr; /**< 管理入口五秒长按计时器。 */
    bool m_leftPressTracking = false;        /**< 当前是否正在跟踪主页面左键长按。 */
    bool m_adminLongPressTriggered = false;  /**< 本次按下是否已经触发管理入口。 */
    QPoint m_leftPressPoint;                 /**< 短按释放后执行旧热点动作的位置。 */
    // 防重入 + 屏蔽标记
    bool m_leaving = false;

    SignalBoardClient *sigClient = nullptr;

    // 外部资源根目录 + qrc 根目录
    const QString extRoot = Rk3566Platform::staticRoot();
    const QString qrcRoot = ":/static";

    // 数据缓冲区
    int my_buf[10];
    int times;

    // 状态变量
    DirectionStatus dir_status;
    StateStatus state_status;
    bool RunMode; /**< false 为正常运行，true 为演示运行。 */
    QTimer *m_elevatorDemoTimer = nullptr; /**< 演示模式下每秒推进楼层和状态标志。 */
    int m_demoFloor = -8;                  /**< 当前演示楼层，范围 -8～32。 */
    DirectionStatus m_demoDirection = UP;  /**< 当前演示运行方向。 */
    int m_demoModeAscii = 0x31;            /**< 演示状态：满载、超载、消防、检修循环。 */

    // UI组件
    QLabel *label_direction;
    QMovie *m_directionMovie = nullptr; /**< 当前方向 GIF，切换方向或停止时释放。 */
    QLabel *label_ten;
    QLabel *label_one;
    QLabel *label_state;
    QLabel *label_date;
    QLabel *label_week;
    QLabel *label_time;
    QLabel *label_back;
    QLabel *label_bg01;
    QLabel *label_logo1;
    QLabel *label_logo2;
    QLabel *label_logo3;
    QLabel *label_logo4;
    QWidget *m_moveTextViewport = nullptr; /**< moveTextRect 限定的透明滚动区域。 */
    QLabel *m_moveTextLabel = nullptr;     /**< notice.txt 单行滚动文字。 */
    QTimer *m_moveTextTimer = nullptr;     /**< 滚动文字平移动画定时器。 */
    int m_moveTextOffsetX = 0;             /**< 文字当前在区域内的横坐标。 */
    int m_moveTextStepPx = 2;              /**< 每帧左移像素数，对应五档速度。 */
    CameraPreviewWidget *m_facePreview = nullptr; /**< pictureRect 内嵌人脸预览。 */
    QWidget *m_recognitionResultHost = nullptr;   /**< pictureRect 通过人员图片覆盖容器。 */
    QList<QLabel *> m_recognitionImageLabels;     /**< 当前批次中能完整放入目标区域的通过图片。 */
    QHash<QLabel *, QImage> m_recognitionImages;  /**< 通过图片原图，用于重新布局缩放。 */
    QHash<QLabel *, QString> m_recognitionNames;  /**< 每张通过图片对应的人员姓名。 */
    QList<RecognitionImageItem> m_pendingRecognitionImages; /**< 容量不足时等待下一批的图片。 */
    QString m_latestRecognitionName; /**< 结果照片显示期间用于锁定人脸框的通过文字。 */
    bool m_embeddedFaceRecognitionActive = false;
    bool m_picturePausedForFaceOverlay = false;
    bool m_embeddedFacePresent = false;

    // 图像对象

    // 定时器
    QTimer *timer;
    QTimer *date_timer;

    // 配置参数
    bool Flag1_en;
    bool Flag2_en;
    bool DateDisplay_en;
    bool WeekDisplay_en;
    bool TimeDisplay_en;
    bool Diretion_en;
    bool backPic_en;
    bool bg01Pic_en;
    bool SpecialFloor_en;
    bool BgDisplay_en;
    bool Logo1_en;
    bool Logo2_en;
    bool Logo3_en;
    bool Logo4_en;
    int DisPlayPicOrText;

    // 位置和大小参数
    int First_Floor_x;
    int First_Floor_y;
    int First_Floor_x_Size;
    int First_Floor_y_Size;
    int First_FloorS_x;
    int First_FloorS_y;
    int Flag1_x;
    int Flag1_y;
    int Flag1_x_Size;
    int Flag1_y_Size;
    int Flag2_x;
    int Flag2_y;
    int Flag2_x_Size;
    int Flag2_y_Size;
    int Diretion_x;
    int Diretion_y;
    int Diretion_x_Size;
    int Diretion_y_Size;
    int DateDisplay_x;
    int DateDisplay_y;
    int DateDisplay_x_Size;
    int DateDisplay_y_Size;

    int DateTextColorR;//字体颜色
    int DateTextColorG;//字体颜色
    int DateTextColorB;//字体颜色
    int DateTextSize;//字体大小
    int DateTextStyle;//字体是否加粗

    int TimeDisplay_x;
    int TimeDisplay_y;
    int TimeDisplay_x_Size;
    int TimeDisplay_y_Size;
    int TimeTextColorR;
    int TimeTextColorG;
    int TimeTextColorB;
    int TimeTextSize;
    int TimeTextStyle;

    int backPic_x_Size;
    int backPic_y_Size;
    int WeekDisplay_x;
    int WeekDisplay_y;
    int WeekDisplay_x_Size;
    int WeekDisplay_y_Size;
    int WeekTextColorR;
    int WeekTextColorG;
    int WeekTextColorB;
    int WeekTextSize  ;
    int WeekTextStyle ;
    int WeekTextDispay;

    int SpecialFloor_x;
    int SpecialFloor_y;
    int BgDisplay_x;
    int BgDisplay_y;
    int BgDisplay_x_Size;
    int BgDisplay_y_Size;

    int Logo1_x;
    int Logo1_y;
    int Logo1_x_Size;
    int Logo1_y_Size;
    int Logo2_x;
    int Logo2_y;
    int Logo2_x_Size;
    int Logo2_y_Size;
    int Logo3_x;
    int Logo3_y;
    int Logo3_x_Size;
    int Logo3_y_Size;
    int Logo4_x;
    int Logo4_y;
    int Logo4_x_Size;
    int Logo4_y_Size;

    // 滚动通知文字配置
    bool MoveTextDisplay_en = false;
    int MoveTextDisplay_x = 0;
    int MoveTextDisplay_y = 730;
    int MoveTextDisplay_x_Size = 1024;
    int MoveTextDisplay_y_Size = 40;
    int MoveTextColorR = 255;
    int MoveTextColorG = 255;
    int MoveTextColorB = 255;
    int MoveTextSize = 25;
    int MoveTextSpeed = 2;

    // 视频显示相关变量
    bool VideoDisplay_en;
    int VideoDisplay_x;
    int VideoDisplay_y;
    int VideoDisplay_x_Size;
    int VideoDisplay_y_Size;

    int VideoPic_disp_Time;    /**< videoRect 图片轮播间隔，单位秒。 */

    /** @brief videoRect 混播列表中的资源类型。 */
    enum VideoRectMediaType {
        VideoRectMedia_Video = 0, /**< 视频文件。 */
        VideoRectMedia_Image = 1  /**< 静态图片。 */
    };

    /** @brief videoRect 混播列表中的一个本地媒体项。 */
    struct VideoRectMediaItem {
        VideoRectMediaType type; /**< 资源类型。 */
        QString filePath;        /**< 本地绝对路径。 */

        VideoRectMediaItem()
            : type(VideoRectMedia_Video)
        {
        }

        VideoRectMediaItem(VideoRectMediaType t, const QString &p)
            : type(t), filePath(p)
        {
        }
    };

    // videoRect 区域图片显示
    QLabel *label_videoRectPicture;
    QList<QString> videoRectPictureFileList;
    QList<VideoRectMediaItem> videoRectMediaList;
    int currentVideoRectMediaIndex;
    QTimer *videoRectPictureTimer;
    bool m_currentVideoRectIsImage;
    QString m_currentVideoRectImagePath;
    QString m_currentVideoRectMediaPath;   /**< 当前 videoRect 正在播放的资源路径。 */

    // 图片显示相关变量
    QLabel *label_picture;
    bool PicDisplay_en;
    int PicDisplay_x;
    int PicDisplay_y;
    int PicDisplay_x_Size;
    int PicDisplay_y_Size;
    int PicDisplay_Time;
    bool PicToFlag_En;
    int PicDisplay_XgSpeed;

    // 视频播放相关成员变量
    GstPlayerWidget  *mediaPlayer;
    GstPlayerWidget  *nextVideoPlayer; /**< 预加载下一视频的备用播放器。 */

    // 直播探测播放器
    GstPlayerWidget *m_liveProbePlayer = nullptr;
    bool m_liveProbeRunning = false;
    bool m_liveSwitching = false;

    VideoHoleWidget  *videoWidget;
    QWidget  *nextVideoWidget;

    QList<QString> videoFileList;
    int currentVideoIndex;
    bool isPreloading; /**< 备用播放器正在异步预加载。 */


    // 视频缓冲相关变量
    QLabel *bufferIndicator; // 缓冲指示器
    bool isBuffering; // 缓冲状态标志
    int bufferPercentage; // 缓冲百分比
    int maxBufferDuration; // 最大缓冲时长（毫秒）
    bool autoQualityAdjustment; // 自动质量调整标志
    int currentQualityLevel; // 当前质量级别（1-低，2-中，3-高）
    int qualityCheckInterval; // 质量检查间隔（毫秒）

    QList<QString> pictureFileList;
    QList<QPixmap> picturePixmapCache; // 启动时预加载并缩放后的图片缓存
    int currentPictureIndex;
    QTimer *pictureTimer;

    int SpecialFloor_x_Size;
    int SpecialFloor_y_Size;
    int oldFloor1;
    QString newFloor1;

    // 空调配置参数
    bool AirCondi_en;
    bool AirCondiMode_en;
    bool AirCondiTemp_en;
    bool AirCondiWind_en;
    int AirCondiMode_x;
    int AirCondiMode_y;
    int AirCondiMode_x_Size;
    int AirCondiMode_y_Size;
    int AirCondiTemp_x;
    int AirCondiTemp_y;
    int AirCondiTemp_x_Size;
    int AirCondiTemp_y_Size;
    int AirCondiTemp_font_Size;
    int AirCondiWind_x;
    int AirCondiWind_y;
    int AirCondiWind_x_Size;
    int AirCondiWind_y_Size;

    // 空调模式显示相关成员变量
    QLabel *label_airCondiMode;
    QTimer *airCondiModeTimer;
    int currentAirCondiMode; // 1: cool, 2: fan, 3: hot

    // 空调风速显示相关成员变量
    QLabel *label_airCondiWindSpeed;
    QTimer *airCondiWindSpeedTimer;
    int currentAirCondiWindSpeed; // 1-4

    // 空调温度显示相关成员变量
    QLabel *label_airCondiTemp;
    QTimer *airCondiTempTimer;
    int currentAirCondiTemp; // 当前温度值
    int tempDirection; // 温度变化方向，1: 增加，-1: 减小
    QString airCondiTempColor; // 温度显示颜色

    // 广播配置参数
    bool BroadCast_en;
    int BroadCast_x;
    int BroadCast_y;
    int BroadCast_x_Size;
    int BroadCast_y_Size;
    QLabel *label_broadCast;

    // 可视对讲配置参数
    bool VoiceTalk_en;
    int VoiceTalk_x;
    int VoiceTalk_y;
    int VoiceTalk_x_Size;
    int VoiceTalk_y_Size;

    // 本地视频列表
    QTimer *m_emptyVideoListTimer = nullptr;
    int m_emptyVideoScanIntervalMs = 1000; /**< 空列表时每秒重新扫描一次。 */
    bool m_localPlaybackEnabled = false;   /**< splash 结束且模块在前台时才允许本地播放。 */
    bool m_startupPlaybackReleased = false; /**< 启动页结束后置 true，阻止启动阶段提前开播。 */
    bool m_initialVideoPlaybackStarted = false; /**< 首次视频区域初始化是否已经执行。 */

    // 网络信息
    NetInfoPanel *m_netInfoPanel = nullptr;

    QString m_netIp;
    QString m_mqttClientId;
    QString m_mqttHost;
    QString m_mqttRouteName;
    QString m_mqttSubTopics;
    QString m_streamUrl;

    // 弹窗定时器
    QTimer *m_panelAutoHideTimer = nullptr;


    DiskUsageMonitor *m_diskUsageMonitor = nullptr;
    NetworkStatusMonitor *m_networkStatusMonitor = nullptr;

    QLabel *m_passwordToastLabel = nullptr;
    QTimer *m_passwordToastTimer = nullptr;

    DownloadProgressToast *m_downloadToast = nullptr;
    bool m_downloadPauseActive = false;
    bool m_downloadResumeLocalAfterFinish = false;
    QString m_downloadActiveFileName;
    quint64 m_localPlaybackStartSerial = 0;


private:
    /** @brief 执行原有日期、时间和视频区短按热点。 */
    bool handleShortLeftClick(const QPoint &position);
    /** @brief 取消尚未触发的管理入口长按。 */
    void cancelAdminLongPress();
    /** @brief 按固定卡片宽度和区域容量居中布局当前批次通过图片。 */
    void layoutRecognitionImages();
    /** @brief 将一张通过图片加入当前显示批次并启动其显示时长。 */
    void appendRecognitionImage(const RecognitionImageItem &item);
    /** @return 当前批次增加该图片后是否仍能完整容纳在目标区域。 */
    bool canAppendRecognitionImage(const QImage &image) const;
    /** @brief 当前批次全部结束后，从等待队列填充下一批。 */
    void startNextRecognitionBatch();
    /** @return 以识别虚线框比例和 pic 高度计算的固定卡片宽度。 */
    int recognitionCardWidth(const QImage &image) const;
    /** @return 以识别虚线框为基准、且不超过 pic 高度的卡片高度。 */
    int recognitionCardHeight() const;
    /** @return 带圆角及底部姓名栏的通过人员卡片。 */
    QPixmap renderRecognitionCard(const QImage &image,
                                  const QString &name,
                                  const QSize &size) const;
    /** @brief 清理所有临时通过图片并恢复 pic 轮播。 */
    void clearRecognitionImages(bool restorePicturePlayback);
    /** @brief 在临时通过图片全部消失后恢复 pic 区原有内容。 */
    void restorePictureAfterRecognitionResult();
    /** @brief 加载图片并屏蔽现场资源文件可能产生的非致命解码告警。 */
    QPixmap loadPixmapWithIgnoreWarnings(const QString &imagePath);
    /** @brief 优先解析外部资源路径，不存在时回退到 qrc。 */
    QString resolveResourcePath(const QString &pathOrRel) const;
    /** @brief 从缓存取得信号板 UI 图片，首次访问时加载。 */
    QPixmap cachedSignalUiPixmap(const QString &imagePath);
    /** @brief 启动时预加载楼层、方向和状态图片，降低串口更新抖动。 */
    void preloadSignalUiPixmaps();
    /** @brief 优先播放方向 GIF，不可用时回退到原 PNG。 */
    void showDirectionImage(DirectionStatus direction);
    /** @brief 停止并解除当前方向动画与标签的关联。 */
    void clearDirectionMovie();
    /** @brief 显示方向 PNG；图片不可用时隐藏标签。 */
    void showDirectionPng(DirectionStatus direction);

    /** @brief 按配置区域缩放并显示楼层图片。 */
    void showFloor(const QPixmap &pixmap, QLabel *label, int x, int y, int width, int height);
    /** @brief 设置标签几何并显示指定图片。 */
    void showImage(const QPixmap &pixmap, QLabel *label, const QString &imagePath, int x, int y, int width, int height);
    /** @brief 从系统时钟刷新日期、星期和时间标签。 */
    void getTimeWithQt();
    /** @brief 按配置样式格式化星期文本。 */
    QString formatWeekText(int dayOfWeek, int style);
    /** @brief 创建页面标签、播放器、监测器和业务对象。 */
    void initializeComponents();
    /** @brief 创建并连接页面全部周期定时器。 */
    void initializeTimers();
    /** @brief 读取配置后初始化界面可见性和几何参数。 */
    void initializeConfiguration();
    /** @brief 初始化信号板接收缓冲及电梯演示状态。 */
    void simulateBufferData();
    /** @brief 演示模式下每秒推进楼层、轮换四种标志，并在楼层端点反向。 */
    void onElevatorDemoTimeout();
    /** @brief 从设备配置文件加载显示、媒体和功能参数。 */
    void readConfigurationFromFile();
    /** @brief 用十位和个位标签显示普通数字楼层。 */
    void displayFloorAsText(int floor, int ten, int one);
    /** @brief 显示单字母楼层。 */
    void displaySingleLetterFloorAsText(char letter);
    /** @brief 显示带 B 或负号前缀的楼层。 */
    void displayPrefixedFloorAsText(char prefix, int one);
    /** @brief 创建主/预加载播放器并连接状态信号。 */
    void initVideoPlayer();
    /** @brief 按列表索引切换到下一段本地视频。 */
    void playNextVideo();
    /** @brief 扫描图片目录并启动图片轮播。 */
    void initPicturePlayer();

    /** @brief 重新扫描 videoRect 专用图片列表。 */
    /** @brief 按配置顺序重建 videoRect 视频/图片混播列表。 */
    void rebuildVideoRectMediaList();
    /** @brief 从混播列表移除运行期间已不存在的文件。 */
    void pruneMissingVideoRectMedia();
    /** @brief 按环形索引播放一个 videoRect 媒体项。 */
    void playVideoRectMediaAt(int index);
    /** @brief 让主播放器播放 videoRect 视频项。 */
    void playVideoRectVideo(const QString &filePath);
    /** @brief 隐藏视频画布并显示 videoRect 图片项。 */
    void playVideoRectImage(const QString &filePath);
    /** @brief 停止 videoRect 图片计时器并按需隐藏标签。 */
    void stopVideoRectPictureDisplay(bool hideLabel = true);
    /** @return 当前混播项是图片时返回 true。 */

    /** @brief 轮换普通图片显示区域中的下一张图片。 */
    void playNextPicture();

    /** @brief 重新扫描本地视频目录并保持可用的当前索引。 */
    void reloadVideoFileList();

    /** @brief 从本地视频列表移除运行期间已删除的文件。 */

    /** @brief 打开日期时间输入并同步系统与 RTC。 */
    void openDateTimeSetting();
    /** @brief 使用系统命令设置本地系统时间。 */
    bool setSystemTimeFromLocal(const QDateTime& localDT, QString* err);
    /** @brief 将本地时间写入 RTC 设备。 */
    bool setRtcFromLocal(const QDateTime& localDT, QString* err);
    /** @brief 从 RTC 恢复系统时间。 */
    bool restoreSystemTime(QString* err = nullptr);

    /** @brief 在仍期望直播时启动定时重试。 */
    void startLiveRetry();
    /** @brief 停止直播重试并清除重入标记。 */
    void stopLiveRetry();
    /** @brief 重试定时到达时重新探测期望直播源。 */
    void onLiveRetryTimeout();

    /** @brief 仅停止主播放器中的直播，不清除业务期望状态。 */
    void stopCurrentLivePlaybackOnly();

    /** @return 配置允许持久化在线播放状态时返回 true。 */
    bool isOnlinePlaybackPersistEnabled() const;
    /** @brief 把期望模式、实际模式和切换原因写入网络配置。 */
    void persistPlaybackStateToNetCfg(const QString &targetMode,
                                      const QString &currentMode,
                                      const QString &reason);
    /** @brief 启动时从网络配置恢复上次播放目标。 */
    void restorePlaybackStateFromNetCfg();

    /** @brief 本地列表为空时启动短周期目录观察。 */
    void startEmptyVideoListWatch();
    /** @brief 有可播文件或页面退出时停止空列表观察。 */
    void stopEmptyVideoListWatch();
    /** @brief 重新扫描目录并在发现文件后启动首项。 */
    void onEmptyVideoListWatchTimeout();
    /** @brief 立即播放刚下载的文件并保护本次启动不被旧回调覆盖。 */
    void playDownloadedVideoNow(const QString& filePath);

    /** @brief 从当前列表首个可用文件启动本地轮播。 */
    void startFirstLocalVideoFromList();

    /** @brief 从配置读取并刷新网络/MQTT 信息面板字段。 */
    void readNetConfiguration();

    /** @brief 隐藏音量、网络等临时信息面板。 */
    void hideInfoPanels();

    /** @brief 从 INI 加载特殊楼层数值到显示名称的映射。 */
    void loadFloorRemap(const QString& iniPath);
    /** @brief 按 moveTextRect 配置读取外部 notice.txt 并启动滚动。 */
    void initializeMovingText();
    /** @brief 将通知文字向左推进一帧，完全离开后从右侧重新进入。 */
    void advanceMovingText();
    /** @brief 将滚动文字保持在媒体、普通图片和识别结果图片之上。 */
    void raiseMovingTextOverlay();
    /** @brief 尝试显示当前楼层对应的专用图片。 */
    bool tryShowSpecialFloorImage(int Value);

    /** @brief 根据弹窗和用户活动刷新空闲计时状态。 */
    void refreshUiIdleStateByActivity();

    /** @brief 按页面可见性和模态状态启停光标隐藏定时。 */
    void updateIdleTimerState();

    /** @brief 若软件光标已隐藏则恢复显示。 */
    void showCursorIfHidden();

    /** @return 事件来源属于本页面或其子控件时返回 true。 */
    bool isEventFromThisPage(QObject *watched, QEvent *event) const;
    /** @brief 出现模态窗口时暂停页面空闲隐藏逻辑。 */
    void suspendUiIdleForModal();
    /** @brief 页面离开时停用空闲隐藏并恢复光标。 */
    void deactivateUiIdle();

    /** @brief 创建密码错误的非模态提示标签和定时器。 */
    void initPasswordErrorToast();
    /** @brief 显示指定时长的密码错误提示。 */
    void showPasswordErrorToast(const QString &text, int ms = 1800);

    /** @brief 下载期间暂停本地轮播并记录是否需要恢复。 */
    void pauseLocalPlaybackForDownload();
    /** @brief 下载结束后按之前状态恢复本地轮播。 */
    void resumeLocalPlaybackAfterDownload();
    /** @brief 更新非模态下载进度提示。 */
    void showDownloadProgressToast(const QString &title, int percent, const QString &detail = QString());
    /** @brief 隐藏并重置下载进度提示。 */
    void hideDownloadProgressToast();

    /** @brief 生成本地播放启动序号，用于识别异步回调所属启动。 */
    quint64 issueLocalPlaybackStartSerial();
    /** @brief 递增序号，使所有在途本地播放启动回调失效。 */
    void invalidatePendingLocalPlaybackStarts();
};

#endif // MULTIMEDIADEMO_H
