/**
 * @file network_status_monitor.h
 * @brief 网络链路、外网和直播源可用性的分阶段监测告警控件。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef NETWORK_STATUS_MONITOR_H
#define NETWORK_STATUS_MONITOR_H

#include <QWidget>
#include <QTimer>
#include <QRect>
#include <QString>
#include <QFont>
#include <QProcess>
#include <QUrl>
#include <QAbstractSocket>

class QNetworkAccessManager;
class QNetworkReply;
class QTcpSocket;
class QUdpSocket;

/**
 * @brief 网络链路、外网和直播源可用性的分阶段监测告警控件。
 *
 * 每轮检查先判断网线和默认路由，再异步 ping 外网；仅当业务期望直播但播放器
 * 尚未进入 LIVE 时，才按 HTTP、RTSP、RTMP 或 UDP 协议探测直播源。所有探测均
 * 带超时，并通过短时闪现提示避免长期遮挡主界面。
 */
class NetworkStatusMonitor : public QWidget
{
    Q_OBJECT
public:
    /**
     * @brief 创建网络告警控件并配置分阶段探测所需的定时器和异步对象。
     * @param mainWindow 告警覆盖层跟随并定位的主窗口。
     * @param parent Qt 父对象。
     * @param configPath 业务配置文件路径；为空时使用平台默认路径。
     */
    explicit NetworkStatusMonitor(QWidget *mainWindow,
                                  const QString &configPath = QString());

    /** @brief 启动周期检查并立即执行首轮检测。 */
    void start();
    /** @brief 停止定时器、进程和所有进行中的流探测。 */
    void stop();

    /** @return 当前存在网络或直播源告警时返回 true。 */
    bool isWarningActive() const;

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
    /** @brief 处理当前 ping 目标结果并决定重试或进入直播探测。 */
    void onPingFinished(int exitCode, QProcess::ExitStatus exitStatus);
    /** @brief 终止超时的 ping 并尝试下一个目标。 */
    void onPingTimeout();

private:
    /** @brief 一轮异步检查所处阶段。 */
    enum class CheckState {
        Idle,
        Pinging,
        StreamProbing
    };

    /** @brief 需要向用户区分显示的故障类型。 */
    enum class WarningKind {
        None,
        LinkDown,
        NoInternet,
        StreamNoData
    };

    /** @brief 从配置解析出的当前直播探测目标。 */
    struct LiveProbeTarget {
        bool needed = false;   /**< 业务目标为 LIVE，但实际尚未进入 LIVE。 */
        bool ok = false;       /**< URL 和类型可用于探测。 */
        QString url;           /**< 归一化前的直播地址。 */
        QString kind;          /**< rtsp、hls、udp 等业务类型。 */
        QString detail;        /**< 无法探测时用于告警的原因。 */
    };

    /** @brief UDP 单播或组播探测所需的端点。 */
    struct UdpProbeTarget {
        bool ok = false;
        bool multicast = false;
        QString host;
        quint16 port = 0;
    };

    /** @brief 从 TCP 缓冲区解析出的一个 RTSP 响应。 */
    struct RtspResponse {
        bool complete = false;
        int statusCode = 0;
        QString statusLine;
        QByteArray body;
        int consumedBytes = 0;
    };

    /** @brief RTSP SDP 中声明的组播地址和端口。 */
    struct RtspMulticastInfo {
        bool ok = false;
        QString group;
        quint16 port = 0;
    };

private:
    /** @return 配置允许联网业务时返回 true。 */
    bool isOnlineFeatureEnabled() const;
    /** @brief 读取有线网卡 carrier；valid 表示系统节点是否可读。 */
    bool isWiredLinkUp(bool *valid = nullptr) const;
    /** @return 路由表中存在默认路由时返回 true。 */
    bool hasDefaultRoute() const;

    /** @brief 依次异步 ping 配置的外网目标。 */
    void beginInternetProbe();
    /** @brief 结束外网探测并继续直播源检查或设置告警。 */
    void finishInternetProbe(bool internetOk);

    /** @brief 从共享配置读取“期望直播/实际模式/URL/类型”。 */
    LiveProbeTarget readLiveProbeTargetFromConfig() const;
    /** @brief 按 URL scheme 和扩展名推断直播协议类型。 */
    QString detectLiveKindFromUrl(const QString &url) const;
    /** @return 业务类型应按裸 UDP 数据流探测时返回 true。 */
    bool isRawUdpLiveKind(const QString &scheme, const QString &kind) const;
    /** @brief 解析 UDP URL 中的主机和端口。 */
    UdpProbeTarget parseUdpProbeTarget(const QString &url) const;
    /** @return 文本是 IPv4 组播地址时返回 true。 */
    bool isMulticastIpv4(const QString &ipText) const;

