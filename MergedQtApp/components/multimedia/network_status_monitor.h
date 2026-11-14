/**
 * @file network_status_monitor.h
 * @brief 网络链路、外网和直播源可用性的分阶段监测告警控件。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-13；修改人：Du lin；声明网络线程直播源请求和独立取消接口，移除告警控件中的 HTTP/TCP/UDP 传输状态。
 */

#ifndef NETWORK_STATUS_MONITOR_H
#define NETWORK_STATUS_MONITOR_H

#include <QWidget>
#include <memory>
struct NetworkCheckGuard;
class NetworkProbeWorker;
class QThread;
#include <QTimer>
#include <QRect>
#include <QString>
#include <QFont>
#include "live_stream_probe.h"

/**
 * @brief 网络链路、外网和直播源可用性的分阶段监测告警控件。
 *
 * 每轮检查先判断网线和默认路由，再异步 ping 外网；仅当业务期望直播但播放器
 * 尚未进入 LIVE 时，才按 HTTP、RTSP、RTMP 或 UDP 协议探测直播源。所有探测均
 * 带超时，并通过短时闪现提示避免长期遮挡主界面。
 * @details UI 管理覆盖层、检查节拍和结果应用。网络线程拥有配置和路由读取、
 * ping、两路直播源请求及共享 HTTP 管理器。取消通过独立代次使旧结果失效，析构不等待网络线程。
 */
class NetworkStatusMonitor : public QWidget
{
    Q_OBJECT
public:
    /**
     * @brief 创建网络告警控件并配置分阶段探测所需的定时器和异步对象。
     * @param mainWindow 告警覆盖层跟随并定位的主窗口。
     * @param configPath 业务配置文件路径；为空时使用平台默认路径。
     */
    explicit NetworkStatusMonitor(QWidget *mainWindow,
                                  const QString &configPath = QString());

    /** @brief 销毁前取消旧查询结果，普通stop不等待后台查询。 */
    ~NetworkStatusMonitor() override;

    /** @brief 启动周期检查并立即异步提交首轮检测。 */
    void start();
    /** @brief UI 停止告警及其探测，异步终止 ping；播放器重试独立取消。 */
    void stop();

    /** @return 当前存在网络或直播源告警时返回 true。 */
    bool isWarningActive() const;

    /** @brief UI 排队提交直播重试，并在 UI 执行仍有效的结果回调。
     * @param[in] url 待探测的直播源。
     * @param[in] kind 原业务协议类型。
     * @param[in] completion 结果回调；新请求、取消或析构会使旧回调失效。
     */
    void requestPlaybackProbe(const QString &url, const QString &kind, LiveStreamProbe::Completion completion);
    /** @brief 取消播放器在途探测，不影响网络告警和 HTTP 管理器。 */
    void cancelPlaybackProbe();

protected:
    /** @brief 绘制居中的半透明告警框和文字。 */
    void paintEvent(QPaintEvent *event) override;
    /** @brief 主窗口尺寸变化时重新定位告警框。 */
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    /** @brief 启动一轮非重入的分阶段网络检查。 */
    void checkNetworkStatus();
    /** @brief 告警持续存在时周期显示提示框。 */
    void onBlinkTimeout();

private:
    /** @brief 一轮异步检查所处阶段。 */
    enum class CheckState {
        Idle, /**< 无在途检测。 */
        CheckingRoute, /**< 后台只读初检，禁止周期重复提交。 */
        Pinging, /**< 原异步外网ping阶段。 */
        StreamProbing /**< 原异步直播源探测阶段。 */
    };

    /** @brief 需要向用户区分显示的故障类型。 */
    enum class WarningKind {
        None,
        LinkDown,
        NoInternet,
        StreamNoData
    };

    /** @brief 读取有线网卡 carrier；valid 表示系统节点是否可读。 */
    static bool isWiredLinkUp(bool *valid = nullptr);
    /** @brief 仅后台任务读取/proc并执行原有限等待的命令回退，禁止从UI调用。
     * @return 路由表或原命令回退发现默认路由时true，否则false。
     */
    static bool hasDefaultRoute();

    /** @brief UI应用公网结果；需要检查直播源时再排队提交网络任务。
     * @param[in] internetOk 是否探测成功。
     * @param[in] onlineEnabled 后台重新读取的在线配置结果；false清除告警，不在UI读取INI。
     */
    void finishInternetProbe(bool internetOk,bool onlineEnabled);

    /** @brief UI 收到本轮直播探测结果后应用或清除告警。
     * @param[in] streamOk true 清除直播源告警。
     * @param[in] detail 失败原因。
     */
    void finishStreamProbe(bool streamOk, const QString &detail);

    /** @brief 设置告警类型、文案及闪现定时器。 */
    void setWarning(WarningKind kind, const QString &detail = QString());
    /** @brief 清除告警并隐藏控件。 */
    void clearWarning();
    /** @brief 按故障类型生成用户可读文案。 */
    void updateWarningText(WarningKind kind, const QString &detail = QString());

    /** @brief 显示一次告警并安排自动隐藏。 */
    void showWarningNow();
    /** @brief 隐藏告警但保留 active 状态供下次闪现。 */
    void hideWarningNow();

    /** @brief 按当前文字和宽度上限调整告警框尺寸。 */
    void resizeToFitText();
    /** @brief 将告警框移动到主窗口可视区域中央。 */
    void moveBoxToScreenCenter();

private:
    friend class NetworkProbeWorker; /**< 仅允许后台通过值结果投递访问原UI判定，不直接操作控件。 */
    QThread *networkThread_=nullptr; /**< 无UI父对象，最终由finished异步deleteLater；UI不join。 */
    NetworkProbeWorker *networkWorker_=nullptr; /**< 网络线程独占，随线程退出删除，UI只排队调用。 */
    std::shared_ptr<NetworkCheckGuard> networkGuard_; /**< 短锁生命周期门及原子代次；取消不等待任务，后台结果不回读已销毁控件。 */
    QWidget *main_ = nullptr;

    QTimer checkTimer_;
    QTimer blinkTimer_;
    QTimer autoHideTimer_;

    CheckState checkState_ = CheckState::Idle; /**< 防止周期定时器重入正在执行的异步检查。 */
    WarningKind warningKind_ = WarningKind::None;

    bool warningActive_ = false;
    bool warningVisible_ = false;
    bool lastDefaultRouteOk_ = true;

    QString text_;
    QRect boxRect_;
    QFont baseFont_;

    int maxWidth_ = 520;
    int padX_ = 12;
    int padY_ = 16;

    int checkIntervalMs_ = 10000;       /**< 每 10 秒检查一次状态。 */
    int blinkIntervalMs_ = 60000;       /**< 告警存在时每 1 分钟闪现一次。 */
    int visibleDurationMs_ = 5000;      /**< 每次显示 5 秒。 */
    int pingTimeoutMs_ = 3000;          /**< 单个外网目标最多等待 3 秒。 */
};

#endif // NETWORK_STATUS_MONITOR_H