    /** @brief 仅在当前业务需要时启动直播源探测。 */
    void beginLiveStreamProbeIfNeeded();
    /** @brief 根据目标协议选择具体异步探测实现。 */
    void startLiveStreamProbe(const LiveProbeTarget &target);
    /** @brief 监听 UDP 数据，组播目标会加入对应组。 */
    void startUdpStreamProbe(const UdpProbeTarget &target, const QString &sourceUrl, const QString &detailPrefix = QString());
    /** @brief 通过 HTTP 响应头和少量正文判断直播内容。 */
    void startHttpStreamProbe(const QUrl &url, const QString &sourceUrl);
    /** @brief 执行 RTSP DESCRIBE/SETUP，并在需要时继续探测 SDP 组播。 */
    void startRtspStreamProbe(const QUrl &url, const QString &sourceUrl);
    /** @brief 通过 TCP 握手可达性探测 RTMP 地址。 */
    void startRtmpStreamProbe(const QUrl &url, const QString &sourceUrl);
    /** @brief 收束流探测资源并更新告警状态。 */
    void finishStreamProbe(bool streamOk, const QString &detail = QString());
    /** @brief 中止并释放当前 HTTP/TCP/UDP 探测对象。 */
    void cancelStreamProbe();

    /** @brief 从累计缓冲区解析完整 RTSP 响应及消费字节数。 */
    RtspResponse parseRtspResponse(const QByteArray &buffer) const;
    /** @brief 从 RTSP 状态行解析三位状态码。 */
    int parseRtspStatusCode(const QByteArray &statusLine) const;
    /** @brief 从 RTSP 头解析 Content-Length。 */
    int parseRtspContentLength(const QByteArray &header) const;
    /** @brief 从 SDP 找到优先的视频 track control。 */
    QString findRtspVideoControl(const QByteArray &sdpBody) const;
    /** @brief 将 SDP control 值解析为可请求的绝对 RTSP URL。 */
    QString buildRtspControlUrl(const QUrl &baseUrl, const QString &control) const;
    /** @brief 从 SDP 媒体段提取视频组播端点。 */
    RtspMulticastInfo findRtspMulticastInfo(const QByteArray &sdpBody) const;
    /** @brief 向套接字发送 RTSP DESCRIBE 请求。 */
    void sendRtspDescribe(QTcpSocket *sock, const QString &url);
    /** @brief 向套接字发送 RTSP SETUP 请求。 */
    void sendRtspSetup(QTcpSocket *sock, const QString &trackUrl);

    /** @return HTTP 状态码表示成功时返回 true。 */
    bool isHttpSuccessStatus(int statusCode) const;
    /** @return HTTP 状态码表示重定向时返回 true。 */
    bool isHttpRedirectStatus(int statusCode) const;
    /** @return 响应正文看起来是 HTML 错误页时返回 true。 */
    bool isProbablyHtmlBody(const QByteArray &body) const;
    /** @brief 结合 URL、Content-Type 和正文特征判断是否为直播内容。 */
    bool isLikelyLiveHttpBody(const QString &url, const QString &contentType, const QByteArray &body, QString *detail) const;

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
    QWidget *main_ = nullptr;
    QString configPath_;

    QTimer checkTimer_;
    QTimer blinkTimer_;
    QTimer autoHideTimer_;
    QTimer pingTimeoutTimer_;
    QProcess pingProcess_;

    CheckState checkState_ = CheckState::Idle; /**< 防止周期定时器重入正在执行的异步检查。 */
    WarningKind warningKind_ = WarningKind::None;

    bool warningActive_ = false;
    bool warningVisible_ = false;
    bool lastDefaultRouteOk_ = true;

    QNetworkAccessManager *streamHttpManager_ = nullptr;
    QNetworkReply *streamHttpReply_ = nullptr;
    QTcpSocket *streamTcpSocket_ = nullptr;
    QUdpSocket *streamUdpSocket_ = nullptr;
    QString streamProbeUrl_;

    QString text_;
    QRect boxRect_;
    QFont baseFont_;

    QStringList pingTargets_;
    int pingTargetIndex_ = 0;
    int maxWidth_ = 520;
    int padX_ = 12;
    int padY_ = 16;

    int checkIntervalMs_ = 10000;       /**< 每 10 秒检查一次状态。 */
    int blinkIntervalMs_ = 60000;       /**< 告警存在时每 1 分钟闪现一次。 */
    int visibleDurationMs_ = 5000;      /**< 每次显示 5 秒。 */
    int pingTimeoutMs_ = 3000;          /**< 单个外网目标最多等待 3 秒。 */
    int streamProbeTimeoutMs_ = 3500;   /**< 单次直播源探测最多等待 3.5 秒。 */
};

#endif // NETWORK_STATUS_MONITOR_H
