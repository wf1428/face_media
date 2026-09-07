/**
 * @file multimediademo.cpp
 * @brief 协调信号板显示、本地轮播、直播、音量、截图和 MQTT 状态。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "multimediademo.h"
#include "ui_multimediademo.h"
#include <QGraphicsDropShadowEffect>
#include <QPushButton>
#include <QDateTimeEdit>
#include <QSpacerItem>
#include <QEventLoop>
#include <QDebug>
#include <QDir>
#include <QWidget>
#include <QPalette>
#include <QTimer>
#include <QImageReader>
#include <QSettings>
#include <QFileInfo>
#include <QFile>
#include <QThread>
#include <QUrl>
#include <QTcpSocket>
#include <QAbstractSocket>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QVariant>
#include <QProcess>
#include <QRegExp>
#include <QUdpSocket>
#include <QHostAddress>
#include <QStringList>
#include <QVector>
#include <cstdlib>
#include "platform/rk3566_platform.h"

/** @return 字节是可显示的大写字母楼层编码时返回 true。 */
static inline bool isFloorUpperLetterAscii(char ch)
{
    return ch >= 'A' && ch <= 'Z';
}

namespace {

/** 持续探测失败期间，同一直播地址每小时最多打印一次探测状态。 */
constexpr qint64 kLiveProbeFailureLogIntervalMs = 60LL * 60LL * 1000LL;

/** @return 平台统一的网络配置文件路径。 */
static QString netCfgPath() { return Rk3566Platform::netConfigPath(); }

/** @brief 将配置中的播放模式兼容值归一化为 NONE、LIVE 或 RECORDED。 */
static QString normalizePlaybackModeForCfg(const QString &raw)
{
    const QString v = raw.trimmed().toUpper();
    if (v == QStringLiteral("LIVE")) {
        return QStringLiteral("LIVE");
    }
    if (v == QStringLiteral("RECORDED") || v == QStringLiteral("RECORED")) {
        return QStringLiteral("RECORDED");
    }
    if (v == QStringLiteral("NONE")) {
        return QStringLiteral("NONE");
    }
    return QString();
}


/** @brief 读取网卡 carrier；valid 区分“链路断开”和“节点不可读”。 */
static bool readNetworkCarrierForLiveRetry(const QString &ifaceName, bool *valid)
{
    if (valid) {
        *valid = false;
    }

    const QString carrierPath = QStringLiteral("/sys/class/net/%1/carrier").arg(ifaceName);
    QFile f(carrierPath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QString v = QString::fromLatin1(f.readAll()).trimmed();
    if (v.isEmpty()) {
        return false;
    }

    if (valid) {
        *valid = true;
    }
    return v == QStringLiteral("1");
}

/** @return 直播重试前确认默认有线网卡链路可用时返回 true。 */
static bool isWiredLinkUpForLiveRetry()
{
    // T113/Tina 常见是 eth0，部分系统可能叫 end0。
    // 优先检查明确的有线网口；如果这些 carrier 文件不存在，再退回扫描所有非 lo 网口。
    const QStringList preferredIfaces = { QStringLiteral("eth0"), QStringLiteral("end0") };
    for (const QString &iface : preferredIfaces) {
        bool valid = false;
        const bool up = readNetworkCarrierForLiveRetry(iface, &valid);
        if (valid) {
            return up;
        }
    }

    QDir netDir(QStringLiteral("/sys/class/net"));
    const QStringList ifaces = netDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    bool sawCarrier = false;
    for (const QString &iface : ifaces) {
        if (iface == QStringLiteral("lo")) {
            continue;
        }

        bool valid = false;
        const bool up = readNetworkCarrierForLiveRetry(iface, &valid);
        if (!valid) {
            continue;
        }

        sawCarrier = true;
        if (up) {
            return true;
        }
    }

    // 没有 carrier 能力时不改变旧逻辑，避免误杀非标准网络环境。
    return !sawCarrier;
}

/** @return 当前直播重试应按裸 UDP 或 SRT 数据到达方式探测时返回 true。 */
static bool isRawUdpOrSrtLiveKindForLiveRetry(const QString &scheme, const QString &kind)
{
    const QString s = scheme.trimmed().toLower();
    const QString k = kind.trimmed().toLower();
    return s == QStringLiteral("udp") || s == QStringLiteral("srt") ||
           k == QStringLiteral("udp") || k == QStringLiteral("srt") ||
           k == QStringLiteral("multicast") || k.contains(QStringLiteral("multicast"));
}

/** @return 当前直播重试应按裸 UDP 数据流探测时返回 true。 */
static bool isRawUdpLiveKindForLiveRetry(const QString &scheme, const QString &kind)
{
    const QString s = scheme.trimmed().toLower();
    const QString k = kind.trimmed().toLower();
    return s == QStringLiteral("udp") ||
           k == QStringLiteral("udp") ||
           k == QStringLiteral("multicast") ||
           k.contains(QStringLiteral("multicast"));
}

/** @return 文本地址属于 IPv4 组播范围时返回 true。 */
static bool isMulticastIpv4ForLiveRetry(const QString &ipText);

/** @brief 直播重试流程解析出的 UDP 单播或组播目标。 */
struct UdpLiveProbeTargetForLiveRetry
{
    QString host;          /**< 目标 IPv4 地址或主机名。 */
    quint16 port = 0;      /**< UDP 端口；0 表示未解析。 */
    bool ok = false;       /**< 地址与端口均有效时为 true。 */
    bool multicast = false; /**< 目标属于 IPv4 组播范围时为 true。 */
};

/** @brief 从直播 URL 解析 UDP 地址、端口及组播属性。 */
static UdpLiveProbeTargetForLiveRetry parseUdpLiveProbeTargetForLiveRetry(const QString &url)
{
    UdpLiveProbeTargetForLiveRetry target;

    const QString text = url.trimmed();
    const QUrl u(text);

    QString host = u.host().trimmed();
    int port = u.port();

    // 兼容常见写法：udp://239.1.1.1:5000、udp://@239.1.1.1:5000、
    // udp://0.0.0.0:5000。部分 Qt 版本对 udp://@host:port 的解析不稳定，
    // 所以这里再做一次文本兜底解析。
    if (host.isEmpty() || port <= 0) {
        QRegExp rx(QStringLiteral("udp://@?([^/:\\?]+):(\\d+)"));
        if (rx.indexIn(text) >= 0) {
            if (host.isEmpty()) {
                host = rx.cap(1).trimmed();
            }
            if (port <= 0) {
                bool ok = false;
                const int parsedPort = rx.cap(2).toInt(&ok);
                if (ok) {
                    port = parsedPort;
                }
            }
        }
    }

    if (port <= 0 || port > 65535) {
        return target;
    }

    if (host.isEmpty()) {
        host = QStringLiteral("0.0.0.0");
    }

    target.host = host;
    target.port = static_cast<quint16>(port);
    target.ok = true;
    target.multicast = isMulticastIpv4ForLiveRetry(host);
    return target;
}

/** @return 文本地址属于 224.0.0.0/4 时返回 true。 */
static bool isMulticastIpv4ForLiveRetry(const QString &ipText)
{
    QString ip = ipText.trimmed();
    const int slash = ip.indexOf('/');
    if (slash >= 0) {
        ip = ip.left(slash);
    }

    const QStringList parts = ip.split('.');
    if (parts.size() != 4) {
        return false;
    }

    bool ok = false;
    const int first = parts.at(0).toInt(&ok);
    return ok && first >= 224 && first <= 239;
}

/** @return SDP 连接字段声明组播地址时返回 true。 */
static bool sdpLooksLikeMulticastForLiveRetry(const QString &sdp)
{
    const QStringList lines = sdp.split('\n', QString::SkipEmptyParts);
    for (QString line : lines) {
        line = line.trimmed();
        if (!line.startsWith(QStringLiteral("c=IN IP4"))) {
            continue;
        }

        const QStringList fields = line.split(QRegExp("\\s+"), QString::SkipEmptyParts);
        if (fields.size() >= 3 && isMulticastIpv4ForLiveRetry(fields.at(2))) {
            return true;
        }
    }
    return false;
}

/** @brief 按 URL scheme 和扩展名推断写入配置的直播类型。 */
static QString detectLiveKindFromUrlForCfg(const QString &url)
{
    const QString lower = url.trimmed().toLower();
    if (lower.startsWith(QStringLiteral("rtsp://"))) {
        return QStringLiteral("rtsp");
    }
    if (lower.startsWith(QStringLiteral("rtmp://")) || lower.startsWith(QStringLiteral("rtmps://"))) {
        return QStringLiteral("rtmp");
    }
    if (lower.startsWith(QStringLiteral("srt://"))) {
        return QStringLiteral("srt");
    }
    if (lower.startsWith(QStringLiteral("udp://"))) {
        return QStringLiteral("udp");
    }
    if (lower.startsWith(QStringLiteral("http://")) || lower.startsWith(QStringLiteral("https://"))) {
        if (lower.contains(QStringLiteral(".m3u8"))) {
            return QStringLiteral("hls");
        }
        if (lower.contains(QStringLiteral(".ts"))) {
            return QStringLiteral("http-ts");
        }
        return QStringLiteral("live");
    }
    return QStringLiteral("live");
}

/** @brief 由功能开关选出的当前 MQTT 路由摘要。 */
struct ActiveMqttRouteInfo
{
    QString routeName; /**< online_v1 或 online_v2 路由名称。 */
    QString host;      /**< 当前 Broker 主机。 */
    QString subTopics; /**< 当前路由订阅主题文本。 */
    bool ok = false;   /**< 功能开关唯一且配置可用时为 true。 */
};

/** @brief 根据互斥功能开关解析当前 MQTT 路由和主题。 */
static ActiveMqttRouteInfo resolveActiveMqttRoute(QSettings &settings)
{
    ActiveMqttRouteInfo info;

    const bool offlineV1 = settings.value("feature/offline_v1", false).toBool();
    const bool onlineV1  = settings.value("feature/online_v1", false).toBool();
    const bool onlineV2  = settings.value("feature/online_v2", false).toBool();

    auto readText = [&](const QString &key) {
        return settings.value(key).toString().trimmed();
    };

    const int enabledCount =
            (offlineV1 ? 1 : 0) +
            (onlineV1  ? 1 : 0) +
            (onlineV2  ? 1 : 0);

    if (enabledCount > 1) {
        info.routeName = QStringLiteral("invalid");
        info.ok = false;
        return info;
    }

    if (offlineV1) {
        info.routeName = QStringLiteral("offline_v1");
        info.ok = false;
        return info;
    }

    if (onlineV1) {
        info.routeName = QStringLiteral("online_v1");
        info.host = readText("mqtt/host_online_v1");
        info.subTopics = readText("mqtt/sub_topics_online_v1");
        info.ok = !info.host.isEmpty() || !info.subTopics.isEmpty();
        return info;
    }

    if (onlineV2) {
        info.routeName = QStringLiteral("online_v2");
        info.host = readText("mqtt/host_online_v2");
        info.subTopics = readText("mqtt/sub_topics_online_v2");
        info.ok = !info.host.isEmpty() || !info.subTopics.isEmpty();
        return info;
    }

    info.routeName = QStringLiteral("none");
    info.ok = false;
    return info;
}

/** @brief 从 RTSP 状态行解析三位状态码。 */
static int parseRtspStatusCodeForLiveRetry(const QByteArray &statusLine)
{
    const QList<QByteArray> parts = statusLine.trimmed().split(' ');
    if (parts.size() < 2) {
        return 0;
    }

    bool ok = false;
    const int code = parts.at(1).toInt(&ok);
    return ok ? code : 0;
}

/** @brief 从 RTSP 头部解析 Content-Length。 */
static int parseRtspContentLengthForLiveRetry(const QByteArray &header)
{
    const QList<QByteArray> lines = header.split('\n');
    for (QByteArray line : lines) {
        line = line.trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }

        const QByteArray key = line.left(colon).trimmed().toLower();
        if (key != "content-length") {
            continue;
        }

        bool ok = false;
        const int len = line.mid(colon + 1).trimmed().toInt(&ok);
        return ok && len >= 0 ? len : 0;
    }

    return 0;
}

/** @brief 直播重试流程从 TCP 缓冲区解析出的 RTSP 响应。 */
struct RtspProbeResponse
{
    bool complete = false;  /**< 头部和 Content-Length 正文均已收齐。 */
    int statusCode = 0;     /**< RTSP 三位状态码；0 表示未解析。 */
    QString statusLine;     /**< 原始 RTSP 状态行。 */
    QByteArray body;        /**< SDP 或其他响应正文。 */
    int consumedBytes = 0;  /**< 当前响应在累计缓冲区中占用的字节数。 */
};

/** @brief 从累计 TCP 缓冲区提取一个完整 RTSP 响应。 */
static RtspProbeResponse parseRtspResponseForLiveRetry(const QByteArray &buffer)
{
    RtspProbeResponse resp;

    int headerEnd = buffer.indexOf("\r\n\r\n");
    int sepLen = 4;
    if (headerEnd < 0) {
        headerEnd = buffer.indexOf("\n\n");
        sepLen = 2;
    }
    if (headerEnd < 0) {
        return resp;
    }

    const QByteArray header = buffer.left(headerEnd);
    const int firstLineEnd = header.indexOf('\n');
    const QByteArray statusLine = (firstLineEnd >= 0 ? header.left(firstLineEnd) : header).trimmed();
    const int contentLength = parseRtspContentLengthForLiveRetry(header);

    const int totalLen = headerEnd + sepLen + contentLength;
    if (buffer.size() < totalLen) {
        return resp;
    }

    resp.complete = true;
    resp.statusCode = parseRtspStatusCodeForLiveRetry(statusLine);
    resp.statusLine = QString::fromLatin1(statusLine);
    resp.body = buffer.mid(headerEnd + sepLen, contentLength);
    resp.consumedBytes = totalLen;
    return resp;
}

/** @brief 从 SDP 中选择视频媒体段的 control 值。 */
static QString findRtspVideoControlForLiveRetry(const QByteArray &sdpBody)
{
    const QString sdp = QString::fromUtf8(sdpBody);
    const QStringList lines = sdp.split('\n', QString::SkipEmptyParts);

    bool inVideoBlock = false;
    QString sessionControl;

    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }

        if (line.startsWith(QStringLiteral("m="))) {
            inVideoBlock = line.startsWith(QStringLiteral("m=video"));
            continue;
        }

        if (!line.startsWith(QStringLiteral("a=control:"))) {
            continue;
        }

        const QString control = line.mid(QStringLiteral("a=control:").size()).trimmed();
        if (control.isEmpty()) {
            continue;
        }

        if (inVideoBlock) {
            return control;
        }

        // session 级 control 只能作为兜底，不能单独证明有 video track。
        if (sessionControl.isEmpty()) {
            sessionControl = control;
        }
    }

    return QString();
}

/** @brief 将 SDP control 字段解析为绝对 RTSP 地址。 */
static QString buildRtspControlUrlForLiveRetry(const QUrl &baseUrl, const QString &control)
{
    const QString c = control.trimmed();
    if (c.isEmpty() || c == QStringLiteral("*")) {
        return baseUrl.toString(QUrl::RemoveQuery | QUrl::RemoveFragment);
    }

    const QString lower = c.toLower();
    if (lower.startsWith(QStringLiteral("rtsp://")) ||
        lower.startsWith(QStringLiteral("rtsps://"))) {
        return c;
    }

    if (c.startsWith('/')) {
        QUrl u = baseUrl;
        u.setPath(c);
        u.setQuery(QString());
        u.setFragment(QString());
        return u.toString();
    }

    QString base = baseUrl.toString(QUrl::RemoveQuery | QUrl::RemoveFragment);
    if (!base.endsWith('/')) {
        base += '/';
    }
    return base + c;
}

/** @brief 发送用于直播源预检的 RTSP DESCRIBE 请求。 */
static void sendRtspDescribeForLiveRetry(QTcpSocket *sock, const QString &url)
{
    QByteArray req;
    req += "DESCRIBE ";
    req += QUrl(url).toEncoded(QUrl::FullyEncoded);
    req += " RTSP/1.0\r\n";
    req += "CSeq: 1\r\n";
    req += "Accept: application/sdp\r\n";
    req += "User-Agent: qt_ycest-live-probe\r\n";
    req += "\r\n";
    sock->write(req);
    sock->flush();
}

/** @brief 发送用于确认媒体轨可用性的 RTSP SETUP 请求。 */
static void sendRtspSetupForLiveRetry(QTcpSocket *sock, const QString &trackUrl)
{
    QByteArray req;
    req += "SETUP ";
    req += QUrl(trackUrl).toEncoded(QUrl::FullyEncoded);
    req += " RTSP/1.0\r\n";
    req += "CSeq: 2\r\n";
    req += "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n";
    req += "User-Agent: qt_ycest-live-probe\r\n";
    req += "\r\n";
    sock->write(req);
    sock->flush();
}


/** @return HTTP 状态码表示成功时返回 true。 */
static bool isHttpSuccessStatusForLiveRetry(int statusCode)
{
    return statusCode == 200 || statusCode == 204 || statusCode == 206;
}

/** @return HTTP 状态码表示重定向时返回 true。 */
static bool isHttpRedirectStatusForLiveRetry(int statusCode)
{
    return statusCode == 301 || statusCode == 302 || statusCode == 303 ||
           statusCode == 307 || statusCode == 308;
}

/** @return 响应正文更像 HTML 错误页而非媒体数据时返回 true。 */
static bool isProbablyHtmlBodyForLiveRetry(const QByteArray &body)
{
    const QByteArray trimmed = body.left(256).trimmed().toLower();
    return trimmed.startsWith("<!doctype html") ||
           trimmed.startsWith("<html") ||
           trimmed.contains("<html");
}

/** @return 正文包含 HLS 播放列表标记时返回 true。 */
static bool isLikelyHlsPlaylistForLiveRetry(const QByteArray &body)
{
    const QByteArray b = body.left(8192).trimmed();
    if (!b.startsWith("#EXTM3U")) {
        return false;
    }

    return b.contains("#EXT-X-STREAM-INF") ||
           b.contains("#EXT-X-TARGETDURATION") ||
           b.contains("#EXTINF") ||
           b.contains(".ts") ||
           b.contains(".m4s");
}

/** @return 数据具有 MPEG-TS 同步字节特征时返回 true。 */
static bool isLikelyTsBodyForLiveRetry(const QByteArray &body)
{
    if (body.size() < 188) {
        return false;
    }

    // MPEG-TS 包长 188，正常情况下同步字节是 0x47。
    // Range/HTTP 分片不一定从包边界开始，因此只在前几个偏移内找连续同步字节。
    for (int offset = 0; offset < 188 && offset < body.size(); ++offset) {
        if (static_cast<unsigned char>(body.at(offset)) != 0x47) {
            continue;
        }
        const int next = offset + 188;
        if (next < body.size() && static_cast<unsigned char>(body.at(next)) == 0x47) {
            return true;
        }
    }
    return false;
}

/** @return 数据具有 FLV 文件头时返回 true。 */
static bool isLikelyFlvBodyForLiveRetry(const QByteArray &body)
{
    return body.size() >= 3 && body.left(3) == "FLV";
}

/** @return 数据具有 MP4 box 文件头时返回 true。 */
static bool isLikelyMp4BodyForLiveRetry(const QByteArray &body)
{
    return body.size() >= 12 && body.mid(4, 4) == "ftyp";
}

/** @brief 综合 URL、Content-Type 和正文魔数判断 HTTP 直播内容。 */
static bool isLikelyLiveHttpBodyForLiveRetry(const QString &url,
                                             const QString &contentType,
                                             const QByteArray &body,
                                             QString *detail)
{
    const QString lowerUrl = url.toLower();
    const QString ct = contentType.toLower();

    if (lowerUrl.contains(QStringLiteral(".m3u8")) ||
        ct.contains(QStringLiteral("application/vnd.apple.mpegurl")) ||
        ct.contains(QStringLiteral("application/x-mpegurl")) ||
        ct.contains(QStringLiteral("mpegurl"))) {
        if (isLikelyHlsPlaylistForLiveRetry(body)) {
            if (detail) *detail = QStringLiteral("HLS playlist ok");
            return true;
        }
        if (detail) *detail = QStringLiteral("HLS playlist invalid or empty");
        return false;
    }

    if (lowerUrl.contains(QStringLiteral(".ts")) || ct.contains(QStringLiteral("mp2t"))) {
        if (isLikelyTsBodyForLiveRetry(body) || ct.contains(QStringLiteral("mp2t"))) {
            if (detail) *detail = QStringLiteral("HTTP-TS ok");
            return true;
        }
        if (detail) *detail = QStringLiteral("HTTP-TS body invalid");
        return false;
    }

    if (lowerUrl.contains(QStringLiteral(".flv")) || ct.contains(QStringLiteral("flv"))) {
        if (isLikelyFlvBodyForLiveRetry(body) || ct.contains(QStringLiteral("flv"))) {
            if (detail) *detail = QStringLiteral("HTTP-FLV ok");
            return true;
        }
        if (detail) *detail = QStringLiteral("HTTP-FLV body invalid");
        return false;
    }

    if (lowerUrl.contains(QStringLiteral(".mp4")) || ct.contains(QStringLiteral("mp4"))) {
        if (isLikelyMp4BodyForLiveRetry(body) || ct.contains(QStringLiteral("mp4"))) {
            if (detail) *detail = QStringLiteral("HTTP-MP4 ok");
            return true;
        }
        if (detail) *detail = QStringLiteral("HTTP-MP4 body invalid");
        return false;
    }

    // 未带典型扩展名的 HTTP 直播，例如 /live、/stream。
    // 不能只看 200，否则普通网页也会误切 LIVE；至少要求不是 HTML，且 content-type 或 body 看起来像媒体。
    if (isProbablyHtmlBodyForLiveRetry(body)) {
        if (detail) *detail = QStringLiteral("HTTP body is html, not media stream");
        return false;
    }

    if (ct.startsWith(QStringLiteral("video/")) ||
        ct.startsWith(QStringLiteral("audio/")) ||
        ct.contains(QStringLiteral("octet-stream")) ||
        ct.contains(QStringLiteral("mpegurl")) ||
        isLikelyTsBodyForLiveRetry(body) ||
        isLikelyFlvBodyForLiveRetry(body) ||
        isLikelyMp4BodyForLiveRetry(body)) {
        if (detail) *detail = QStringLiteral("generic HTTP media ok");
        return true;
    }

    if (detail) *detail = QStringLiteral("HTTP response is not recognized as media stream") +
                          QStringLiteral(" content-type=") + contentType;
    return false;
}


} // namespace



MultimediaDemo::MultimediaDemo(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::MultimediaDemo),
    times(0),
    dir_status(LE),
    state_status(NORMAL),
    RunMode(false),

    Flag1_en(true),
    Flag2_en(true),
    DateDisplay_en(true),
    WeekDisplay_en(true),
    TimeDisplay_en(false),
    Diretion_en(false),
    backPic_en(true),
    bg01Pic_en(true),
    Logo1_en(false),
    Logo2_en(false),
    Logo3_en(false),
    Logo4_en(false),

    DisPlayPicOrText(0),
    VideoPic_disp_Time(4),
    label_videoRectPicture(nullptr),
    currentVideoRectMediaIndex(-1),
    videoRectPictureTimer(nullptr),
    m_currentVideoRectIsImage(false),
    m_currentVideoRectMediaPath(QString()),

    PicDisplay_en(true),
    PicDisplay_Time(5),
    PicToFlag_En(false),
    PicDisplay_XgSpeed(500),
    isPreloading(false),
    isBuffering(false),
    bufferPercentage(0),
    maxBufferDuration(10000), // 默认缓冲10秒
    autoQualityAdjustment(true),
    currentQualityLevel(2), // 默认中等质量
    qualityCheckInterval(5000), // 默认5秒检查一次
    currentPictureIndex(0),
    pictureTimer(nullptr),
    // 空调配置默认值
    AirCondi_en(false),
    AirCondiMode_en(false),
    AirCondiTemp_en(false),
    AirCondiWind_en(false),
    AirCondiMode_x(250),
    AirCondiMode_y(12),

    AirCondiMode_x_Size(50),
    AirCondiMode_y_Size(50),
    AirCondiTemp_x(305),
    AirCondiTemp_y(12),
    AirCondiTemp_x_Size(60),
    AirCondiTemp_y_Size(50),
    AirCondiTemp_font_Size(40),
    AirCondiWind_x(375),
    AirCondiWind_y(12),
    AirCondiWind_x_Size(50),
    AirCondiWind_y_Size(50),

    // 空调模式显示默认值
    label_airCondiMode(nullptr),
    airCondiModeTimer(nullptr),
    currentAirCondiMode(0),

    // 空调风速显示默认值
    label_airCondiWindSpeed(nullptr),
    airCondiWindSpeedTimer(nullptr),
    currentAirCondiWindSpeed(0),

    // 空调温度显示默认值
    label_airCondiTemp(nullptr),
    airCondiTempTimer(nullptr),
    currentAirCondiTemp(25),
    tempDirection(1), // 默认温度增加
    airCondiTempColor("white"), // 默认白色

    // 广播配置默认值
    BroadCast_en(false),
    BroadCast_x(430),
    BroadCast_y(12),
    BroadCast_x_Size(50),
    BroadCast_y_Size(50),
    label_broadCast(nullptr),

    // 可视对讲配置默认值
    VoiceTalk_en(false),
    VoiceTalk_x(570),
    VoiceTalk_y(12),
    VoiceTalk_x_Size(50),
    VoiceTalk_y_Size(50)

{
    LOG_INIT("MultimediaDemo构造函数开始执行");

    // 注意：QImageReader没有静态的setIgnoreWarning方法，需要在每个实例中设置
    // 我们已经在loadPixmapWithIgnoreWarnings函数中为每个QImageReader实例设置了忽略警告选项

    ui->setupUi(static_cast<QWidget*>(this));
    LOG_INIT("UI初始化完成");

    // initializeComponents() 早于配置文件读取，先使用与 videoRect 相同的
    // 默认值，避免用未初始化坐标创建 X11 原生视频子窗口。读取配置后会
    // 再把实际区域传给 GstPlayerWidget。
    VideoDisplay_en = true;
    VideoDisplay_x = 16;
    VideoDisplay_y = 163;
    VideoDisplay_x_Size = 613;
    VideoDisplay_y_Size = 529;

    initializeComponents();

    initPasswordErrorToast();

    m_downloadToast = new DownloadProgressToast(this);
    m_downloadToast->hideToast();

    readConfigurationFromFile();
    simulateBufferData();

    //初始化串口 client
    sigClient = new SignalBoardClient(Rk3566Platform::signalBoardDevice(), 9600, this);

    connect(sigClient, &SignalBoardClient::stateReceived,
            this, &MultimediaDemo::onSignalBoardState, Qt::QueuedConnection);

    sigClient->start();

    // 初始化音量条
    m_ampCtrl = new AmpVolumeController(this);

    // 读取上次保存的音量和amp_flag，并按文件内容恢复一次硬件状态
    int v = m_ampCtrl->loadVolumeFromFile();

    // ALSA 硬件音量与 GStreamer playbin 软件音量保持同步。即使 RK3566
    // 声卡没有 simple-mixer 控件，播放器音量仍然能够生效。
    connect(m_ampCtrl, &AmpVolumeController::volumeApplied, this, [this](int percent) {
        const float value01 = qBound(0, percent, 100) / 100.0f;
        if (mediaPlayer) mediaPlayer->setVolume(value01);
        if (nextVideoPlayer) nextVideoPlayer->setVolume(value01);
        if (m_liveProbePlayer) m_liveProbePlayer->setVolume(0.0f);
    });
    if (mediaPlayer) mediaPlayer->setVolume(v / 100.0f);
    if (nextVideoPlayer) nextVideoPlayer->setVolume(v / 100.0f);
    if (m_liveProbePlayer) m_liveProbePlayer->setVolume(0.0f);
    m_ampCtrl->setVolume(v);

    // 建面板
    m_volPanel = new VolumePanel(m_ampCtrl, this);
    m_volPanel->setCurrentVolume(v);

    connect(m_volPanel, &VolumePanel::manualVolumeChanged,
            this, [this](int v, const QString &reason) {
                emit currentPlayVolumeChanged(v);   // 现有链路
                emit localManualVolumeChanged(v, reason);   // 给 MQTT 主动上报
            });

    // 初始化网络信息面板
    m_netInfoPanel = new NetInfoPanel(this);
    readNetConfiguration();
    m_netInfoPanel->setInfo(m_netIp, m_mqttClientId, m_mqttRouteName, m_mqttHost, m_mqttSubTopics, m_streamUrl);

    // 开机恢复音量后，主动发一次
    QTimer::singleShot(0, this, [this, v]() {
        emit currentPlayVolumeChanged(v);
    });

    // 设定“固定区域”
    m_volumeRect = QRect(VideoDisplay_x, VideoDisplay_y, VideoDisplay_x_Size, VideoDisplay_y_Size);

    initializeConfiguration();
    initializeTimers();
    m_signalUiReady = true;
    onTimeout(); // 显示串口启动后可能已经收到的最新完整帧

    // 开机只在 online_v1/online_v2 模式下读取 [playback]，没有收到过云端播放指令则保持原逻辑。
    QTimer::singleShot(1500, this, &MultimediaDemo::restorePlaybackStateFromNetCfg);

    // 根据背景图片配置设置窗体大小
    this->setFixedSize(backPic_x_Size, backPic_y_Size);
    LOG_INIT("窗体大小设置为: " << backPic_x_Size << "x" << backPic_y_Size);

    LOG_INIT("MultimediaDemo构造函数执行完成");
}


/** @brief 创建密码错误的非模态提示标签和定时器。 */
void MultimediaDemo::initPasswordErrorToast()
{
    if (m_passwordToastLabel) {
        return;
    }

    m_passwordToastLabel = new QLabel(this);
    m_passwordToastLabel->setObjectName("passwordErrorToast");
    m_passwordToastLabel->setAlignment(Qt::AlignCenter);
    m_passwordToastLabel->setWordWrap(true);
    m_passwordToastLabel->setVisible(false);
    m_passwordToastLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    QFont f = m_passwordToastLabel->font();
    f.setPointSize(18);
    f.setBold(true);
    m_passwordToastLabel->setFont(f);

    m_passwordToastLabel->setStyleSheet(
        "QLabel#passwordErrorToast {"
        " background-color: rgba(220, 38, 38, 220);"
        " color: white;"
        " border: 1px solid rgba(248, 113, 113, 180);"
        " border-radius: 22px;"
        " padding: 10px 18px;"
        "}"
    );

    auto *shadow = new QGraphicsDropShadowEffect(m_passwordToastLabel);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 8);
    shadow->setColor(QColor(0, 0, 0, 90));
    m_passwordToastLabel->setGraphicsEffect(shadow);

    m_passwordToastTimer = new QTimer(this);
    m_passwordToastTimer->setSingleShot(true);

    connect(m_passwordToastTimer, &QTimer::timeout, this, [this]() {
        if (m_passwordToastLabel) {
            m_passwordToastLabel->hide();
        }
    });
}

/** @brief 显示指定时长的密码错误提示。 */
void MultimediaDemo::showPasswordErrorToast(const QString &text, int ms)
{
    if (!m_passwordToastLabel) {
        initPasswordErrorToast();
    }
    if (!m_passwordToastLabel) {
        return;
    }

    m_passwordToastLabel->setText(text);

    const int margin = 40;
    const int maxW = qMin(width() - margin * 2, 360);
    const int textW = qMax(240, maxW);

    m_passwordToastLabel->setFixedWidth(textW);

    QFontMetrics fm(m_passwordToastLabel->font());
    QRect br = fm.boundingRect(
        QRect(0, 0, textW - 40, 1000),
        Qt::AlignCenter | Qt::TextWordWrap,
        text
    );

    const int h = qMax(58, br.height() + 24);
    m_passwordToastLabel->setFixedHeight(h);

    const int x = (width() - m_passwordToastLabel->width()) / 2;
    const int y = qMax(40, (height() - m_passwordToastLabel->height()) / 2);

    m_passwordToastLabel->move(x, y);
    m_passwordToastLabel->show();
    m_passwordToastLabel->raise();

    if (m_passwordToastTimer) {
        m_passwordToastTimer->start(ms);
    }
}



/** @brief 临时切往其他模块时停止解码资源，同时保留返回后需要恢复的 LIVE 目标。 */
void MultimediaDemo::suspendPlaybackForModuleSwitch()
{
    if (m_modulePlaybackSuspended) return;

    m_modulePlaybackSuspended = true;
    // “正在前台播放 LIVE”和“仍期望 LIVE、但已回退本地等待重试”
    // 是两个独立状态。模块切换时两者都要保留 LIVE 业务目标，
    // 但只有前者适合在返回界面时直接重建 LIVE 播放。
    m_resumeLiveAfterModuleSwitch = m_liveDesired &&
                                    !m_desiredLiveUrl.trimmed().isEmpty();
    m_resumeLiveDirectlyAfterModuleSwitch = m_resumeLiveAfterModuleSwitch &&
                                            m_isLiveMode;
    m_localPlaybackEnabled = false;
    stopEmptyVideoListWatch();
    stopLiveWatchdog();
    stopLiveRetry();

    if (m_liveProbePlayer) {
        m_liveProbePlayer->stop();
    }
    m_liveProbeRunning = false;

    disconnect(mediaPlayer, &GstPlayerWidget::videoFinished,
               this, &MultimediaDemo::playNextVideo);
    if (nextVideoPlayer) {
        disconnect(nextVideoPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);
    }

    if (mediaPlayer) mediaPlayer->stop();
    if (nextVideoPlayer) nextVideoPlayer->stop();
    if (videoWidget) videoWidget->hide();

    m_isLiveMode = false;
    m_liveRecovering = false;
    m_liveSwitching = false;
    m_leaving = false;

    if (m_resumeLiveDirectlyAfterModuleSwitch) {
        LOG_VIDEO("模块切换暂停：已保留LIVE目标，返回后恢复:" << m_desiredLiveUrl);
    } else if (m_resumeLiveAfterModuleSwitch) {
        LOG_VIDEO("模块切换暂停：已保留LIVE重试目标，返回后恢复本地轮播并继续探测:"
                  << m_desiredLiveUrl);
    } else {
        LOG_VIDEO("模块切换暂停：返回后恢复本地轮播");
    }
}

/** @brief 返回多媒体模块时恢复暂停前的 LIVE 目标，否则沿用原本的本地轮播恢复。 */
void MultimediaDemo::resumePlaybackAfterModuleSwitch()
{
    m_modulePlaybackSuspended = false;
    m_localPlaybackEnabled = true;

    const bool resumeLiveTarget = m_resumeLiveAfterModuleSwitch &&
                                  m_liveDesired &&
                                  !m_desiredLiveUrl.trimmed().isEmpty();
    const bool resumeLiveDirectly = resumeLiveTarget &&
                                    m_resumeLiveDirectlyAfterModuleSwitch;
    m_resumeLiveAfterModuleSwitch = false;
    m_resumeLiveDirectlyAfterModuleSwitch = false;

    if (resumeLiveDirectly) {
        LOG_VIDEO("模块切换返回：恢复暂停前的LIVE目标:" << m_desiredLiveUrl);
        switchToLiveStream(m_desiredLiveUrl, m_desiredLiveKind);
        startLiveRetry();
        return;
    }

    if (resumeLiveTarget) {
        // 切换模块前 LIVE 已经因无帧回退到本地。返回时保持本地播放，
        // 但必须重启 MultimediaDemo 自己的重试定时器，它才能在探测成功后切回 LIVE。
        LOG_VIDEO("模块切换返回：恢复本地轮播并继续探测LIVE目标:"
                  << m_desiredLiveUrl);
        resumeLocalPlaylistFromStart();
        startLiveRetry();
        return;
    }

    resumeLocalPlaylistFromStart();
}

/** @brief 离开页面时停止全部播放器、探测和定时任务，避免后台占用解码资源。 */
void MultimediaDemo::leaveAndStopVideo()
{
    m_localPlaybackEnabled = false;
    stopEmptyVideoListWatch();

    stopLiveWatchdog();

    if (m_leaving) return;
    m_leaving = true;

    // 1) 断开 finished，避免 stop 过程中触发 playNextVideo（尤其是 QueuedConnection）
    disconnect(mediaPlayer, &GstPlayerWidget::videoFinished,
               this, &MultimediaDemo::playNextVideo);
    if (nextVideoPlayer) {
        disconnect(nextVideoPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);
    }

    // 2) 先断显示输出（非常重要：切页时立刻停止 UI 更新/绘制链路）
//    if (mediaPlayer)  mediaPlayer->setVideoOutput(nullptr);
//    if (nextVideoPlayer) nextVideoPlayer->setVideoOutput(nullptr);

    // 2) 先停播放器，不要先 setVideoOutput(nullptr)
    if (mediaPlayer) {
        mediaPlayer->stop();
    }
    if (nextVideoPlayer) {
        nextVideoPlayer->stop();
    }


    // 3) 隐藏视频窗口，避免还在 repaint
    if (videoWidget) videoWidget->hide();

    // 4) 用 singleShot(0) 异步 stop：不要把 stop 放在“切页那条调用栈”里
    QTimer::singleShot(0, this, [this]() {
        if (mediaPlayer) mediaPlayer->stop();           // GstPlayerWidget::stop 会停止管线并清空待显示帧。
        if (nextVideoPlayer) nextVideoPlayer->stop();

        m_leaving = false;

        // 页面离开时，彻底取消直播目标
        stopDesiredLive();
        m_isLiveMode = false;
    });
}


// 刷新播放列表
void MultimediaDemo::reloadVideoFileList()
{
    const QString videoDirPath = Rk3566Platform::videoDir();
    QDir videoDir(videoDirPath);

    QString currentPlayingFile;
    if (!videoFileList.isEmpty() &&
        currentVideoIndex >= 0 &&
        currentVideoIndex < videoFileList.size()) {
        currentPlayingFile = videoFileList.at(currentVideoIndex);
    }

    videoFileList.clear();

    if (!videoDir.exists()) {
        LOG_VIDEO("警告: 视频目录不存在:" << videoDirPath);
        currentVideoIndex = -1;
        return;
    }

    QStringList filters;
    filters << "*.mp4" << "*.MP4"
            << "*.avi" << "*.AVI"
            << "*.mov" << "*.MOV"
            << "*.mkv" << "*.MKV"
            << "*.wmv" << "*.WMV"
            << "*.flv" << "*.FLV"
            << "*.ts"  << "*.TS"
            << "*.pls" << "*.PLS"
            << "*.webm" << "*.WEBM"
            << "*.rmvb" << "*.RMVB"
            << "*.3gp" << "*.3GP";

    videoDir.setNameFilters(filters);
    videoDir.setFilter(QDir::Files | QDir::Readable);
    videoDir.setSorting(QDir::Name | QDir::IgnoreCase);

    const QStringList fileNames = videoDir.entryList();
    for (const QString &fileName : fileNames) {
        videoFileList << videoDir.absoluteFilePath(fileName);
    }

    if (!currentPlayingFile.isEmpty()) {
        int idx = videoFileList.indexOf(currentPlayingFile);
        currentVideoIndex = idx >= 0 ? idx : -1;
    } else {
        currentVideoIndex = videoFileList.isEmpty() ? -1 : 0;
    }

    LOG_VIDEO("刷新后视频列表数量:" << videoFileList.size());
    LOG_VIDEO("刷新后当前视频索引:" << currentVideoIndex);
}

/** @brief 按配置顺序重建 videoRect 视频/图片混播列表。 */
void MultimediaDemo::rebuildVideoRectMediaList()
{
    QString currentPath = m_currentVideoRectMediaPath;

    if (currentPath.isEmpty() &&
        currentVideoRectMediaIndex >= 0 &&
        currentVideoRectMediaIndex < videoRectMediaList.size()) {
        currentPath = videoRectMediaList.at(currentVideoRectMediaIndex).filePath;
    }

    videoFileList.clear();
    videoRectPictureFileList.clear();
    videoRectMediaList.clear();

    const QString mediaDirPath = Rk3566Platform::videoDir();
    QDir mediaDir(mediaDirPath);

    if (!mediaDir.exists()) {
        LOG_VIDEO("警告: videoRect 混播目录不存在:" << mediaDirPath);
        currentVideoIndex = -1;
        currentVideoRectMediaIndex = -1;
        m_currentVideoRectMediaPath.clear();
        return;
    }

    QStringList filters;

    // 视频格式
    filters << "*.mp4" << "*.MP4"
            << "*.avi" << "*.AVI"
            << "*.mov" << "*.MOV"
            << "*.mkv" << "*.MKV"
            << "*.wmv" << "*.WMV"
            << "*.flv" << "*.FLV"
            << "*.ts"  << "*.TS"
            << "*.pls" << "*.PLS"
            << "*.webm" << "*.WEBM"
            << "*.rmvb" << "*.RMVB"
            << "*.3gp" << "*.3GP";

    // 图片格式
    filters << "*.png" << "*.PNG"
            << "*.jpg" << "*.JPG"
            << "*.jpeg" << "*.JPEG"
            << "*.bmp" << "*.BMP"
            << "*.gif" << "*.GIF";

    mediaDir.setNameFilters(filters);
    mediaDir.setFilter(QDir::Files | QDir::Readable);
    mediaDir.setSorting(QDir::Name | QDir::IgnoreCase);

    const QFileInfoList infos = mediaDir.entryInfoList();

    auto isImageFile = [](const QString &suffix) -> bool {
        const QString s = suffix.toLower();
        return s == "png" ||
               s == "jpg" ||
               s == "jpeg" ||
               s == "bmp" ||
               s == "gif";
    };

    auto isVideoFile = [](const QString &suffix) -> bool {
        const QString s = suffix.toLower();
        return s == "mp4"  ||
               s == "avi"  ||
               s == "mov"  ||
               s == "mkv"  ||
               s == "wmv"  ||
               s == "flv"  ||
               s == "ts"   ||
               s == "pls"  ||
               s == "webm" ||
               s == "rmvb" ||
               s == "3gp";
    };

    for (const QFileInfo &info : infos) {
        const QString absPath = info.absoluteFilePath();
        const QString suffix = info.suffix();

        if (isVideoFile(suffix)) {
            videoFileList << absPath;
            videoRectMediaList << VideoRectMediaItem(VideoRectMedia_Video, absPath);
        } else if (isImageFile(suffix)) {
            videoRectPictureFileList << absPath;
            videoRectMediaList << VideoRectMediaItem(VideoRectMedia_Image, absPath);
        }
    }

    currentVideoIndex = videoFileList.isEmpty() ? -1 : 0;
    currentVideoRectMediaIndex = -1;

    if (!currentPath.isEmpty()) {
        for (int i = 0; i < videoRectMediaList.size(); ++i) {
            if (videoRectMediaList.at(i).filePath == currentPath) {
                currentVideoRectMediaIndex = i;
                break;
            }
        }
    }

    if (currentVideoRectMediaIndex < 0 && !videoRectMediaList.isEmpty()) {
        currentVideoRectMediaIndex = 0;
    }

    if (currentVideoRectMediaIndex >= 0 &&
        currentVideoRectMediaIndex < videoRectMediaList.size()) {
        m_currentVideoRectMediaPath = videoRectMediaList.at(currentVideoRectMediaIndex).filePath;
    } else {
        m_currentVideoRectMediaPath.clear();
    }

    LOG_VIDEO("videoRect 混播目录:" << mediaDirPath);
    LOG_VIDEO("videoRect 混播视频数量:" << videoFileList.size());
    LOG_VIDEO("videoRect 混播图片数量:" << videoRectPictureFileList.size());
    LOG_VIDEO("videoRect 混播总数量:" << videoRectMediaList.size());

    for (int i = 0; i < videoRectMediaList.size(); ++i) {
        LOG_VIDEO("videoRect 混播顺序[" << i << "]"
                  << (videoRectMediaList.at(i).type == VideoRectMedia_Video ? "视频:" : "图片:")
                  << videoRectMediaList.at(i).filePath);
    }
}

/** @brief 从混播列表移除运行期间已不存在的文件。 */
void MultimediaDemo::pruneMissingVideoRectMedia()
{
    for (int i = videoRectMediaList.size() - 1; i >= 0; --i) {
        const QString path = videoRectMediaList.at(i).filePath;

        if (!path.startsWith(":/")) {
            const QFileInfo fi(path);
            if (!fi.exists() || !fi.isFile() || !fi.isReadable()) {
                LOG_VIDEO("移除不存在的 videoRect 混播资源:" << path);

                if (i < currentVideoRectMediaIndex) {
                    currentVideoRectMediaIndex--;
                } else if (i == currentVideoRectMediaIndex) {
                    currentVideoRectMediaIndex--;
                }

                videoRectMediaList.removeAt(i);
            }
        }
    }

    if (videoRectMediaList.isEmpty()) {
        currentVideoRectMediaIndex = -1;
        currentVideoIndex = -1;
        return;
    }

    if (currentVideoRectMediaIndex < -1) {
        currentVideoRectMediaIndex = -1;
    }

    if (currentVideoRectMediaIndex >= videoRectMediaList.size()) {
        currentVideoRectMediaIndex = videoRectMediaList.size() - 1;
    }
}

/** @brief 停止 videoRect 图片计时器并按需隐藏标签。 */
void MultimediaDemo::stopVideoRectPictureDisplay(bool hideLabel)
{
    if (videoRectPictureTimer) {
        videoRectPictureTimer->stop();
    }

    if (hideLabel && label_videoRectPicture) {
        label_videoRectPicture->clear();
        label_videoRectPicture->hide();
    }

    if (hideLabel) {
        m_currentVideoRectIsImage = false;
        m_currentVideoRectImagePath.clear();
    }
}

/** @brief 按环形索引播放一个 videoRect 媒体项。 */
void MultimediaDemo::playVideoRectMediaAt(int index)
{
    if (index < 0 || index >= videoRectMediaList.size()) {
        return;
    }

    currentVideoRectMediaIndex = index;

    const VideoRectMediaItem item = videoRectMediaList.at(currentVideoRectMediaIndex);

    // 记录当前播放资源路径，后面 playNextVideo() 用它定位下一项
    m_currentVideoRectMediaPath = item.filePath;

    LOG_VIDEO("videoRect 混播准备播放 index=" << currentVideoRectMediaIndex
              << ", type=" << (item.type == VideoRectMedia_Video ? "video" : "image")
              << ", path=" << item.filePath);

    if (item.type == VideoRectMedia_Image) {
        playVideoRectImage(item.filePath);
    } else {
        playVideoRectVideo(item.filePath);
    }
}

/** @brief 让主播放器播放 videoRect 视频项。 */
void MultimediaDemo::playVideoRectVideo(const QString &filePath)
{
    if (filePath.trimmed().isEmpty()) {
        playNextVideo();
        return;
    }

    // 本地视频切文件时会 stop/setMedia/play，同一时刻不要让后台 LIVE 探测播放器抢占 GStreamer 解码资源。
    m_localPlayerBusyUntilMs = QDateTime::currentMSecsSinceEpoch() + 2000;

    stopVideoRectPictureDisplay(true);

    m_currentVideoRectIsImage = false;
    m_currentVideoRectImagePath.clear();
    m_currentVideoRectMediaPath = filePath;

    int videoIndex = videoFileList.indexOf(filePath);
    if (videoIndex < 0) {
        videoFileList << filePath;
        videoIndex = videoFileList.size() - 1;
    }
    currentVideoIndex = videoIndex;

    emit currentRecordedFileChanged(filePath);

    if (nextVideoPlayer) {
        disconnect(nextVideoPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);
        nextVideoPlayer->stop();
        nextVideoPlayer->setVideoOutput(nullptr);
        nextVideoPlayer->setPreloadMode(true);
    }

    if (videoWidget) {
        videoWidget->resize(VideoDisplay_x_Size, VideoDisplay_y_Size);
        videoWidget->move(VideoDisplay_x, VideoDisplay_y);
        videoWidget->show();
        // EGLFS 下视频是 Qt 场景中的 QOpenGLWidget。不要在每次切片时 raise，
        // 否则会把已经显示的提示条、状态标签和软件光标压到下面。
        videoWidget->update();
    }

    if (mediaPlayer) {
        disconnect(mediaPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);

        mediaPlayer->stop();
        mediaPlayer->setPreloadMode(false);
        mediaPlayer->setVideoOutput(videoWidget);

        connect(mediaPlayer, &GstPlayerWidget::videoFinished,
                this, &MultimediaDemo::playNextVideo,
                Qt::QueuedConnection);

        mediaPlayer->setMedia(filePath);
        mediaPlayer->requestDisplayRectUpdate();
        mediaPlayer->play();
    }

    LOG_VIDEO("videoRect 混播：开始播放视频:" << filePath);
}

/** @brief 隐藏视频画布并显示 videoRect 图片项。 */
void MultimediaDemo::playVideoRectImage(const QString &filePath)
{
    if (filePath.trimmed().isEmpty()) {
        playNextVideo();
        return;
    }

    if (!label_videoRectPicture) {
        playNextVideo();
        return;
    }

    if (!videoRectPictureTimer) {
        videoRectPictureTimer = new QTimer(this);
        connect(videoRectPictureTimer, &QTimer::timeout,
                this, &MultimediaDemo::playNextVideo);
    }

    if (videoRectPictureTimer) {
        videoRectPictureTimer->stop();
    }

    if (mediaPlayer) {
        disconnect(mediaPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);
        mediaPlayer->stop();
        mediaPlayer->setVideoOutput(nullptr);
        mediaPlayer->setPreloadMode(false);
    }

    if (nextVideoPlayer) {
        disconnect(nextVideoPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);
        nextVideoPlayer->stop();
        nextVideoPlayer->setVideoOutput(nullptr);
        nextVideoPlayer->setPreloadMode(true);
    }

    if (videoWidget) {
        videoWidget->hide();
    }

    const QPixmap pixmap = loadPixmapWithIgnoreWarnings(filePath);
    if (pixmap.isNull()) {
        LOG_IMAGE("videoRect 混播图片加载失败:" << filePath);

        if (currentVideoRectMediaIndex >= 0 &&
            currentVideoRectMediaIndex < videoRectMediaList.size()) {
            videoRectMediaList.removeAt(currentVideoRectMediaIndex);
            currentVideoRectMediaIndex--;
        }

        QTimer::singleShot(0, this, &MultimediaDemo::playNextVideo);
        return;
    }

    const QPixmap scaled = pixmap.scaled(VideoDisplay_x_Size,
                                        VideoDisplay_y_Size,
                                        Qt::KeepAspectRatio,
                                        Qt::SmoothTransformation);

    label_videoRectPicture->setGeometry(VideoDisplay_x,
                                        VideoDisplay_y,
                                        VideoDisplay_x_Size,
                                        VideoDisplay_y_Size);
    label_videoRectPicture->setAlignment(Qt::AlignCenter);
    label_videoRectPicture->setStyleSheet("background-color: black; border: none;");
    label_videoRectPicture->setPixmap(scaled);
    label_videoRectPicture->show();
    label_videoRectPicture->raise();

    currentVideoIndex = -1;
    m_currentVideoRectIsImage = true;
    m_currentVideoRectImagePath = filePath;
    m_currentVideoRectMediaPath = filePath;

    emit currentRecordedFileChanged(QString());

    videoRectPictureTimer->start(qMax(1, VideoPic_disp_Time) * 1000);

    LOG_IMAGE("videoRect 混播：显示图片:" << filePath);
}



/** @brief 创建主/预加载播放器并连接状态信号。 */
void MultimediaDemo::initVideoPlayer()
{
    rebuildVideoRectMediaList();

    videoWidget->resize(VideoDisplay_x_Size, VideoDisplay_y_Size);
    videoWidget->move(VideoDisplay_x, VideoDisplay_y);

    if (label_videoRectPicture) {
        label_videoRectPicture->setGeometry(VideoDisplay_x,
                                            VideoDisplay_y,
                                            VideoDisplay_x_Size,
                                            VideoDisplay_y_Size);
        label_videoRectPicture->hide();
    }

    if (!videoRectPictureTimer) {
        videoRectPictureTimer = new QTimer(this);
        connect(videoRectPictureTimer, &QTimer::timeout,
                this, &MultimediaDemo::playNextVideo);
    }

    if (videoRectMediaList.isEmpty()) {
        LOG_VIDEO("警告: videoRect 混播列表为空，启动空列表监测");

        currentVideoIndex = -1;
        currentVideoRectMediaIndex = -1;

        if (videoWidget) {
            videoWidget->hide();
        }

        stopVideoRectPictureDisplay(true);
        startEmptyVideoListWatch();
        return;
    }

    stopEmptyVideoListWatch();

    currentVideoRectMediaIndex = 0;
    m_currentVideoRectMediaPath.clear();
    playVideoRectMediaAt(currentVideoRectMediaIndex);
}

/** @brief 按列表索引切换到下一段本地视频。 */
void MultimediaDemo::playNextVideo()
{
    if (m_downloadPauseActive) return;
    if (m_leaving) return;
    if (!isVisible()) return;
    if (!m_localPlaybackEnabled) return;

    if (videoRectMediaList.isEmpty()) {
        rebuildVideoRectMediaList();
    }

    if (videoRectMediaList.isEmpty()) {
        LOG_VIDEO("videoRect 混播列表为空，无法切换下一项");
        currentVideoIndex = -1;
        currentVideoRectMediaIndex = -1;
        m_currentVideoRectMediaPath.clear();
        startEmptyVideoListWatch();
        return;
    }

    pruneMissingVideoRectMedia();

    if (videoRectMediaList.isEmpty()) {
        LOG_VIDEO("清理后 videoRect 混播列表为空，启动空列表监测");
        currentVideoIndex = -1;
        currentVideoRectMediaIndex = -1;
        m_currentVideoRectMediaPath.clear();
        startEmptyVideoListWatch();
        return;
    }

    int currentIndex = -1;

    // 优先根据当前播放路径定位
    if (!m_currentVideoRectMediaPath.isEmpty()) {
        for (int i = 0; i < videoRectMediaList.size(); ++i) {
            if (videoRectMediaList.at(i).filePath == m_currentVideoRectMediaPath) {
                currentIndex = i;
                break;
            }
        }
    }

    // 路径找不到时，再使用 currentVideoRectMediaIndex
    if (currentIndex < 0 &&
        currentVideoRectMediaIndex >= 0 &&
        currentVideoRectMediaIndex < videoRectMediaList.size()) {
        currentIndex = currentVideoRectMediaIndex;
    }

    // 仍然找不到，则从 -1 开始，下一项就是 0
    if (currentIndex < 0) {
        currentIndex = -1;
    }

    const int nextIndex = (currentIndex + 1) % videoRectMediaList.size();

    LOG_VIDEO("videoRect 混播切换下一项:"
              << " currentIndex=" << currentIndex
              << " nextIndex=" << nextIndex
              << " currentPath=" << m_currentVideoRectMediaPath
              << " nextPath=" << videoRectMediaList.at(nextIndex).filePath);

    playVideoRectMediaAt(nextIndex);
}

// 预加载下一个视频
void MultimediaDemo::preloadNextVideo()
{
    LOG_VIDEO("[preloadNextVideo] enter, currentVideoIndex=" << currentVideoIndex
              << " size=" << videoFileList.size()
              << " isPreloading=" << isPreloading);

    if (!nextVideoPlayer || videoFileList.size() < 2 || isPreloading) {
        return;
    }

    int nextIndex = (currentVideoIndex + 1) % videoFileList.size();
    QString nextVideoPath = videoFileList.at(nextIndex);

    isPreloading = true;

    // 防止重复连接导致 lambda 累积
    disconnect(nextVideoPlayer, &GstPlayerWidget::prepared, this, nullptr);
    disconnect(nextVideoPlayer, &GstPlayerWidget::errorOccured, this, nullptr);

    // ✅ 预加载完成后再把 isPreloading 置回 false
    // 需要 GstPlayerWidget 有 prepared() 信号
    connect(nextVideoPlayer, &GstPlayerWidget::prepared, this, [this]() {
        isPreloading = false;
        LOG_VIDEO("[preloadNextVideo] prepared done, next prepared=" << (nextVideoPlayer ? nextVideoPlayer->isPrepared() : false));
    }, Qt::UniqueConnection);

    // 预加载失败也要把 isPreloading 置回 false
    connect(nextVideoPlayer, &GstPlayerWidget::errorOccured, this, [this](const QString&) {
        isPreloading = false;
        LOG_VIDEO("[preloadNextVideo] preload error, isPreloading=false");
    }, Qt::UniqueConnection);


    // 停止并清空预加载播放器
    nextVideoPlayer->setPreloadMode(true);
    nextVideoPlayer->setVideoOutput(nullptr);

    nextVideoPlayer->stop();
    nextVideoPlayer->setMedia(nextVideoPath);

    // 应用与当前相同的质量设置
    switch (currentQualityLevel) {
    case 1: // 低质量
        // 低质量设置（移除了setBufferMode调用）
        break;
    case 2: // 中等质量
    case 3: // 高质量
        // 中高质量设置（移除了setBufferMode调用）
        break;
    default:
        break;
    }

    //LOG_VIDEO("开始预加载下一个视频:" << nextVideoPath);
    isPreloading = false;

}

/** @brief 下载完成后更新列表并按当前模式决定是否立即播放。 */
void MultimediaDemo::onDownloadedVideoReady(const QString& filePath)
{
    const QString absPath = QFileInfo(filePath).absoluteFilePath();
    if (absPath.isEmpty()) {
        return;
    }


    if (m_downloadPauseActive) {
        LOG_VIDEO("下载完成视频已落盘，等待下载结束后立即播放:" << absPath);
        return;
    }

    LOG_VIDEO("下载完成视频准备立即播放:" << absPath);
    playDownloadedVideoNow(absPath);
}


/** @brief 把新视频插入当前项之后，使其成为下一播放项。 */
void MultimediaDemo::insertVideoNext(const QString& filePath)
{
    if (filePath.trimmed().isEmpty()) {
        return;
    }

    const QString absPath = QFileInfo(filePath).absoluteFilePath();

    // 先刷新目录，确保列表是最新的
    reloadVideoFileList();

    // 如果列表里已经有这个文件，先移除旧位置
    int oldIndex = videoFileList.indexOf(absPath);
    if (oldIndex >= 0) {
        videoFileList.removeAt(oldIndex);

        // 如果删掉的是当前索引前面的元素，要修正 currentVideoIndex
        if (oldIndex < currentVideoIndex) {
            currentVideoIndex--;
        }
    }

    // 插到当前播放项的后一个位置
    int insertPos = currentVideoIndex + 1;
    if (insertPos < 0) insertPos = 0;
    if (insertPos > videoFileList.size()) insertPos = videoFileList.size();

    videoFileList.insert(insertPos, absPath);

    // 插入后，旧的预加载内容已经不可信，清掉
    if (nextVideoPlayer) {
        nextVideoPlayer->stop();
        nextVideoPlayer->setVideoOutput(nullptr);
        nextVideoPlayer->setPreloadMode(true);
    }
    isPreloading = false;

    LOG_VIDEO("已插入到下一个播放位置:" << absPath);
    LOG_VIDEO("插入位置:" << insertPos);
    LOG_VIDEO("当前播放索引:" << currentVideoIndex);

    // 同步插入到 videoRect 混播列表中，保证下载视频可以进入混播序列
    VideoRectMediaItem item(VideoRectMedia_Video, absPath);

    for (int i = videoRectMediaList.size() - 1; i >= 0; --i) {
        if (videoRectMediaList.at(i).filePath == absPath) {
            if (i < currentVideoRectMediaIndex) {
                currentVideoRectMediaIndex--;
            } else if (i == currentVideoRectMediaIndex) {
                currentVideoRectMediaIndex--;
            }

            videoRectMediaList.removeAt(i);
        }
    }

    int mediaInsertPos = currentVideoRectMediaIndex + 1;
    if (mediaInsertPos < 0) {
        mediaInsertPos = 0;
    }
    if (mediaInsertPos > videoRectMediaList.size()) {
        mediaInsertPos = videoRectMediaList.size();
    }

    videoRectMediaList.insert(mediaInsertPos, item);

    LOG_VIDEO("已插入到 videoRect 混播下一个位置:" << absPath);
}


/** @brief 生成本地播放启动序号，用于识别异步回调所属启动。 */
quint64 MultimediaDemo::issueLocalPlaybackStartSerial()
{
    return ++m_localPlaybackStartSerial;
}


/** @brief 递增序号，使所有在途本地播放启动回调失效。 */
void MultimediaDemo::invalidatePendingLocalPlaybackStarts()
{
    ++m_localPlaybackStartSerial;
}


// 下载时停止播放
void MultimediaDemo::pauseLocalPlaybackForDownload()
{
    if (m_downloadPauseActive) {
        return;
    }

    m_downloadPauseActive = true;
    m_localPlaybackEnabled = false;
    m_downloadResumeLocalAfterFinish = (!m_isLiveMode && !m_liveDesired);
    invalidatePendingLocalPlaybackStarts();
    stopEmptyVideoListWatch();
    stopLiveWatchdog();
    isPreloading = false;

    if (nextVideoPlayer) {
        disconnect(nextVideoPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);
        nextVideoPlayer->stop();
        nextVideoPlayer->setVideoOutput(nullptr);
        nextVideoPlayer->setPreloadMode(true);
    }

    if (mediaPlayer) {
        disconnect(mediaPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);

        if (!m_currentVideoRectIsImage) {
            mediaPlayer->pause();
            mediaPlayer->setPreloadMode(false);
            mediaPlayer->setVideoOutput(videoWidget);
        }
    }

    if (m_currentVideoRectIsImage) {
        // 当前是图片播放时，暂停图片切换，但保留当前图片画面
        if (videoRectPictureTimer) {
            videoRectPictureTimer->stop();
        }

        if (videoWidget) {
            videoWidget->hide();
        }

        if (label_videoRectPicture) {
            label_videoRectPicture->show();
            label_videoRectPicture->raise();
        }
    } else {
        if (videoWidget) {
            videoWidget->show();
            videoWidget->update();
        }
    }

    LOG_VIDEO("下载期间已暂停本地视频播放与切换");
}

/** @brief 下载结束后按之前状态恢复本地轮播。 */
void MultimediaDemo::resumeLocalPlaybackAfterDownload()
{
    if (!m_downloadPauseActive) {
        return;
    }

    m_downloadPauseActive = false;
    m_localPlaybackEnabled = true;
    invalidatePendingLocalPlaybackStarts();

    if (!m_downloadResumeLocalAfterFinish) {
        return;
    }

    if (m_isLiveMode || m_liveDesired) {
        return;
    }

    if (!isVisible()) {
        return;
    }

    resumeLocalPlaylistFromStart();
}

/** @brief 更新非模态下载进度提示。 */
void MultimediaDemo::showDownloadProgressToast(const QString &title, int percent, const QString &detail)
{
    if (!m_downloadToast) {
        return;
    }

    m_downloadToast->showProgress(title, percent, detail);
}

/** @brief 隐藏并重置下载进度提示。 */
void MultimediaDemo::hideDownloadProgressToast()
{
    if (m_downloadToast) {
        m_downloadToast->hideToast();
    }
}

/** @brief 下载开始时暂停本地播放并显示进度提示。 */
void MultimediaDemo::onDownloadStarted(const QString& fileName)
{
    m_downloadActiveFileName = QFileInfo(fileName).fileName();
    pauseLocalPlaybackForDownload();
    showDownloadProgressToast(QStringLiteral("正在下载视频"), 0, m_downloadActiveFileName);
}

/** @brief 更新当前下载文件的进度提示。 */
void MultimediaDemo::onDownloadProgress(const QString& fileName, int percent)
{
    if (!m_downloadPauseActive) {
        pauseLocalPlaybackForDownload();
    }

    if (!fileName.trimmed().isEmpty()) {
        m_downloadActiveFileName = QFileInfo(fileName).fileName();
    }

    const QString detail = m_downloadActiveFileName.isEmpty()
            ? QStringLiteral("%1%").arg(percent)
            : QStringLiteral("%1    %2%").arg(m_downloadActiveFileName).arg(percent);

    showDownloadProgressToast(QStringLiteral("正在下载视频"), percent, detail);
}

/** @brief 下载成功后隐藏提示、更新媒体列表并恢复播放。 */
void MultimediaDemo::onDownloadFinished(const QString& filePath)
{
    const QString absPath = QFileInfo(filePath).absoluteFilePath();
    const QString name = QFileInfo(absPath).fileName();
    m_downloadActiveFileName = name;
    showDownloadProgressToast(QStringLiteral("下载完成"), 100, name);

    QTimer::singleShot(800, this, [this, absPath]() {
        hideDownloadProgressToast();

        const bool wasPaused = m_downloadPauseActive;
        m_downloadPauseActive = false;
        m_localPlaybackEnabled = true;

        // 下载结束后清空下载态截图缓存
        m_lastSnapshotMs = 0;
        m_lastSnapshotBase64.clear();

        invalidatePendingLocalPlaybackStarts();

        if (!wasPaused) {
            playDownloadedVideoNow(absPath);
            return;
        }


        if (!m_downloadResumeLocalAfterFinish) {
            stopEmptyVideoListWatch();
            return;
        }

        if (m_isLiveMode || m_liveDesired) {
            return;
        }

        if (!isVisible()) {
            return;
        }

        playDownloadedVideoNow(absPath);
    });
}

/** @brief 下载失败后显示原因并恢复被暂停的本地播放。 */
void MultimediaDemo::onDownloadFailed(const QString& fileName, const QString& reason)
{
    Q_UNUSED(reason);

    if (!fileName.trimmed().isEmpty()) {
        m_downloadActiveFileName = QFileInfo(fileName).fileName();
    }

    hideDownloadProgressToast();

    // 下载失败时清空缓存
    m_lastSnapshotMs = 0;
    m_lastSnapshotBase64.clear();
    resumeLocalPlaybackAfterDownload();
}

// 恢复资源使用
void MultimediaDemo::restoreResourcesOnPlay()
{
    // 恢复之前的视频质量设置
    applyVideoQualitySettings();

    LOG_VIDEO("视频恢复播放，已恢复资源使用");
}

// 测试视频优化功能
void MultimediaDemo::testVideoOptimizations()
{
    LOG_INIT("===== 开始测试视频优化功能 ====");

    // 测试缓冲功能
    LOG_BUFFER("测试1: 缓冲功能初始化");
    LOG_BUFFER("  - 缓冲指示器创建:" << (bufferIndicator != nullptr ? "成功" : "失败"));
    LOG_BUFFER("  - 初始缓冲状态:" << (isBuffering ? "缓冲中" : "非缓冲"));
    LOG_BUFFER("  - 当前缓冲百分比:" << bufferPercentage << "%");
    LOG_BUFFER("  - 最大缓冲时长:" << maxBufferDuration << "ms");

    // 测试自动质量调整
    LOG_QUALITY("\n测试2: 自动质量调整");
    LOG_QUALITY("  - 自动质量调整启用状态:" << (autoQualityAdjustment ? "已启用" : "已禁用"));
    LOG_QUALITY("  - 当前质量级别:" << currentQualityLevel);
    LOG_QUALITY("  - 质量检查间隔:" << qualityCheckInterval << "ms");

    // 测试预加载功能
    LOG_VIDEO("\n测试3: 视频预加载");
    LOG_VIDEO("  - 预加载播放器初始化:" << (nextVideoPlayer != nullptr ? "成功" : "失败"));
    LOG_VIDEO("  - 当前预加载状态:" << (isPreloading ? "预加载中" : "未预加载"));
    LOG_VIDEO("  - 视频文件数量:" << videoFileList.size());

    // 测试资源管理
    LOG_INIT("\n测试4: 资源管理优化");
    LOG_INIT("  - 媒体播放器状态:");

    LOG_VIDEO("播放器可用状态:" << mediaPlayer->isAvailable());
    LOG_VIDEO("预加载播放器可用状态:" << nextVideoPlayer->isAvailable());

    LOG_BUFFER("    缓冲模式: 不可用(在Qt 5.14中不支持)");

    LOG_INIT("\n===== 视频优化功能测试完成 =====");
}


// 提高视频质量
void MultimediaDemo::increaseQuality()
{
    if (currentQualityLevel < 3) {
        currentQualityLevel++;
        LOG_QUALITY("提高视频质量级别至:" << currentQualityLevel);
        applyVideoQualitySettings();
    }
}

// 应用视频质量设置
void MultimediaDemo::applyVideoQualitySettings()
{
    // 根据质量级别调整视频播放参数
    switch (currentQualityLevel) {
    case 1: // 低质量
        // 降低缓冲时长，减少内存占用
        maxBufferDuration = 5000;
        // 设置较低的播放质量参数
        //mediaPlayer->setPlaybackRate(0.85); // 稍微降低播放速度以提高流畅度
        break;
    case 2: // 中等质量
        maxBufferDuration = 7000;
        //mediaPlayer->setPlaybackRate(0.98);
        break;
    case 3: // 高质量
        maxBufferDuration = 8000;
        //mediaPlayer->setPlaybackRate(1);
        break;
    default:
        break;
    }

    // 重新设置缓冲模式
    if (currentQualityLevel == 1) {
        // 低质量时使用最小缓冲模式（移除了setBufferMode调用）
    } else {
        // 中高质量时使用全缓冲模式（移除了setBufferMode调用）
    }

    //LOG_QUALITY("应用视频质量设置: 缓冲时长=" << maxBufferDuration << "ms, 播放速度=" << mediaPlayer->playbackRate());
}

/** @brief 停止播放器和工作线程后释放页面资源。 */
MultimediaDemo::~MultimediaDemo()
{
    if (m_snapshotThread) {
        m_snapshotThread->quit();
        m_snapshotThread->wait();
    }

    delete ui;
    delete label_back;
    delete label_bg01;
    delete label_picture;
    delete label_logo1;
    delete label_logo2;
    delete label_logo3;
    delete label_logo4;
    delete bufferIndicator;
    delete nextVideoPlayer;
    delete timer;
    delete date_timer;
    delete pictureTimer;
    delete mediaPlayer;
    delete m_liveProbePlayer;
    delete videoWidget;
    delete m_netInfoPanel;
    // 释放空调相关资源
    delete airCondiModeTimer;
    delete airCondiWindSpeedTimer;
    delete airCondiTempTimer;
    delete label_airCondiMode;
    delete label_airCondiWindSpeed;
    delete label_airCondiTemp;
}

//初始化UI组件
void MultimediaDemo::initializeComponents()
{
    LOG_INIT("开始初始化UI组件");

    // 创建UI组件
    label_back = new QLabel(this);
    label_bg01 = new QLabel(this);
    label_direction = new QLabel(this);
    label_ten = new QLabel(this);
    label_one = new QLabel(this);
    label_state = new QLabel(this);
    label_date = new QLabel(this);
    label_time = new QLabel(this);
    label_week = new QLabel(this);
    label_picture = new QLabel(this);

    // videoRect 图片混播
    label_videoRectPicture = new QLabel(this);
    label_videoRectPicture->setObjectName("label_videoRectPicture");
    label_videoRectPicture->setAlignment(Qt::AlignCenter);
    label_videoRectPicture->setStyleSheet("background-color: black; border: none;");
    label_videoRectPicture->hide();

    // Logo标签初始化
    label_logo1 = new QLabel(this);
    label_logo2 = new QLabel(this);
    label_logo3 = new QLabel(this);
    label_logo4 = new QLabel(this);

    // 空调模式标签初始化
    label_airCondiMode = new QLabel(this);
    label_airCondiMode->setScaledContents(true);
    label_airCondiMode->hide(); // 默认隐藏，根据配置显示

    // 空调风速标签初始化
    label_airCondiWindSpeed = new QLabel(this);
    label_airCondiWindSpeed->setScaledContents(true);
    label_airCondiWindSpeed->hide(); // 默认隐藏，根据配置显示

    // 空调温度标签初始化
    label_airCondiTemp = new QLabel(this);
    label_airCondiTemp->setScaledContents(true);
    label_airCondiTemp->hide(); // 默认隐藏，根据配置显示

    // GStreamer 仍优先使用 MPP 硬件解码，解码后的 NV12 帧通过 appsink
    // 交给该 QOpenGLWidget，由 Qt/EGLFS 完成最终合成。
    videoWidget = new VideoHoleWidget(this);
    videoWidget->setObjectName("videoWidget");
    videoWidget->setAttribute(Qt::WA_OpaquePaintEvent, true);
    videoWidget->setAttribute(Qt::WA_NoSystemBackground, true);
    videoWidget->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    videoWidget->setFocusPolicy(Qt::NoFocus);

    //videoWidget->setUpdatesEnabled(false);      //禁用更新，避免后续 repaint 擦光标

    // 设置显示窗口大小与位置
    videoWidget->resize(VideoDisplay_x_Size, VideoDisplay_y_Size);
    videoWidget->move(VideoDisplay_x, VideoDisplay_y);
    videoWidget->show();
    videoWidget->raise();

    // 创建缓冲指示器
    bufferIndicator = new QLabel(this);
    bufferIndicator->setStyleSheet("background-color: rgba(0, 0, 0, 150); color: white; font-size: 16px; padding: 10px; border-radius: 5px;");
    bufferIndicator->setText("正在缓冲...");
    bufferIndicator->setAlignment(Qt::AlignCenter);
    bufferIndicator->hide(); // 默认隐藏
    LOG_BUFFER("缓冲指示器创建完成");

    // 广播标签初始化
    label_broadCast = new QLabel(this);
    label_broadCast->setScaledContents(true);
    label_broadCast->hide(); // 默认隐藏，根据配置显示

    // 创建 GStreamer 主媒体播放器和预加载媒体播放器
    mediaPlayer = new GstPlayerWidget(this);
    nextVideoPlayer = new GstPlayerWidget(this);

    m_liveProbePlayer = new GstPlayerWidget(this);
    m_liveProbePlayer->setPreloadMode(true);
    m_liveProbePlayer->setVolume(0.0f);
    m_liveProbePlayer->setVideoOutput(nullptr);

    // 绑定输出窗口
    mediaPlayer->setVideoOutput(videoWidget);
    nextVideoPlayer->setVideoOutput(nullptr);

    // 将 [videoRect] 区域显式传给 Qt/EGLFS 视频画布。GStreamer 仅负责
    // 解码和输出 NV12 帧，不再使用 GstVideoOverlay/X11 窗口句柄。
    mediaPlayer->setDisplayRect(QRect(VideoDisplay_x,
                                      VideoDisplay_y,
                                      VideoDisplay_x_Size,
                                      VideoDisplay_y_Size));

    mediaPlayer->setPreloadMode(false);
    nextVideoPlayer->setPreloadMode(true);

    connect(mediaPlayer, &GstPlayerWidget::videoFinished,
            this, &MultimediaDemo::playNextVideo, Qt::QueuedConnection);
    connect(nextVideoPlayer, &GstPlayerWidget::videoFinished,
            this, &MultimediaDemo::playNextVideo, Qt::QueuedConnection);


    connect(mediaPlayer, &GstPlayerWidget::snapshotReady,
            this, &MultimediaDemo::onVideoSnapshotReady);

    connect(mediaPlayer, &GstPlayerWidget::snapshotFailed,
            this, &MultimediaDemo::onVideoSnapshotFailed);

//    qDebug() << "videoFinished emitted from thread:" << QThread::currentThread();
//    qDebug() << "MultimediaDemo thread:" << this->thread();


    // 监听错误，定位预加载是否失败
    connect(mediaPlayer, &GstPlayerWidget::errorOccured, this, [this](const QString& msg){
        qWarning() << "[GStreamer] mediaPlayer error:" << msg;
        if (!m_isLiveMode) {
            return;
        }

        if (!m_liveSourceValidated) {
            if (!m_liveRecovering) {
                // 新地址在产生任何真实视频帧前就由GStreamer报告致命错误，
                // 延迟到当前setMedia/play调用栈结束后立即回本地，且不启动后台重试。
                m_liveRecovering = true;
                QTimer::singleShot(0, this, [this, msg]() {
                    if (!m_isLiveMode || m_liveSourceValidated) {
                        m_liveRecovering = false;
                        return;
                    }
                    onLiveStreamInterrupted(QStringLiteral("live startup error: ") + msg, false);
                });
            }
            return;
        }

        LOG_VIDEO("已验证LIVE播放器报告错误，等待10秒无帧watchdog确认网络中断:" << msg);
    });
    connect(nextVideoPlayer, &GstPlayerWidget::errorOccured, this, [](const QString& msg){
        qWarning() << "[GStreamer] nextVideoPlayer error:" << msg;
    });

    // 后台 LIVE 探测已经改为 QTcpSocket 轻量探测。
    // 不再使用 GstPlayerWidget 做探测，避免探测播放器误触发 prepared 后把前台从本地轮播切到 LIVE。

    connect(mediaPlayer, &GstPlayerWidget::positionChanged, this, [this](qint64){
        // 不再把 positionChanged 当作“收到新视频帧”。
        // 组播/UDP 断流时底层管线可能仍然不报错，甚至 position timer 还能周期触发，
        // 如果这里刷新 m_lastLiveFrameMs，会掩盖真实无帧状态，导致画面停在最后一帧不回本地。
    });

    connect(mediaPlayer, &GstPlayerWidget::videoFrameArrived,
            this,
            [this](qint64 msecs) {
        if (m_isLiveMode &&
            m_liveWatchdogStartMs > 0 &&
            msecs >= m_liveWatchdogStartMs) {
            m_lastLiveFrameMs = msecs;
            if (!m_liveSourceValidated) {
                m_liveSourceValidated = true;
                persistPlaybackStateToNetCfg(QStringLiteral("LIVE"), QStringLiteral("LIVE"),
                                             QStringLiteral("live_frame_validated"));
                LOG_VIDEO("直播源已收到真实解码帧，后续断流允许后台重试");
            }
        }
    }, Qt::QueuedConnection);

    connect(mediaPlayer, &GstPlayerWidget::liveTimelineFaultDetected,
            this,
            [this](const QString &reason) {
        if (!m_isLiveMode || !m_liveDesired || m_liveRecovering) return;
        m_liveRecovering = true;
        LOG_VIDEO("直播MPP输出持续失速或时间轴异常，原地完整重建LIVE管线:" << reason);
        restartLivePipelineAfterDecoderStall(reason);
    }, Qt::QueuedConnection);

    QTimer *qualityMonitorTimer = new QTimer(this);
    connect(qualityMonitorTimer, &QTimer::timeout, this, [this]() {
        if (autoQualityAdjustment && mediaPlayer->isAvailable()) {
            increaseQuality();
        }
    });
    qualityMonitorTimer->start(qualityCheckInterval);

    LOG_VIDEO("GStreamer 播放器创建完成");

    m_liveWatchdogTimer = new QTimer(this);
    m_liveWatchdogTimer->setInterval(1000); // 每秒检查一次
    connect(m_liveWatchdogTimer, &QTimer::timeout,
            this, &MultimediaDemo::onLiveWatchdogTimeout);

    m_liveRetryTimer = new QTimer(this);
    m_liveRetryTimer->setInterval(m_liveRetryIntervalMs);
    connect(m_liveRetryTimer, &QTimer::timeout,
            this, &MultimediaDemo::onLiveRetryTimeout);

    // 提示框
    m_diskUsageMonitor = new DiskUsageMonitor(this, Rk3566Platform::storageRoot(), 0.8);
    m_diskUsageMonitor->start();

    // online_v1/online_v2 网络异常提示：样式与磁盘空间提示一致。
    // 只有在线模式下才检查；异常时每1分钟提示一次，每次显示3秒。
    m_networkStatusMonitor = new NetworkStatusMonitor(this, Rk3566Platform::netConfigPath());
    m_networkStatusMonitor->start();

    // =========================
    // 截图后台处理线程
    // =========================
    m_snapshotThread = new QThread(this);
    m_snapshotProcessor = new SnapshotProcessor();
    m_snapshotProcessor->moveToThread(m_snapshotThread);

    m_snapshotTimeoutTimer = new QTimer(this);
    m_snapshotTimeoutTimer->setSingleShot(true);
    m_snapshotTimeoutTimer->setInterval(3000);

    connect(m_snapshotTimeoutTimer, &QTimer::timeout, this, [this]() {
        if (m_snapshotPending) {
            qWarning() << "[截图] 截图请求超时，强制清除 pending 状态";
            m_snapshotPending = false;
        }
    });

    connect(this, &MultimediaDemo::processUiSnapshotAsync,
            m_snapshotProcessor, &SnapshotProcessor::processUiOnlySnapshot,
            Qt::QueuedConnection);

    connect(this, &MultimediaDemo::processSnapshotAsync,
            m_snapshotProcessor, &SnapshotProcessor::processSnapshot,
            Qt::QueuedConnection);

    connect(m_snapshotProcessor, &SnapshotProcessor::finished,
            this, &MultimediaDemo::onSnapshotProcessFinished,
            Qt::QueuedConnection);

    connect(m_snapshotProcessor, &SnapshotProcessor::failed,
            this, &MultimediaDemo::onSnapshotProcessFailed,
            Qt::QueuedConnection);

    connect(m_snapshotThread, &QThread::finished,
            m_snapshotProcessor, &QObject::deleteLater);

    m_snapshotThread->start();

    // 空列表监测
    m_emptyVideoListTimer = new QTimer(this);
    m_emptyVideoListTimer->setInterval(m_emptyVideoScanIntervalMs);
    connect(m_emptyVideoListTimer, &QTimer::timeout,
            this, &MultimediaDemo::onEmptyVideoListWatchTimeout);

    // 自动弹窗收回
    m_panelAutoHideTimer = new QTimer(this);
    m_panelAutoHideTimer->setSingleShot(true);
    m_panelAutoHideTimer->setInterval(8000);

    connect(m_panelAutoHideTimer, &QTimer::timeout, this, [this]() {
            hideInfoPanels();
    });


    LOG_INIT("UI组件创建完成");

    // 设置标签属性
    label_back->setScaledContents(true);
    label_bg01->setScaledContents(true);
    label_direction->setScaledContents(true);
    label_ten->setScaledContents(true);
    label_one->setScaledContents(true);
    label_state->setScaledContents(true);
    label_picture->setScaledContents(false); // 图片在预加载时已缩放到目标尺寸
    label_picture->setVisible(false);

    // 设置Logo标签属性
    label_logo1->setScaledContents(true);
    label_logo1->setVisible(false);
    label_logo2->setScaledContents(true);
    label_logo2->setVisible(false);
    label_logo3->setScaledContents(true);
    label_logo3->setVisible(false);
    label_logo4->setScaledContents(true);
    label_logo4->setVisible(false);

    // 信号板数据到达前保持隐藏，避免启动阶段显示未初始化内容。
    label_direction->hide();
    label_ten->hide();
    label_one->hide();
    label_state->hide();

    // 确保日期和星期标签根据配置显示
    if (DateDisplay_en) {
        label_date->show();
    }
    if (WeekDisplay_en) {
        label_week->show();
    }
    if (TimeDisplay_en) {
        label_time->show();
    }


    LOG_INIT("UI组件初始化完成，初始标签状态设置完毕");
    LOG_INIT("初始显示状态: 楼层数字和状态标签已显示，方向/日期/星期标签根据配置显示");
}

/** @brief 从设备配置文件加载显示、媒体和功能参数。 */
void MultimediaDemo::readConfigurationFromFile()
{
    const QString configPath = Rk3566Platform::uiConfigPath();
    LOG_DEBUG("开始读取磁盘配置文件: " << configPath);

    QSettings settings(configPath, QSettings::IniFormat);
    LOG_DEBUG("实际使用的配置文件路径: " << configPath);

    // 读取运行模式。正常运行时不启动空调演示定时器。
    settings.beginGroup("DisplayDirRect");
    RunMode = settings.value("RunMode", false).toBool();
    settings.endGroup();
    LOG_CONFIG("读取运行模式: RunMode=" << RunMode);

    // 读取背景配置
    settings.beginGroup("backRect");
    backPic_en = settings.value("backPic_en", true).toBool();
    backPic_x_Size = settings.value("backPic_x_Size", 1024).toInt();
    backPic_y_Size = settings.value("backPic_y_Size", 768).toInt();
    settings.endGroup();
    //打印读取结果
    LOG_CONFIG("读取背景配置: backPic_en=" << backPic_en << ", backPic_x_Size=" << backPic_x_Size << ", backPic_y_Size=" << backPic_y_Size);

    // 读取方向显示配置
    settings.beginGroup("DirectionRect");
    Diretion_en = settings.value("Diretion_en", true).toBool();
    Diretion_x = settings.value("Diretion_x", 655).toInt();
    Diretion_y = settings.value("Diretion_y", 173).toInt();
    Diretion_x_Size = settings.value("Diretion_x_Size", 160).toInt();
    Diretion_y_Size = settings.value("Diretion_y_Size", 150).toInt();
    settings.endGroup();

    // 读取Logo配置
    settings.beginGroup("logoRect");
    Logo1_en = settings.value("Logo1_en", true).toBool();
    Logo1_x = settings.value("Logo1_x", 10).toInt();
    Logo1_y = settings.value("Logo1_y", 12).toInt();
    Logo1_x_Size = settings.value("Logo1_x_Size", 210).toInt();
    Logo1_y_Size = settings.value("Logo1_y_Size", 110).toInt();

    Logo2_en = settings.value("Logo2_en", false).toBool();
    Logo2_x = settings.value("Logo2_x", 628).toInt();
    Logo2_y = settings.value("Logo2_y", 541).toInt();
    Logo2_x_Size = settings.value("Logo2_x_Size", 172).toInt();
    Logo2_y_Size = settings.value("Logo2_y_Size", 59).toInt();

    Logo3_en = settings.value("Logo3_en", false).toBool();
    Logo3_x = settings.value("Logo3_x", 0).toInt();
    Logo3_y = settings.value("Logo3_y", 160).toInt();
    Logo3_x_Size = settings.value("Logo3_x_Size", 600).toInt();
    Logo3_y_Size = settings.value("Logo3_y_Size", 80).toInt();

    Logo4_en = settings.value("Logo4_en", false).toBool();
    Logo4_x = settings.value("Logo4_x", 167).toInt();
    Logo4_y = settings.value("Logo4_y", 0).toInt();
    Logo4_x_Size = settings.value("Logo4_x_Size", 600).toInt();
    Logo4_y_Size = settings.value("Logo4_y_Size", 80).toInt();
    settings.endGroup();
    //打印读取结果
    LOG_CONFIG("读取方向显示配置: Diretion_en=" << Diretion_en << ", Diretion_x=" << Diretion_x << ", Diretion_y=" << Diretion_y << ", Diretion_x_Size=" << Diretion_x_Size << ", Diretion_y_Size=" << Diretion_y_Size);

    // 读取楼层显示配置
    settings.beginGroup("floorRect");
    DisPlayPicOrText = settings.value("DisPlayPicOrText", 0).toInt();
    First_Floor_x = settings.value("First_Floor_x", 866).toInt();
    First_Floor_y = settings.value("First_Floor_y", 173).toInt();
    First_FloorS_x = settings.value("First_FloorS_x", 910).toInt();
    First_FloorS_y = settings.value("First_FloorS_y", 173).toInt();
    First_Floor_x_Size = settings.value("First_Floor_x_Size", 88).toInt();
    First_Floor_y_Size = settings.value("First_Floor_y_Size", 160).toInt();
    settings.endGroup();
    //打印读取结果
    LOG_CONFIG("读取楼层显示配置: DisPlayPicOrText=" << DisPlayPicOrText << ", First_Floor_x=" << First_Floor_x << ", First_Floor_y=" << First_Floor_y << ", First_FloorS_x=" << First_FloorS_x << ", First_FloorS_y=" << First_FloorS_y << ", First_Floor_x_Size=" << First_Floor_x_Size << ", First_Floor_y_Size=" << First_Floor_y_Size);

    // 读取状态标志配置
    settings.beginGroup("stateRect");
    Flag1_en = settings.value("Flag1_en", true).toBool();
    Flag1_x = settings.value("Flag1_x", 660).toInt();
    Flag1_y = settings.value("Flag1_y", 350).toInt();
    Flag1_x_Size = settings.value("Flag1_x_Size", 100).toInt();
    Flag1_y_Size = settings.value("Flag1_y_Size", 40).toInt();

    Flag2_en = settings.value("Flag2_en", true).toBool();
    Flag2_x = settings.value("Flag2_x", 770).toInt();
    Flag2_y = settings.value("Flag2_y", 350).toInt();
    Flag2_x_Size = settings.value("Flag2_x_Size", 100).toInt();
    Flag2_y_Size = settings.value("Flag2_y_Size", 40).toInt();
    settings.endGroup();
    //打印读取结果
    LOG_CONFIG("读取状态标志配置: Flag1_en=" << Flag1_en << ", Flag1_x=" << Flag1_x << ", Flag1_y=" << Flag1_y << ", Flag1_x_Size=" << Flag1_x_Size << ", Flag1_y_Size=" << Flag1_y_Size << ", Flag2_en=" << Flag2_en << ", Flag2_x=" << Flag2_x << ", Flag2_y=" << Flag2_y << ", Flag2_x_Size=" << Flag2_x_Size << ", Flag2_y_Size=" << Flag2_y_Size);

    // 读取日期和星期配置
    settings.beginGroup("dateRect");
    DateDisplay_en = settings.value("DateDisplay_en", true).toBool();
    DateDisplay_x = settings.value("DateDisplay_x", 670).toInt();
    DateDisplay_y = settings.value("DateDisplay_y", 54).toInt();
    DateDisplay_x_Size = settings.value("DateDisplay_x_Size", 170).toInt();
    DateDisplay_y_Size = settings.value("DateDisplay_y_Size", 38).toInt();

    // 日期字体配置 ----
    DateTextColorR = settings.value("DateTextColorR", 255).toInt();
    DateTextColorG = settings.value("DateTextColorG", 255).toInt();
    DateTextColorB = settings.value("DateTextColorB", 0).toInt();   // 默认黄
    DateTextSize   = settings.value("DateTextSize", 24).toInt();
    DateTextStyle  = settings.value("DateTextStyle", 0).toInt();

    // 读取时间配置
    TimeDisplay_en      = settings.value("TimeDisplay_en", false).toBool();
    TimeDisplay_x       = settings.value("TimeDisplay_x", 630).toInt();
    TimeDisplay_y       = settings.value("TimeDisplay_y", 10).toInt();
    TimeDisplay_x_Size  = settings.value("TimeDisplay_x_Size", 170).toInt();
    TimeDisplay_y_Size  = settings.value("TimeDisplay_y_Size", 80).toInt();

    TimeTextColorR      = settings.value("TimeTextColorR", 254).toInt();
    TimeTextColorG      = settings.value("TimeTextColorG", 254).toInt();
    TimeTextColorB      = settings.value("TimeTextColorB", 65).toInt();
    TimeTextSize        = settings.value("TimeTextSize", 50).toInt();
    TimeTextStyle       = settings.value("TimeTextStyle", 1).toInt();


    WeekDisplay_en = settings.value("WeekDisplay_en", true).toBool();
    WeekDisplay_x = settings.value("WeekDisplay_x", 890).toInt();
    WeekDisplay_y = settings.value("WeekDisplay_y", 48).toInt();
    WeekDisplay_x_Size = settings.value("WeekDisplay_x_Size", 160).toInt();
    WeekDisplay_y_Size = settings.value("WeekDisplay_y_Size", 38).toInt();
    WeekTextDispay = settings.value("WeekTextDispay", 0).toInt();

    WeekTextColorR = settings.value("WeekTextColorR", 255).toInt();
    WeekTextColorG = settings.value("WeekTextColorG", 255).toInt();
    WeekTextColorB = settings.value("WeekTextColorB", 0).toInt();
    WeekTextSize   = settings.value("WeekTextSize", 24).toInt();
    WeekTextStyle  = settings.value("WeekTextStyle", 0).toInt();

    settings.endGroup();
    //打印读取结果
    LOG_CONFIG("读取日期和星期配置: DateDisplay_en=" << DateDisplay_en << ", DateDisplay_x=" << DateDisplay_x << ", DateDisplay_y=" << DateDisplay_y << ", DateDisplay_x_Size=" << DateDisplay_x_Size << ", DateDisplay_y_Size=" << DateDisplay_y_Size << ", WeekDisplay_en=" << WeekDisplay_en << ", WeekDisplay_x=" << WeekDisplay_x << ", WeekDisplay_y=" << WeekDisplay_y << ", WeekDisplay_x_Size=" << WeekDisplay_x_Size << ", WeekDisplay_y_Size=" << WeekDisplay_y_Size);
    LOG_CONFIG("读取时间显示配置: TimeDisplay_en=" << TimeDisplay_en
               << ", pos=(" << TimeDisplay_x << "," << TimeDisplay_y << ")"
               << ", size=(" << TimeDisplay_x_Size << "x" << TimeDisplay_y_Size << ")"
               << ", color=(" << TimeTextColorR << "," << TimeTextColorG << "," << TimeTextColorB << ")"
               << ", textSize=" << TimeTextSize
               << ", textStyle=" << TimeTextStyle);

    // 读取特殊楼层配置
    const QString iniPath = configPath;
    loadFloorRemap(iniPath);

//    settings.beginGroup("SpecialFloor");
//    SpecialFloor_en = settings.value("SpecialFloor_en", true).toBool();
//    SpecialFloor_x = settings.value("SpecialFloor_x", 0).toInt();
//    SpecialFloor_y = settings.value("SpecialFloor_y", 315).toInt();
//    SpecialFloor_x_Size = settings.value("SpecialFloor_x_Size", 244).toInt();
//    SpecialFloor_y_Size = settings.value("SpecialFloor_y_Size", 160).toInt();
//    oldFloor1 = settings.value("oldFloor1", 4).toInt();
//    newFloor1 = settings.value("newFloor1", "3A").toString();
//    settings.endGroup();
//    //打印读取结果
//    LOG_CONFIG("读取特殊楼层配置: SpecialFloor_en=" << SpecialFloor_en << ", SpecialFloor_x=" << SpecialFloor_x << ", SpecialFloor_y=" << SpecialFloor_y << ", SpecialFloor_x_Size=" << SpecialFloor_x_Size << ", SpecialFloor_y_Size=" << SpecialFloor_y_Size << ", oldFloor1=" << oldFloor1 << ", newFloor1=" << newFloor1);

    // 读取BG01背景图配置
    settings.beginGroup("BGpictureRect");
    BgDisplay_en = settings.value("BgDisplay_en", true).toBool();
    BgDisplay_x = settings.value("BgDisplay_x", 16).toInt();
    BgDisplay_y = settings.value("BgDisplay_y", 163).toInt();
    BgDisplay_x_Size = settings.value("BgDisplay_x_Size", 0).toInt();
    BgDisplay_y_Size = settings.value("BgDisplay_y_Size", 0).toInt();
    settings.endGroup();
    //打印读取结果
    LOG_CONFIG("读取BG01背景图配置: BgDisplay_en=" << BgDisplay_en << ", BgDisplay_x=" << BgDisplay_x << ", BgDisplay_y=" << BgDisplay_y
             << ", BgDisplay_x_Size=" << BgDisplay_x_Size << ", BgDisplay_y_Size=" << BgDisplay_y_Size);

    // 读取视频显示配置
    settings.beginGroup("videoRect");
    VideoDisplay_en = settings.value("VideoDisplay_en", true).toBool();
    VideoDisplay_x = settings.value("VideoDisplay_x", 16).toInt();
    VideoDisplay_y = settings.value("VideoDisplay_y", 163).toInt();
    VideoDisplay_x_Size = settings.value("VideoDisplay_x_Size", 613).toInt();
    VideoDisplay_y_Size = settings.value("VideoDisplay_y_Size", 529).toInt();

    // videoRect 图片轮播间隔，单位秒
    VideoPic_disp_Time = settings.value("VideoPic_disp_Time", 4).toInt();
    if (VideoPic_disp_Time <= 0) {
        VideoPic_disp_Time = 4;
    }

    settings.endGroup();

    // 读取图片显示配置
    settings.beginGroup("pictureRect");
    PicDisplay_en = settings.value("PicDisplay_en", true).toBool();
    PicDisplay_x = settings.value("PicDisplay_x", 650).toInt();
    PicDisplay_y = settings.value("PicDisplay_y", 431).toInt();
    PicDisplay_x_Size = settings.value("PicDisplay_x_Size", 358).toInt();
    PicDisplay_y_Size = settings.value("PicDisplay_y_Size", 261).toInt();
    PicDisplay_Time = settings.value("PicDisplay_Time", 5).toInt();
    PicToFlag_En = settings.value("PicToFlag_En", 0).toBool();
    PicDisplay_XgSpeed = settings.value("PicDisplay_XgSpeed", 500).toInt();
    settings.endGroup();
    //打印读取结果
    LOG_CONFIG("读取视频显示配置: VideoDisplay_en=" << VideoDisplay_en << ", VideoDisplay_x="
                << VideoDisplay_x << ", VideoDisplay_y=" << VideoDisplay_y
                << ", VideoDisplay_x_Size=" << VideoDisplay_x_Size << ", VideoDisplay_y_Size=" << VideoDisplay_y_Size
                << ", VideoPic_disp_Time=" << VideoPic_disp_Time);

    // 配置读取发生在播放器控件创建之后，因此必须在这里重新应用实际区域。
    // 旧代码只更新了成员变量，GStreamer 原生窗口可能仍保留初始化时区域。
    const QRect configuredVideoRect(VideoDisplay_x,
                                    VideoDisplay_y,
                                    VideoDisplay_x_Size,
                                    VideoDisplay_y_Size);
    if (configuredVideoRect.width() > 0 && configuredVideoRect.height() > 0) {
        if (videoWidget) {
            videoWidget->setGeometry(configuredVideoRect);
        }
        if (label_videoRectPicture) {
            label_videoRectPicture->setGeometry(configuredVideoRect);
        }
        if (mediaPlayer) {
            mediaPlayer->setDisplayRect(configuredVideoRect);
        }
        LOG_CONFIG("GStreamer视频区域已应用:" << configuredVideoRect);
    } else {
        LOG_CONFIG("警告: videoRect区域无效，继续使用默认区域:" << configuredVideoRect);
    }

    // 读取空调配置
    settings.beginGroup("airCondiRect");
    AirCondi_en = settings.value("AirCondi_en", false).toBool();
    AirCondiMode_en = settings.value("AirCondiMode_en", false).toBool();
    AirCondiTemp_en = settings.value("AirCondiTemp_en", false).toBool();
    AirCondiWind_en = settings.value("AirCondiWind_en", false).toBool();
    AirCondiMode_x = settings.value("AirCondiMode_x", 250).toInt();
    AirCondiMode_y = settings.value("AirCondiMode_y", 12).toInt();
    AirCondiMode_x_Size = settings.value("AirCondiMode_x_Size", 50).toInt();
    AirCondiMode_y_Size = settings.value("AirCondiMode_y_Size", 50).toInt();
    AirCondiTemp_x = settings.value("AirCondiTemp_x", 305).toInt();
    AirCondiTemp_y = settings.value("AirCondiTemp_y", 12).toInt();
    AirCondiTemp_x_Size = settings.value("AirCondiTemp_x_Size", 60).toInt();
    AirCondiTemp_y_Size = settings.value("AirCondiTemp_y_Size", 50).toInt();
    airCondiTempColor = settings.value("AirCondiTemp_color", "Red").toString();
    AirCondiTemp_font_Size = settings.value("AirCondiTemp_font_Size", 20).toInt();
    AirCondiWind_x = settings.value("AirCondiWind_x", 375).toInt();
    AirCondiWind_y = settings.value("AirCondiWind_y", 12).toInt();
    AirCondiWind_x_Size = settings.value("AirCondiWind_x_Size", 50).toInt();
    AirCondiWind_y_Size = settings.value("AirCondiWind_y_Size", 50).toInt();
    settings.endGroup();
    // 打印空调配置
    LOG_CONFIG("读取空调配置:");
    LOG_CONFIG("  AirCondi_en=" << AirCondi_en
               << ", AirCondiMode_en=" << AirCondiMode_en
               << ", AirCondiTemp_en=" << AirCondiTemp_en
               << ", AirCondiWind_en=" << AirCondiWind_en);
    LOG_AC("  AirCondiMode: x=" << AirCondiMode_x << ", y=" << AirCondiMode_y << ", size= " << AirCondiMode_x_Size << "x" << AirCondiMode_y_Size);
    LOG_AC("  AirCondiTemp: x=" << AirCondiTemp_x << ", y=" << AirCondiTemp_y << ", size= " << AirCondiTemp_x_Size << "x" << AirCondiTemp_y_Size << ", color=" << airCondiTempColor << ", font size=" << AirCondiTemp_font_Size);
    LOG_AC("  AirCondiWind: x=" << AirCondiWind_x << ", y=" << AirCondiWind_y << ", size= " << AirCondiWind_x_Size << "x" << AirCondiWind_y_Size);

    // 读取广播配置
    settings.beginGroup("broadCastRect");
    BroadCast_en = settings.value("BroadCast_en", false).toBool();
    BroadCast_x = settings.value("BroadCast_x", 430).toInt();
    BroadCast_y = settings.value("BroadCast_y", 12).toInt();
    BroadCast_x_Size = settings.value("BroadCast_x_Size", 50).toInt();
    BroadCast_y_Size = settings.value("BroadCast_y_Size", 50).toInt();
    settings.endGroup();
    // 打印广播配置
    LOG_CONFIG("读取广播配置:");
    LOG_CONFIG("  BroadCast_en=" << BroadCast_en);
    LOG_CONFIG("  BroadCast: x=" << BroadCast_x << ", y=" << BroadCast_y << ", size= " << BroadCast_x_Size << "x" << BroadCast_y_Size);

    // 读取可视对讲配置
    settings.beginGroup("voiceTalkRect");
    VoiceTalk_en = settings.value("VoiceTalk_en", false).toBool();
    VoiceTalk_x = settings.value("VoiceTalk_x", 660).toInt();
    VoiceTalk_y = settings.value("VoiceTalk_y", 1120).toInt();
    VoiceTalk_x_Size = settings.value("VoiceTalk_x_Size", 160).toInt();
    VoiceTalk_y_Size = settings.value("VoiceTalk_y_Size", 150).toInt();
    settings.endGroup();
    // 打印可视对讲配置
    LOG_CONFIG("读取可视对讲配置:");
    LOG_CONFIG("  VoiceTalk_en=" << VoiceTalk_en);
    LOG_CONFIG("  VoiceTalk: x=" << VoiceTalk_x << ", y=" << VoiceTalk_y << ", size= " << VoiceTalk_x_Size << "x" << VoiceTalk_y_Size);
}


/** @brief 读取配置后初始化界面可见性和几何参数。 */
void MultimediaDemo::initializeConfiguration()
{
    LOG_INIT("开始初始化配置参数");

    // 加载并显示背景图片（最低层）
    // 图片相对路径
    const QString backRel = "demoResources/images/logo/back.bmp";

    // 外部优先、qrc 兜底
    const QString backPathExt = QDir(extRoot).filePath(backRel);
    const QString backPathQrc = QDir(qrcRoot).filePath(backRel);

    if (backPic_en) {
        QPixmap backPixmap;
        QString usedPath;

        bool loaded = false;

        LOG_DEBUG("Qt支持的图片格式: " << QImageReader::supportedImageFormats());

        auto tryLoadBackImage = [&](const QString &path, const QString &tag) -> bool {
            QFileInfo fi(path);

            LOG_DEBUG(tag << " 背景图片路径: " << path);
//            LOG_DEBUG(tag << " exists=" << QFile::exists(path)
//                          << ", isFile=" << fi.isFile()
//                          << ", readable=" << fi.isReadable()
//                          << ", size=" << fi.size());

            QImageReader reader(path);
//            LOG_DEBUG(tag << " reader.format=" << reader.format()
//                          << ", canRead=" << reader.canRead());

            QImage image = reader.read();
            if (image.isNull()) {
                LOG_DEBUG(tag << " 背景图片读取失败: "
                              << reader.error()
                              << ", errorString=" << reader.errorString());
                return false;
            }

            backPixmap = QPixmap::fromImage(image);
            if (backPixmap.isNull()) {
                LOG_DEBUG(tag << " QImage读取成功，但转换QPixmap失败");
                return false;
            }

            usedPath = path;
            LOG_DEBUG(tag << " 背景图片读取成功: "
                          << image.width() << "x" << image.height()
                          << ", format=" << image.format());

            return true;
        };

        // 先尝试外部
        if (QFile::exists(backPathExt)) {
            LOG_DEBUG("尝试从外部目录加载背景图片: " << backPathExt);
            loaded = tryLoadBackImage(backPathExt, "外部");
        } else {
            LOG_DEBUG("外部背景图片不存在: " << backPathExt);
        }

        // 外部失败再用 qrc
        if (!loaded) {
            LOG_DEBUG("尝试从qrc加载背景图片: " << backPathQrc);
            loaded = tryLoadBackImage(backPathQrc, "qrc");
        }

        if (loaded) {
            label_back->setPixmap(backPixmap);
            label_back->resize(backPic_x_Size, backPic_y_Size);
            label_back->move(0, 0);
            label_back->show();
            label_back->lower(); // 确保back在最低层

            LOG_DEBUG("背景图片加载成功: " << usedPath);
            LOG_DEBUG("背景图片大小: " << backPic_x_Size << "x" << backPic_y_Size);
        } else {
            LOG_DEBUG("警告: 无法加载背景图片");
            LOG_DEBUG("尝试的外部路径: " << backPathExt);
            LOG_DEBUG("尝试的qrc路径: " << backPathQrc);
        }
    }

    // 图片相对路径
    const QString bg01Rel = "demoResources/images/logo/BG01.jpg";

    // 外部优先、qrc 兜底
    const QString bg01PathExt = QDir(extRoot).filePath(bg01Rel);
    const QString bg01PathQrc = QDir(qrcRoot).filePath(bg01Rel);

    // 加载并显示BG01背景图，同时考虑BgDisplay_en配置
    if (bg01Pic_en && BgDisplay_en) {
        QPixmap bg01Pixmap;
        QString usedPath;
        bool loaded = false;

        // 1) 先尝试外部
        LOG_CONFIG("尝试从外部目录加载BG01背景图: " << bg01PathExt);
        {
            const QFileInfo fi(bg01PathExt);
            if (fi.exists() && fi.isFile() && fi.isReadable()) {
                loaded = bg01Pixmap.load(bg01PathExt);
                if (loaded) {
                    usedPath = bg01PathExt;
                } else {
                    LOG_CONFIG("外部BG01存在但加载失败(可能损坏/格式不对): " << bg01PathExt);
                }
            } else {
                LOG_CONFIG("外部BG01不可用(不存在/非文件/不可读): " << bg01PathExt);
            }
        }

        // 2) 外部失败再用 qrc
        if (!loaded) {
            LOG_CONFIG("尝试从qrc加载BG01背景图: " << bg01PathQrc);
            loaded = bg01Pixmap.load(bg01PathQrc);
            if (loaded) {
                usedPath = bg01PathQrc;
            } else {
                LOG_CONFIG("qrc BG01加载失败: " << bg01PathQrc);
            }
        }

        if (loaded) {
            label_bg01->setPixmap(bg01Pixmap);

            // 优先使用配置文件中的大小，如果未设置或为0，则使用图片本身大小
            if (BgDisplay_x_Size > 0 && BgDisplay_y_Size > 0) {
                label_bg01->resize(BgDisplay_x_Size, BgDisplay_y_Size);
                LOG_CONFIG("BG01使用配置文件大小: " << BgDisplay_x_Size << "x" << BgDisplay_y_Size);
            } else {
                label_bg01->resize(bg01Pixmap.size());
                LOG_CONFIG("BG01使用图片自身大小: " << bg01Pixmap.width() << "x" << bg01Pixmap.height());
            }

            // 使用从配置文件读取的坐标
            label_bg01->move(BgDisplay_x, BgDisplay_y);
            label_bg01->show();

            LOG_CONFIG("BG01背景图加载成功: " << usedPath);
            LOG_CONFIG("BG01位置: (" << BgDisplay_x << ", " << BgDisplay_y << ")");
        } else {
            LOG_CONFIG("警告: 无法加载BG01背景图");
            LOG_CONFIG("尝试的外部路径: " << bg01PathExt);
            LOG_CONFIG("尝试的qrc路径: " << bg01PathQrc);
        }
    }

    // 设置方向标签位置和大小
    if (Diretion_en) {
        label_direction->move(Diretion_x, Diretion_y);
        label_direction->resize(Diretion_x_Size, Diretion_y_Size);
        LOG_CONFIG("方向标签已设置位置和大小: (" << Diretion_x << ", " << Diretion_y << ") 大小: " << Diretion_x_Size << "x" << Diretion_y_Size);
    }

    // 楼层、方向和状态图片在启动阶段预加载，信号变化时只取缓存。
    preloadSignalUiPixmaps();

    // 设置Logo1显示
    if (Logo1_en) {
        // 图片相对路径
        const QString logo1Rel = "demoResources/images/logo/logo1.png";

        // 外部优先、qrc 兜底
        const QString logo1PathExt = QDir(extRoot).filePath(logo1Rel);
        const QString logo1PathQrc = QDir(qrcRoot).filePath(logo1Rel);

        QPixmap logo1Pixmap;
        QString usedPath;

        // 1) 先尝试外部
        LOG_CONFIG("尝试从外部目录加载logo1图片: " << logo1PathExt);
        {
            const QFileInfo fi(logo1PathExt);
            if (fi.exists() && fi.isFile() && fi.isReadable()) {
                logo1Pixmap = loadPixmapWithIgnoreWarnings(logo1PathExt);
                if (!logo1Pixmap.isNull()) {
                    usedPath = logo1PathExt;
                } else {
                    LOG_CONFIG("外部logo1存在但加载失败(可能损坏/格式不对): " << logo1PathExt);
                }
            } else {
                LOG_CONFIG("外部logo1不可用(不存在/非文件/不可读): " << logo1PathExt);
            }
        }

        // 2) 外部失败再用 qrc
        if (logo1Pixmap.isNull()) {
            LOG_CONFIG("尝试从qrc加载logo1图片: " << logo1PathQrc);
            logo1Pixmap = loadPixmapWithIgnoreWarnings(logo1PathQrc);
            if (!logo1Pixmap.isNull()) {
                usedPath = logo1PathQrc;
            } else {
                LOG_CONFIG("qrc logo1加载失败: " << logo1PathQrc);
            }
        }

        if (!logo1Pixmap.isNull()) {
            label_logo1->move(Logo1_x, Logo1_y);
            label_logo1->resize(Logo1_x_Size, Logo1_y_Size);
            label_logo1->setPixmap(logo1Pixmap);
            label_logo1->show();

            LOG_CONFIG("logo1图片加载成功: " << usedPath);
            LOG_CONFIG("logo1已设置位置和大小: (" << Logo1_x << ", " << Logo1_y << ") 大小: " << Logo1_x_Size << "x" << Logo1_y_Size);
        } else {
            LOG_CONFIG("警告: 无法加载logo1图片");
            LOG_CONFIG("尝试的外部路径: " << logo1PathExt);
            LOG_CONFIG("尝试的qrc路径: " << logo1PathQrc);
        }
    }


    // 设置Logo2显示
    if (Logo2_en) {
        const QString logo2Rel = "demoResources/images/logo/logo2.png";
        const QString logo2PathExt = QDir(extRoot).filePath(logo2Rel);
        const QString logo2PathQrc = QDir(qrcRoot).filePath(logo2Rel);

        QPixmap logo2Pixmap;
        QString usedPath;

        // 1) 外部优先
        LOG_CONFIG("尝试从外部目录加载logo2图片: " << logo2PathExt);
        {
            const QFileInfo fi(logo2PathExt);
            if (fi.exists() && fi.isFile() && fi.isReadable()) {
                logo2Pixmap = loadPixmapWithIgnoreWarnings(logo2PathExt);
                if (!logo2Pixmap.isNull()) {
                    usedPath = logo2PathExt;
                } else {
                    LOG_CONFIG("外部logo2存在但加载失败(可能损坏/格式不对): " << logo2PathExt);
                }
            } else {
                LOG_CONFIG("外部logo2不可用(不存在/非文件/不可读): " << logo2PathExt);
            }
        }

        // 2) 外部失败再用 qrc
        if (logo2Pixmap.isNull()) {
            LOG_CONFIG("尝试从qrc加载logo2图片: " << logo2PathQrc);
            logo2Pixmap = loadPixmapWithIgnoreWarnings(logo2PathQrc);
            if (!logo2Pixmap.isNull()) {
                usedPath = logo2PathQrc;
            } else {
                LOG_CONFIG("qrc logo2加载失败: " << logo2PathQrc);
            }
        }

        if (!logo2Pixmap.isNull()) {
            label_logo2->move(Logo2_x, Logo2_y);
            label_logo2->resize(Logo2_x_Size, Logo2_y_Size);
            label_logo2->setPixmap(logo2Pixmap);
            label_logo2->show();

            LOG_CONFIG("logo2图片加载成功: " << usedPath);
            LOG_CONFIG("logo2已设置位置和大小: (" << Logo2_x << ", " << Logo2_y
                      << ") 大小: " << Logo2_x_Size << "x" << Logo2_y_Size);
        } else {
            LOG_CONFIG("警告: 无法加载logo2图片");
            LOG_CONFIG("尝试的外部路径: " << logo2PathExt);
            LOG_CONFIG("尝试的qrc路径: " << logo2PathQrc);
        }
    }


    // 设置Logo3显示
    if (Logo3_en) {
        const QString logo3Rel = "demoResources/images/logo/logo3.png";
        const QString logo3PathExt = QDir(extRoot).filePath(logo3Rel);
        const QString logo3PathQrc = QDir(qrcRoot).filePath(logo3Rel);

        QPixmap logo3Pixmap;
        QString usedPath;

        // 1) 外部优先
        LOG_CONFIG("尝试从外部目录加载logo3图片: " << logo3PathExt);
        {
            const QFileInfo fi(logo3PathExt);
            if (fi.exists() && fi.isFile() && fi.isReadable()) {
                logo3Pixmap = loadPixmapWithIgnoreWarnings(logo3PathExt);
                if (!logo3Pixmap.isNull()) {
                    usedPath = logo3PathExt;
                } else {
                    LOG_CONFIG("外部logo3存在但加载失败(可能损坏/格式不对): " << logo3PathExt);
                }
            } else {
                LOG_CONFIG("外部logo3不可用(不存在/非文件/不可读): " << logo3PathExt);
            }
        }

        // 2) 外部失败再用 qrc
        if (logo3Pixmap.isNull()) {
            LOG_CONFIG("尝试从qrc加载logo3图片: " << logo3PathQrc);
            logo3Pixmap = loadPixmapWithIgnoreWarnings(logo3PathQrc);
            if (!logo3Pixmap.isNull()) {
                usedPath = logo3PathQrc;
            } else {
                LOG_CONFIG("qrc logo3加载失败: " << logo3PathQrc);
            }
        }

        if (!logo3Pixmap.isNull()) {
            label_logo3->move(Logo3_x, Logo3_y);
            label_logo3->resize(Logo3_x_Size, Logo3_y_Size);
            label_logo3->setPixmap(logo3Pixmap);
            label_logo3->show();

            LOG_CONFIG("logo3图片加载成功: " << usedPath);
            LOG_CONFIG("logo3已设置位置和大小: (" << Logo3_x << ", " << Logo3_y
                      << ") 大小: " << Logo3_x_Size << "x" << Logo3_y_Size);
        } else {
            LOG_CONFIG("警告: 无法加载logo3图片");
            LOG_CONFIG("尝试的外部路径: " << logo3PathExt);
            LOG_CONFIG("尝试的qrc路径: " << logo3PathQrc);
        }
    }


    // 设置Logo4显示
    if (Logo4_en) {
        const QString logo4Rel = "demoResources/images/logo/logo4.png";
        const QString logo4PathExt = QDir(extRoot).filePath(logo4Rel);
        const QString logo4PathQrc = QDir(qrcRoot).filePath(logo4Rel);

        QPixmap logo4Pixmap;
        QString usedPath;

        // 1) 外部优先
        LOG_CONFIG("尝试从外部目录加载logo4图片: " << logo4PathExt);
        {
            const QFileInfo fi(logo4PathExt);
            if (fi.exists() && fi.isFile() && fi.isReadable()) {
                logo4Pixmap = loadPixmapWithIgnoreWarnings(logo4PathExt);
                if (!logo4Pixmap.isNull()) {
                    usedPath = logo4PathExt;
                } else {
                    LOG_CONFIG("外部logo4存在但加载失败(可能损坏/格式不对): " << logo4PathExt);
                }
            } else {
                LOG_CONFIG("外部logo4不可用(不存在/非文件/不可读): " << logo4PathExt);
            }
        }

        // 2) 外部失败再用 qrc
        if (logo4Pixmap.isNull()) {
            LOG_CONFIG("尝试从qrc加载logo4图片: " << logo4PathQrc);
            logo4Pixmap = loadPixmapWithIgnoreWarnings(logo4PathQrc);
            if (!logo4Pixmap.isNull()) {
                usedPath = logo4PathQrc;
            } else {
                LOG_CONFIG("qrc logo4加载失败: " << logo4PathQrc);
            }
        }

        if (!logo4Pixmap.isNull()) {
            label_logo4->move(Logo4_x, Logo4_y);
            label_logo4->resize(Logo4_x_Size, Logo4_y_Size);
            label_logo4->setPixmap(logo4Pixmap);
            label_logo4->show();

            LOG_CONFIG("logo4图片加载成功: " << usedPath);
            LOG_CONFIG("logo4已设置位置和大小: (" << Logo4_x << ", " << Logo4_y
                      << ") 大小: " << Logo4_x_Size << "x" << Logo4_y_Size);
        } else {
            LOG_CONFIG("警告: 无法加载logo4图片");
            LOG_CONFIG("尝试的外部路径: " << logo4PathExt);
            LOG_CONFIG("尝试的qrc路径: " << logo4PathQrc);
        }
    }


    // 设置空调模式显示
    if (AirCondi_en && AirCondiMode_en && label_airCondiMode) {
        // 手动触发一次空调模式切换，确保启动时就显示图标
        onAirCondiModeTimeout();
    }

    // 设置空调风速显示
    if (AirCondi_en && AirCondiWind_en && label_airCondiWindSpeed) {
        // 手动触发一次空调风速切换，确保启动时就显示图标
        onAirCondiWindSpeedTimeout();
    }

    // 设置空调温度显示
    if (AirCondi_en && AirCondiTemp_en && label_airCondiTemp) {
        // 设置温度值（使用配置的默认值25度）
        label_airCondiTemp->setText(QString::number(currentAirCondiTemp) + "°C");
        // 设置字体大小和样式
        QFont font;
        font.setPointSize(AirCondiTemp_font_Size);
        font.setBold(true);
        label_airCondiTemp->setFont(font);
        // 设置文本颜色
        label_airCondiTemp->setStyleSheet("color: " + airCondiTempColor + ";");
        // 设置位置和大小（使用配置参数）
        label_airCondiTemp->move(AirCondiTemp_x, AirCondiTemp_y);
        label_airCondiTemp->resize(AirCondiTemp_x_Size, AirCondiTemp_y_Size);
        // 设置文本居中对齐
        label_airCondiTemp->setAlignment(Qt::AlignCenter);
        // 显示温度标签
        label_airCondiTemp->show();
        LOG_AC("空调温度显示已初始化: " << currentAirCondiTemp << "°C, 位置: (" << AirCondiTemp_x << ", " << AirCondiTemp_y << "), 大小: " << AirCondiTemp_x_Size << "x" << AirCondiTemp_y_Size);
    }

    // 背景图是在 VideoHoleWidget 创建之后才加载和 show() 的。这里重新固定
    // EGLFS 合成层级，避免整块 BG01/back 标签遮住已经正常绘制的视频 FBO。
    if (videoWidget) {
        videoWidget->raise();
    }

    // 再把文字、楼层和图标提升到视频之上，保持原来的叠加显示效果。
    // 最终层级：back/bg01 -> videoWidget -> 业务UI标签。
    label_direction->raise();
    label_ten->raise();
    label_one->raise();
    label_state->raise();
    label_date->raise();
    label_time->raise();
    label_week->raise();
    label_picture->raise();
    label_logo1->raise();
    label_logo2->raise();
    label_logo3->raise();
    label_logo4->raise();

    LOG_INIT("EGLFS层级已固定：back/bg01(底层) -> videoWidget -> 业务UI标签(顶层)");

    LOG_CONFIG("配置参数初始化完成:");
    LOG_CONFIG("  - 楼层显示位置: (" << First_Floor_x << ", " << First_Floor_y << ") 大小: " << First_Floor_x_Size << "x" << First_Floor_y_Size);
    LOG_CONFIG("  - 日期显示位置: (" << DateDisplay_x << ", " << DateDisplay_y << ")");
    LOG_CONFIG("  - 星期显示位置: (" << WeekDisplay_x << ", " << WeekDisplay_y << ")");

    // 初始化视频播放
    if (VideoDisplay_en) {
        initVideoPlayer();

        // 显示播放组件
        if (!videoRectMediaList.isEmpty()) {
            if (!m_currentVideoRectIsImage && videoWidget) {
                videoWidget->show();
            }
            // 延迟调用测试函数，确保所有组件已初始化完成
            QTimer::singleShot(1000, this, &MultimediaDemo::testVideoOptimizations);

            // 新增：延时截图，等视频真正显示出来后再抓
            //QTimer::singleShot(3000, this, &MultimediaDemo::testScreenGrab);
        } else {
            if (videoWidget) {
                videoWidget->hide();
            }
        }

    } else {
        stopVideoRectPictureDisplay();
        if (videoWidget) {
            videoWidget->hide();
        }
    }

    // 设置广播显示
    if (BroadCast_en && label_broadCast) {
        // 图片相对路径
        const QString broadRel = "demoResources/images/broad/Broadcasting.png";

        // 外部优先、qrc 兜底
        const QString broadPathExt = QDir(extRoot).filePath(broadRel);
        const QString broadPathQrc = QDir(qrcRoot).filePath(broadRel);

        QPixmap broadCastPixmap;
        QString usedPath;

        // 1) 先尝试外部
        LOG_CONFIG("尝试从外部目录加载广播图标: " << broadPathExt);
        {
            const QFileInfo fi(broadPathExt);
            if (fi.exists() && fi.isFile() && fi.isReadable()) {
                broadCastPixmap = loadPixmapWithIgnoreWarnings(broadPathExt);
                if (!broadCastPixmap.isNull()) {
                    usedPath = broadPathExt;
                } else {
                    LOG_CONFIG("外部广播图标存在但加载失败(可能损坏/格式不对): " << broadPathExt);
                }
            } else {
                LOG_CONFIG("外部广播图标不可用(不存在/非文件/不可读): " << broadPathExt);
            }
        }

        // 2) 外部失败再用 qrc
        if (broadCastPixmap.isNull()) {
            LOG_CONFIG("尝试从qrc加载广播图标: " << broadPathQrc);
            broadCastPixmap = loadPixmapWithIgnoreWarnings(broadPathQrc);
            if (!broadCastPixmap.isNull()) {
                usedPath = broadPathQrc;
            } else {
                LOG_CONFIG("qrc 广播图标加载失败: " << broadPathQrc);
            }
        }

        if (!broadCastPixmap.isNull()) {
            label_broadCast->move(BroadCast_x, BroadCast_y);
            label_broadCast->resize(BroadCast_x_Size, BroadCast_y_Size);
            label_broadCast->setPixmap(broadCastPixmap);
            label_broadCast->show();
            label_broadCast->raise(); // 确保在顶层

            LOG_CONFIG("广播图标加载成功: " << usedPath);
            LOG_CONFIG("广播图标已设置位置和大小: (" << BroadCast_x << ", " << BroadCast_y
                      << ") 大小: " << BroadCast_x_Size << "x" << BroadCast_y_Size);
        } else {
            LOG_CONFIG("警告: 无法加载广播图标");
            LOG_CONFIG("尝试的外部路径: " << broadPathExt);
            LOG_CONFIG("尝试的qrc路径: " << broadPathQrc);
        }
    }

    // 初始化图片显示
    if (PicDisplay_en) {
        initPicturePlayer();
    } else {
        label_picture->hide();
    }

    // 让本窗口和所有子控件的右键都能被捕获
    this->installEventFilter(this);
    //qApp->installEventFilter(this);

    const auto children = this->findChildren<QWidget*>();
    for (QWidget *w : children) {
        w->installEventFilter(this);
    }
}

/** @brief 创建并连接页面全部周期定时器。 */
void MultimediaDemo::initializeTimers()
{
    LOG_INIT("开始初始化定时器");

    // 信号板数据到达时会立即刷新；该定时器仅作为低开销兜底。
    // onTimeout() 内部已做变化比较，相同楼层、方向和状态不会重复绘制。
    timer = new QTimer(this);
    timer->setInterval(300);
    connect(timer, &QTimer::timeout, this, &MultimediaDemo::onTimeout);
    timer->start();
    LOG_INIT("信号板UI兜底定时器已启动，间隔: 300ms");

    // 日期定时器
    date_timer = new QTimer(this);
    date_timer->setInterval(10000);
    connect(date_timer, &QTimer::timeout, this, &MultimediaDemo::onTimeoutDate);
    date_timer->start();
    onTimeoutDate();
    LOG_INIT("日期定时器已启动，间隔: 10000ms");

    // 空调定时器属于演示数据。正常运行或对应子项未使能时不创建、不启动。
    if (RunMode && AirCondi_en && AirCondiMode_en) {
        airCondiModeTimer = new QTimer(this);
        airCondiModeTimer->setInterval(10000);
        connect(airCondiModeTimer, &QTimer::timeout,
                this, &MultimediaDemo::onAirCondiModeTimeout);
        airCondiModeTimer->start();
        LOG_INIT("空调模式演示定时器已启动，间隔: 10000ms");
    }

    if (RunMode && AirCondi_en && AirCondiWind_en) {
        airCondiWindSpeedTimer = new QTimer(this);
        airCondiWindSpeedTimer->setInterval(10000);
        connect(airCondiWindSpeedTimer, &QTimer::timeout,
                this, &MultimediaDemo::onAirCondiWindSpeedTimeout);
        airCondiWindSpeedTimer->start();
        LOG_INIT("空调风速演示定时器已启动，间隔: 10000ms");
    }

    if (RunMode && AirCondi_en && AirCondiTemp_en) {
        airCondiTempTimer = new QTimer(this);
        airCondiTempTimer->setInterval(3000);
        connect(airCondiTempTimer, &QTimer::timeout,
                this, &MultimediaDemo::onAirCondiTempTimeout);
        airCondiTempTimer->start();
        LOG_INIT("空调温度演示定时器已启动，间隔: 3000ms");
    }

    LOG_INIT("定时器初始化完成");
}

/** @brief 演示模式下生成一帧模拟信号板数据。 */
void MultimediaDemo::simulateBufferData()
{
    LOG_BUFFER("开始初始化数据缓冲区");

    // 初始化缓冲区
    for(int i = 0; i < 10; i++) {
        my_buf[i] = 0;
    }

    // 程序刚启动，还没有收到信号板完整帧。
    m_signalBoardFrameReceived = false;

    LOG_BUFFER("数据缓冲区初始化完成，所有值设置为0");
}



// 信号板楼层、方向和状态显示。
// 信号到达时立即调用；300ms定时器只作为兜底，相同数据不会重复绘制。
void MultimediaDemo::onTimeout()
{
    if (!m_signalUiReady) {
        return;
    }

    // 1) 方向显示：仅在方向实际变化时更新。
    if (Diretion_en && m_signalBoardFrameReceived) {
        DirectionStatus targetDirection = LE;
        if (my_buf[1] == 0x31) {
            targetDirection = UP;
        } else if (my_buf[1] == 0x33) {
            targetDirection = DN;
        }

        if (targetDirection != dir_status) {
            dir_status = targetDirection;

            if (targetDirection == UP || targetDirection == DN) {
                const QString imagePath = targetDirection == UP
                        ? QStringLiteral(":/static/demoResources/images/tl/up.png")
                        : QStringLiteral(":/static/demoResources/images/tl/down.png");
                const QPixmap pixmap = cachedSignalUiPixmap(imagePath);

                if (!pixmap.isNull()) {
                    label_direction->setPixmap(pixmap);
                    label_direction->show();
                    label_direction->raise();
                    if (targetDirection == UP) {
                        LOG_SYSTEM("方向状态: 向上");
                    } else {
                        LOG_SYSTEM("方向状态: 向下");
                    }
                } else {
                    label_direction->hide();
                    LOG_SYSTEM("方向图片加载失败，方向标签已隐藏: " << imagePath);
                }
            } else {
                label_direction->hide();
                LOG_SYSTEM("方向状态: 停止");
            }
        }
    } else if (label_direction->isVisible()) {
        dir_status = LE;
        label_direction->hide();
    }

    // 2) 楼层显示：生成稳定键值，只在楼层内容变化时重绘。
    QString floorKey;
    if (!m_signalBoardFrameReceived) {
        floorKey = QStringLiteral("NO_FRAME");
    } else if (m_floorBlank) {
        floorKey = QStringLiteral("BLANK");
    } else if (isFloorUpperLetterAscii(m_floorLetter)) {
        floorKey = QStringLiteral("LETTER:%1").arg(QChar(m_floorLetter));
    } else if (m_floorPrefix == 'B' || m_floorPrefix == '-') {
        floorKey = QStringLiteral("PREFIX:%1:%2")
                .arg(QChar(m_floorPrefix))
                .arg(my_buf[4]);
    } else {
        floorKey = QStringLiteral("NUMBER:%1:%2").arg(my_buf[3]).arg(my_buf[4]);
    }

    if (floorKey != m_lastRenderedFloorKey) {
        m_lastRenderedFloorKey = floorKey;

        // 使用局部函数处理楼层。内部 return 只结束楼层绘制，不能跳过状态刷新。
        [this]() {
            if (!m_signalBoardFrameReceived || m_floorBlank) {
                label_ten->hide();
                label_one->hide();
                return;
            }

            const int ten = my_buf[3];
            const int one = my_buf[4];
            const int floorValue = ten * 10 + one;
            const bool isNegativeFloor = (m_floorPrefix == 'B' || m_floorPrefix == '-');
            const bool isLetterFloor = isFloorUpperLetterAscii(m_floorLetter);

            if (DisPlayPicOrText == 0) {
                // 单字母楼层 A-Z
                if (isLetterFloor) {
                    const QString letterPath = QString(":/static/demoResources/images/tl/%1.png")
                            .arg(QChar(m_floorLetter));
                    const QPixmap pxLetter = cachedSignalUiPixmap(letterPath);
                    label_ten->hide();

                    if (!pxLetter.isNull()) {
                        label_one->setText(QString());
                        showFloor(pxLetter, label_one,
                                  First_Floor_x, First_Floor_y,
                                  First_Floor_x_Size, First_Floor_y_Size);
                    } else {
                        displaySingleLetterFloorAsText(m_floorLetter);
                    }
                    return;
                }

                // 特殊正楼层
                if (SpecialFloor_en && !isNegativeFloor && tryShowSpecialFloorImage(floorValue)) {
                    return;
                }

                // 负楼层 / 地下楼层
                if (isNegativeFloor) {
                    const QString prefixPath = QString(":/static/demoResources/images/tl/%1.png")
                            .arg(QChar(m_floorPrefix));
                    const QString onePath = QString(":/static/demoResources/images/tl/%1.png").arg(one);
                    const QPixmap pxPrefix = cachedSignalUiPixmap(prefixPath);
                    const QPixmap pxOne = cachedSignalUiPixmap(onePath);

                    if (!pxPrefix.isNull() && !pxOne.isNull()) {
                        showFloor(pxPrefix, label_ten,
                                  First_FloorS_x - First_Floor_x_Size, First_FloorS_y,
                                  First_Floor_x_Size, First_Floor_y_Size);
                        showFloor(pxOne, label_one,
                                  First_FloorS_x, First_FloorS_y,
                                  First_Floor_x_Size, First_Floor_y_Size);
                    } else {
                        displayPrefixedFloorAsText(m_floorPrefix, one);
                    }
                    return;
                }

                // 普通数字楼层
                if (ten == 0) {
                    label_ten->hide();
                } else {
                    const QString tenPath = QString(":/static/demoResources/images/tl/%1.png").arg(ten);
                    const QPixmap pxTen = cachedSignalUiPixmap(tenPath);
                    if (!pxTen.isNull()) {
                        showFloor(pxTen, label_ten,
                                  First_FloorS_x - First_Floor_x_Size, First_Floor_y,
                                  First_Floor_x_Size, First_Floor_y_Size);
                    } else {
                        displayFloorAsText(floorValue, ten, one);
                        return;
                    }
                }

                const QString onePath = QString(":/static/demoResources/images/tl/%1.png").arg(one);
                const QPixmap pxOne = cachedSignalUiPixmap(onePath);
                if (!pxOne.isNull()) {
                    if (ten != 0) {
                        showFloor(pxOne, label_one,
                                  First_FloorS_x, First_FloorS_y,
                                  First_Floor_x_Size, First_Floor_y_Size);
                    } else {
                        showFloor(pxOne, label_one,
                                  First_Floor_x, First_FloorS_y,
                                  First_Floor_x_Size, First_Floor_y_Size);
                    }
                } else {
                    displayFloorAsText(floorValue, ten, one);
                }
            } else {
                if (isLetterFloor) {
                    displaySingleLetterFloorAsText(m_floorLetter);
                } else if (isNegativeFloor) {
                    displayPrefixedFloorAsText(m_floorPrefix, one);
                } else {
                    displayFloorAsText(floorValue, ten, one);
                }
            }
        }();
    }

    // 3) 状态显示：先确定目标状态，再判断是否需要更新，避免持续状态反复显隐。
    StateStatus targetState = NORMAL;
    if (m_signalBoardFrameReceived) {
        if (my_buf[2] == 0x31 && Flag1_en) {
            targetState = FULL;
        } else if (my_buf[2] == 0x32 && Flag1_en) {
            targetState = Overload;
        } else if (my_buf[2] == 0x33 && Flag2_en) {
            targetState = FIRE;
        } else if (my_buf[2] == 0x34 && Flag2_en) {
            targetState = REPAIR;
        }
    }

    if (targetState == state_status) {
        return;
    }

    state_status = targetState;
    if (targetState == NORMAL) {
        label_state->hide();
        LOG_SYSTEM("设备状态: 正常");
        return;
    }

    QString imagePath;
    QString fallbackText;
    QString fallbackStyle;
    int x = Flag1_x;
    int y = Flag1_y;
    int width = Flag1_x_Size;
    int height = Flag1_y_Size;

    switch (targetState) {
    case FULL:
        imagePath = QStringLiteral(":/static/demoResources/images/tl/full.png");
        fallbackText = QStringLiteral("人员已满");
        fallbackStyle = QStringLiteral("color: red; background-color: rgba(255, 255, 255, 128);");
        break;
    case Overload:
        imagePath = QStringLiteral(":/static/demoResources/images/tl/overload.png");
        fallbackText = QStringLiteral("设备超载");
        fallbackStyle = QStringLiteral("color: orange; background-color: rgba(255, 255, 255, 128);");
        break;
    case FIRE:
        imagePath = QStringLiteral(":/static/demoResources/images/tl/fire.png");
        fallbackText = QStringLiteral("火灾险情");
        fallbackStyle = QStringLiteral("color: orange; background-color: rgba(255, 255, 255, 128);");
        x = Flag2_x;
        y = Flag2_y;
        width = Flag2_x_Size;
        height = Flag2_y_Size;
        break;
    case REPAIR:
        imagePath = QStringLiteral(":/static/demoResources/images/tl/repair.png");
        fallbackText = QStringLiteral("维修中");
        fallbackStyle = QStringLiteral("color: orange; background-color: rgba(255, 255, 255, 128);");
        x = Flag2_x;
        y = Flag2_y;
        width = Flag2_x_Size;
        height = Flag2_y_Size;
        break;
    case NORMAL:
        return;
    }

    const QPixmap pixmap = cachedSignalUiPixmap(imagePath);
    if (!pixmap.isNull()) {
        label_state->setText(QString());
        label_state->setStyleSheet(QString());
        showImage(pixmap, label_state, imagePath, x, y, width, height);
    } else {
        label_state->setPixmap(QPixmap());
        label_state->setText(fallbackText);
        QFont font = label_state->font();
        font.setPointSize(16);
        font.setBold(true);
        label_state->setFont(font);
        label_state->setStyleSheet(fallbackStyle);
        label_state->setGeometry(x, y, width, height);
    }

    label_state->show();
    label_state->raise();
    LOG_SYSTEM("设备状态已更新: " << fallbackText);
}


/* 特殊楼层映射,解析ini配置 */
static inline QString stripInlineComment(QString s)
{
    int p = s.indexOf("//");
    if (p >= 0) s = s.left(p);
    p = s.indexOf(';');
    if (p >= 0) s = s.left(p);
    return s.trimmed();
}

/** @brief 兼容 1/0、true/false、yes/no 等配置布尔写法。 */
static inline bool parseBoolLoose(const QString& s, bool defVal=false)
{
    const QString v = stripInlineComment(s).toLower();
    if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
    if (v == "false" || v == "0" || v == "no" || v == "off") return false;
    return defVal;
}

/** @brief 从 INI 加载特殊楼层数值到显示名称的映射。 */
void MultimediaDemo::loadFloorRemap(const QString& iniPath)
{
    m_floorRemap.clear();

    QSettings cfg(iniPath, QSettings::IniFormat);

    cfg.beginGroup("SpecialFloor");

    // 1) 开关
    SpecialFloor_en = parseBoolLoose(cfg.value("SpecialFloor_en", "false").toString(), false);

    // 2) 特殊楼层区域
    SpecialFloor_x      = stripInlineComment(cfg.value("SpecialFloor_x", 0).toString()).toInt();
    SpecialFloor_y      = stripInlineComment(cfg.value("SpecialFloor_y", 0).toString()).toInt();
    SpecialFloor_x_Size = stripInlineComment(cfg.value("SpecialFloor_x_Size", 0).toString()).toInt();
    SpecialFloor_y_Size = stripInlineComment(cfg.value("SpecialFloor_y_Size", 0).toString()).toInt();

    // 3) old/new 映射
    for (int i = 0; ; ++i) {
        const QString kOld = QString("oldFloor%1").arg(i);
        const QString kNew = QString("newFloor%1").arg(i);

        if (!cfg.contains(kOld) || !cfg.contains(kNew)) break;

        const QString oldStr = stripInlineComment(cfg.value(kOld).toString());
        const QString newStr = stripInlineComment(cfg.value(kNew).toString());

        bool ok = false;
        const int oldFloor = oldStr.toInt(&ok);
        if (!ok || newStr.isEmpty()) continue;

        m_floorRemap.insert(oldFloor, newStr); // 14 -> "13A"
    }

    cfg.endGroup();

//    LOG_DEBUG("SpecialFloor_en=%d remapSize=%d" << (int)SpecialFloor_en << m_floorRemap.size());
//    for (auto it = m_floorRemap.begin(); it != m_floorRemap.end(); ++it) {
//        LOG_DEBUG("remap: %d -> [%s]" << it.key() << qPrintable(it.value()));
//    }
}


/** @brief 尝试显示当前楼层对应的专用图片。 */
bool MultimediaDemo::tryShowSpecialFloorImage(int Value)
{

    if (!SpecialFloor_en) return false;

    if (!m_floorRemap.contains(Value)) return false;

    const QString disp = m_floorRemap.value(Value);   // 例如 "13A"
    const QString combinedPath = QString(":/static/demoResources/images/tl/%1.png").arg(disp);

    // (1) 含十位：占用两格（宽=2*单格宽，高不变）
    QPixmap pxCombined = cachedSignalUiPixmap(combinedPath);
    if (!pxCombined.isNull()) {
        label_ten->hide();
        label_one->show();

        // 从“十位格”的x开始，宽度=两格
        showFloor(pxCombined, label_one,
                  First_FloorS_x - First_Floor_x_Size,  // 十位格位置
                  First_FloorS_y,
                  First_Floor_x_Size * 2,               // 两格宽
                  First_Floor_y_Size);

        LOG_DEBUG("特殊楼层合成图显示: floor=" << Value << " -> " << combinedPath);
        return true;
    }

    // (2) 不含十位：仅支持 "3A" :一位数字 + A 的拆分占位
    if (disp.size() == 2 && disp.endsWith('A') && disp[0].isDigit()) {
        const QString numPart = QString(disp[0]); // "3"
        const QString tenPath = QString(":/static/demoResources/images/tl/%1.png").arg(numPart);
        const QString onePath = QString(":/static/demoResources/images/tl/A.png");

        QPixmap pxTen = cachedSignalUiPixmap(tenPath);
        QPixmap pxOne = cachedSignalUiPixmap(onePath);

        if (!pxTen.isNull() && !pxOne.isNull()) {
            label_ten->show();
            label_one->show();

            showFloor(pxTen, label_ten,
                      First_FloorS_x - First_Floor_x_Size, First_FloorS_y,
                      First_Floor_x_Size, First_Floor_y_Size);

            showFloor(pxOne, label_one,
                      First_FloorS_x, First_FloorS_y,
                      First_Floor_x_Size, First_Floor_y_Size);

            LOG_DEBUG("特殊楼层拆分占位显示: floor=" << Value << " -> " << numPart << "+A");
            return true;
        }
    }

    // 合成图没有、且不满足拆分条件：回退原逻辑
    LOG_DEBUG("特殊楼层图片不可用，回退原显示: floor=" << Value << " mapped=" << disp);
    return false;
}


/** @brief 刷新日期、星期和时间文本。 */
void MultimediaDemo::onTimeoutDate()
{
    LOG_CONFIG("日期更新定时器触发");
    getTimeWithQt();
}

/** @brief 演示模式下轮换空调工作模式图标。 */
void MultimediaDemo::onAirCondiModeTimeout()
{
    if (!AirCondi_en || !AirCondiMode_en || !label_airCondiMode) {
        return;
    }

    // 切换模式 (1→2→3→1循环)
    currentAirCondiMode++;
    if (currentAirCondiMode > 3) {
        currentAirCondiMode = 1;
    }

    // 选择相对路径
    QString airRel;
    switch (currentAirCondiMode) {
    case 1: // cool
        airRel = "demoResources/images/air/cool.png";
        break;
    case 2: // fan
        airRel = "demoResources/images/air/fan.png";
        break;
    case 3: // hot
        airRel = "demoResources/images/air/hot.png";
        break;
    default:
        airRel = "demoResources/images/air/cool.png";
        currentAirCondiMode = 1;
        break;
    }

    // 外部优先、qrc 兜底：拼出两条候选路径
    const QString airPathExt = QDir(extRoot).filePath(airRel);
    const QString airPathQrc = QDir(qrcRoot).filePath(airRel);

    QPixmap pixmap;
    QString usedPath;

    // 加载参数
    Qt::ImageConversionFlags flags;
    flags |= Qt::AutoColor;

    // 1) 先尝试外部（存在/可读才尝试load，且load成功才算）
    {
        LOG_AC("尝试从外部目录加载空调模式图标: " << airPathExt);
        const QFileInfo fi(airPathExt);
        if (fi.exists() && fi.isFile() && fi.isReadable()) {
            if (pixmap.load(airPathExt, nullptr, flags) && !pixmap.isNull()) {
                usedPath = airPathExt;
            } else {
                LOG_AC("外部空调图标存在但加载失败(可能损坏/格式不对): " << airPathExt);
            }
        } else {
            LOG_AC("外部空调图标不可用(不存在/非文件/不可读): " << airPathExt);
        }
    }

    // 2) 外部失败 → qrc兜底
    if (pixmap.isNull()) {
        LOG_AC("尝试从qrc加载空调模式图标: " << airPathQrc);
        if (pixmap.load(airPathQrc, nullptr, flags) && !pixmap.isNull()) {
            usedPath = airPathQrc;
        }
    }

    if (pixmap.isNull()) {
        LOG_INIT("❌ 空调模式图片加载失败");
        LOG_INIT("尝试的外部路径: " << airPathExt);
        LOG_INIT("尝试的qrc路径: " << airPathQrc);
        return;
    }

    // 设置图标位置和大小
    label_airCondiMode->setPixmap(pixmap);
    label_airCondiMode->setScaledContents(true);
    label_airCondiMode->resize(AirCondiMode_x_Size, AirCondiMode_y_Size);
    label_airCondiMode->move(AirCondiMode_x, AirCondiMode_y);

    // 显式层级控制
    label_airCondiMode->raise();
    label_airCondiMode->show();

    LOG_AC("空调模式图标切换成功: 模式=" << currentAirCondiMode << ", 图片路径=" << usedPath);
    LOG_AC("图标位置: (" << AirCondiMode_x << ", " << AirCondiMode_y << "), 大小: " << AirCondiMode_x_Size << "x" << AirCondiMode_y_Size);

}

/** @brief 演示模式下轮换空调风速图标。 */
void MultimediaDemo::onAirCondiWindSpeedTimeout()
{
    if (!AirCondi_en || !AirCondiWind_en || !label_airCondiWindSpeed) {
        return;
    }

    // 切换风速 (1→2→3→4→1循环)
    currentAirCondiWindSpeed++;
    if (currentAirCondiWindSpeed > 4) {
        currentAirCondiWindSpeed = 1;
    }

    // 加载对应风速的图标（外部优先 + qrc兜底）
    QString airRel;

    switch (currentAirCondiWindSpeed) {
    case 1:
        airRel = "demoResources/images/air/level_1.png";
        break;
    case 2:
        airRel = "demoResources/images/air/level_2.png";
        break;
    case 3:
        airRel = "demoResources/images/air/level_3.png";
        break;
    case 4:
        airRel = "demoResources/images/air/level_4.png";
        break;
    default:
        airRel = "demoResources/images/air/level_1.png";
        currentAirCondiWindSpeed = 1;
        break;
    }

    const QString airPathExt = QDir(extRoot).filePath(airRel);
    const QString airPathQrc = QDir(qrcRoot).filePath(airRel);

    QPixmap pixmap;
    QString usedPath;
    bool loaded = false;

    // 1) 先尝试外部（存在/可读才读）
    LOG_AC("尝试从外部目录加载空调风速图片: " << airPathExt);
    {
        const QFileInfo fi(airPathExt);
        if (fi.exists() && fi.isFile() && fi.isReadable()) {
            QImageReader reader(airPathExt);
            QImage image = reader.read();
            if (!image.isNull()) {
                pixmap = QPixmap::fromImage(image);
                loaded = true;
                usedPath = airPathExt;
            } else {
                LOG_AC("外部空调风速图片存在但读取失败(可能损坏/格式不对): " << airPathExt);
            }
        } else {
            LOG_AC("外部空调风速图片不可用(不存在/非文件/不可读): " << airPathExt);
        }
    }

    // 2) 外部失败 → qrc兜底
    if (!loaded) {
        LOG_AC("尝试从qrc加载空调风速图片: " << airPathQrc);
        QImageReader reader(airPathQrc);
        QImage image = reader.read();
        if (!image.isNull()) {
            pixmap = QPixmap::fromImage(image);
            loaded = true;
            usedPath = airPathQrc;
        } else {
            LOG_AC("qrc 空调风速图片读取失败: " << airPathQrc);
        }
    }

    if (loaded) {
        // 设置图标位置和大小
        label_airCondiWindSpeed->setPixmap(pixmap);
        label_airCondiWindSpeed->move(AirCondiWind_x, AirCondiWind_y);
        label_airCondiWindSpeed->resize(AirCondiWind_x_Size, AirCondiWind_y_Size);
        label_airCondiWindSpeed->show();

        LOG_AC("空调风速图标切换成功: 风速=" << currentAirCondiWindSpeed << ", 图片路径=" << usedPath);
        LOG_AC("图标位置: (" << AirCondiWind_x << ", " << AirCondiWind_y
               << "), 大小: " << AirCondiWind_x_Size << "x" << AirCondiWind_y_Size);
    } else {
        LOG_AC("警告: 无法加载空调风速图片");
        LOG_AC("尝试的外部路径: " << airPathExt);
        LOG_AC("尝试的qrc路径: " << airPathQrc);
    }

}

/** @brief 演示模式下按当前方向调整空调温度。 */
void MultimediaDemo::onAirCondiTempTimeout()
{
    if (!AirCondi_en || !AirCondiTemp_en || !label_airCondiTemp) {
        return;
    }

    // 根据方向增减温度
    currentAirCondiTemp += tempDirection;

    // 检查温度边界，调整方向
    if (currentAirCondiTemp >= 27) {
        currentAirCondiTemp = 27;
        tempDirection = -1; // 开始降温
    } else if (currentAirCondiTemp <= 16) {
        currentAirCondiTemp = 16;
        tempDirection = 1; // 开始升温
    }

    // 更新温度显示
    label_airCondiTemp->setText(QString::number(currentAirCondiTemp) + "°C");
    // 确保颜色保持一致
    label_airCondiTemp->setStyleSheet("color: " + airCondiTempColor + ";");

    LOG_AC("空调温度更新成功: " << currentAirCondiTemp << "°C, 方向=" << tempDirection << ", 颜色=" << airCondiTempColor);
}

/** @brief 将配置英文单词转换为界面使用的首字母大写形式。 */
static QString toTitleCase(const QString& s)
{
    if (s.isEmpty()) return s;
    QString lower = s.toLower();
    lower[0] = lower[0].toUpper();
    return lower;
}

/** @brief 按配置样式格式化星期文本。 */
QString MultimediaDemo::formatWeekText(int dayOfWeek, int style)
{
    // Qt: dayOfWeek 1=Mon ... 7=Sun
    static const QString zhWeek[7] = { "星期一","星期二","星期三","星期四","星期五","星期六","星期日" };
    static const QString enFull[7] = { "Monday","Tuesday","Wednesday","Thursday","Friday","Saturday","Sunday" };
    static const QString enShort[7] = { "Mon","Tue","Wed","Thu","Fri","Sat","Sun" };

    if (dayOfWeek < 1 || dayOfWeek > 7) {
        return "未知";
    }

    const QString zh = zhWeek[dayOfWeek - 1];
    const QString full = enFull[dayOfWeek - 1];
    const QString shrt = enShort[dayOfWeek - 1];

    // 0: 简英全大写+中（MON 星期一）
    // 1: 中文（星期一）
    // 2: 简英头字大写+中（Mon 星期一）
    // 3: 全英文头字大写（Monday）
    // 4: 全英文全大写（MONDAY）
    // 5: 简英全大写（MON）
    // 6: 简英头字大写（Mon）
    switch (style) {
    case 0: return shrt.toUpper() + " " + zh;
    case 1: return zh;
    case 2: return toTitleCase(shrt) + " " + zh;   // shrt 本身已是 Mon，但这里更稳
    case 3: return toTitleCase(full);              // full 本身已是 Monday
    case 4: return full.toUpper();
    case 5: return shrt.toUpper();
    case 6: return toTitleCase(shrt);
    default:
        // 防御：未知值回退到 1 或 0
        return shrt.toUpper() + " " + zh;
    }
}

/** @brief 按配置的字号、颜色和粗体选项统一设置标签样式。 */
static void applyLabelStyle(QLabel* label,
                            int x, int y, int w, int h,
                            int textSize, int textStyleBold,
                            int r, int g, int b)
{
    if (!label) return;

    // 位置 + 尺寸
    label->setGeometry(x, y, w, h);

    // 字体
    QFont f = label->font();
    f.setPointSize(textSize);
    f.setBold(textStyleBold != 0);
    label->setFont(f);

    // 颜色
    //用QSS锁死颜色，防止用palette被覆盖导致初始化重置为黑色
    label->setStyleSheet(QString("color: rgb(%1,%2,%3);")
        .arg(r).arg(g).arg(b));
}

/** @brief 从系统时钟刷新日期、星期和时间标签。 */
void MultimediaDemo::getTimeWithQt()
{
    // 如果系统时间是1970，先尝试用RTC拉起来
    QString err;
    restoreSystemTime(&err);

    // 获取当前日期时间
    QDateTime current = QDateTime::currentDateTime();

    // 日期（yyyy-MM-dd）
    const QString currentDateText = current.toString("yyyy-MM-dd");
    LOG_CONFIG("当前日期: " << currentDateText);

    // 时间（s: HH:mm:ss）
    const QString current24Time = current.toString("HH:mm");
    LOG_CONFIG("当前时间(24h): " << current24Time);

    // 星期
    const int dayOfWeek = current.date().dayOfWeek(); // 1=Mon ... 7=Sun
    LOG_CONFIG("当前星期(数字): " << dayOfWeek);

    if (TimeDisplay_en) {
        // 文本
        label_time->setText(current24Time);

        applyLabelStyle(label_time,
                        TimeDisplay_x, TimeDisplay_y, TimeDisplay_x_Size, TimeDisplay_y_Size,
                        TimeTextSize, TimeTextStyle,
                        TimeTextColorR, TimeTextColorG, TimeTextColorB);

        label_time->setVisible(true);

        LOG_UI("时间标签已更新: " << current24Time
               << " bold=" << (TimeTextStyle != 0)
               << " size=" << TimeTextSize);
    } else {
        // 不显示
        if (label_time) label_time->setVisible(false);
    }

    // ===== 日期显示 =====
    if (DateDisplay_en) {
        label_date->setText(currentDateText);

        applyLabelStyle(label_date,
                        DateDisplay_x, DateDisplay_y, DateDisplay_x_Size, DateDisplay_y_Size,
                        DateTextSize, DateTextStyle,
                        DateTextColorR, DateTextColorG, DateTextColorB);

        label_date->setVisible(true);

        LOG_UI("日期标签已更新: " << currentDateText
               << " bold=" << (DateTextStyle != 0)
               << " size=" << DateTextSize);
    } else {
        if (label_date) label_date->setVisible(false);
    }

    // ===== 星期显示 =====
    if (WeekDisplay_en) {
        // week 文本按 WeekTextDispay 决定格式
        const QString weekText = formatWeekText(dayOfWeek, WeekTextDispay);
        label_week->setText(weekText);

        applyLabelStyle(label_week,
                        WeekDisplay_x, WeekDisplay_y, WeekDisplay_x_Size, WeekDisplay_y_Size,
                        WeekTextSize, WeekTextStyle,
                        WeekTextColorR, WeekTextColorG, WeekTextColorB);

        label_week->setVisible(true);

        LOG_UI("星期标签已更新: " << weekText
               << " (WeekTextDispay=" << WeekTextDispay << ")"
               << " bold=" << (WeekTextStyle != 0)
               << " size=" << WeekTextSize);
    } else {
        if (label_week) label_week->setVisible(false);
    }
}


/** @brief 按配置区域缩放并显示楼层图片。 */
void MultimediaDemo::showFloor(const QPixmap &pixmap, QLabel *label, int x, int y, int width, int height)
{
    LOG_UI("显示楼层标签 - 位置: (" << x << ", " << y << ") 大小: " << width << "x" << height);

    // 检查pixmap是否有效
    if (!pixmap.isNull()) {
        label->setText(QString());
        label->setStyleSheet(QString());
        label->setPixmap(pixmap);
        label->move(x, y);
        label->resize(width, height);
        label->show();
        LOG_UI("楼层图片显示成功 - 使用配置参数");
    } else {
        LOG_UI("警告: pixmap无效，无法显示楼层图片");
        // 可以在这里添加更多错误处理逻辑
        label->hide();
    }
}


/** @brief 用十位和个位标签显示普通数字楼层。 */
void MultimediaDemo::displayFloorAsText(int floorNumber, int tens, int units)
{
    LOG_UI("使用文字模式显示楼层: " << floorNumber);

    // 清除之前的图片显示
    label_ten->setPixmap(QPixmap());
    label_one->setPixmap(QPixmap());

    // 设置字体样式
    QFont font;
    font.setPointSize(48);
    font.setBold(true);

    // 单个数字显示（没有十位）
    if (tens == 0) {
        // 隐藏十位标签，只显示个位
        label_ten->hide();

        // 设置个位标签
        label_one->setText(QString::number(units));
        label_one->setFont(font);
        label_one->setStyleSheet("color: black; background-color: rgba(255, 255, 255, 128);");
        label_one->move(First_Floor_x, First_Floor_y);
        label_one->resize(First_Floor_x_Size, First_Floor_y_Size);
        label_one->show();

        LOG_UI("个位文字显示 - 位置: (" << First_Floor_x << ", " << First_Floor_y << ")");
    } else {
        // 两位数字显示
        // 设置十位标签
        label_ten->setText(QString::number(tens));
        label_ten->setFont(font);
        label_ten->setStyleSheet("color: black; background-color: rgba(255, 255, 255, 128);");
        label_ten->move(First_FloorS_x - First_Floor_x_Size, First_Floor_y);
        label_ten->resize(First_Floor_x_Size, First_Floor_y_Size);
        label_ten->show();

        // 设置个位标签
        label_one->setText(QString::number(units));
        label_one->setFont(font);
        label_one->setStyleSheet("color: black; background-color: rgba(255, 255, 255, 128);");
        label_one->move(First_FloorS_x, First_Floor_y);
        label_one->resize(First_Floor_x_Size, First_Floor_y_Size);
        label_one->show();

        LOG_UI("十位文字显示 - 位置: (" << (First_FloorS_x - First_Floor_x_Size) << ", " << First_Floor_y << ")");
        LOG_UI("个位文字显示 - 位置: (" << First_FloorS_x << ", " << First_Floor_y << ")");
    }
}


/** @brief 显示单字母楼层。 */
void MultimediaDemo::displaySingleLetterFloorAsText(char letter)
{
    if (!isFloorUpperLetterAscii(letter)) {
        return;
    }

    LOG_UI(QString("使用文字模式显示单字母楼层: %1").arg(QChar(letter)));

    label_ten->setPixmap(QPixmap());
    label_ten->clear();
    label_ten->hide();

    label_one->setPixmap(QPixmap());
    label_one->clear();
    label_one->setText(QString(QChar(letter)));

    QFont font;
    font.setPointSize(48);
    font.setBold(true);

    label_one->setFont(font);
    label_one->setAlignment(Qt::AlignCenter);
    label_one->setStyleSheet("color: black; background-color: rgba(255, 255, 255, 128);");

    label_one->move(First_Floor_x, First_Floor_y);
    label_one->resize(First_Floor_x_Size, First_Floor_y_Size);
    label_one->show();

    LOG_UI("单字母楼层文字显示 - 位置: ("
           << First_Floor_x << ", " << First_Floor_y << ")"
           << " 大小: " << First_Floor_x_Size << "x" << First_Floor_y_Size);
}


/** @brief 显示带 B 或负号前缀的楼层。 */
void MultimediaDemo::displayPrefixedFloorAsText(char prefix, int one)
{
    label_ten->setPixmap(QPixmap());
    label_ten->clear();
    label_ten->hide();

    label_one->setPixmap(QPixmap());
    label_one->clear();
    label_one->setText(QStringLiteral("%1%2").arg(QChar(prefix)).arg(one));

    QFont font;
    font.setPointSize(48);
    font.setBold(true);
    label_one->setFont(font);
    label_one->setAlignment(Qt::AlignCenter);
    label_one->setStyleSheet("color: black; background-color: rgba(255, 255, 255, 128);");
    label_one->setGeometry(First_Floor_x, First_Floor_y,
                           First_Floor_x_Size, First_Floor_y_Size);
    label_one->show();
}


/** @brief 优先解析外部资源路径，不存在时回退到 qrc。 */
QString MultimediaDemo::resolveResourcePath(const QString &pathOrRel) const
{
    QString relPath;

    // 传入 qrc 完整路径，例如 :/static/demoResources/images/tl/1.png
    if (pathOrRel.startsWith(qrcRoot)) {
        relPath = pathOrRel.mid(qrcRoot.length());

        if (relPath.startsWith('/')) {
            relPath.remove(0, 1);
        }
    }
    // 传入相对路径，例如 demoResources/images/tl/1.png
    else if (!pathOrRel.startsWith(":/") && !QDir::isAbsolutePath(pathOrRel)) {
        relPath = pathOrRel;
    }
    // 传入普通绝对路径，直接返回
    else {
        return pathOrRel;
    }

    const QString extPath = QDir(extRoot).filePath(relPath);
    const QString qrcPath = QDir(qrcRoot).filePath(relPath);

    const QFileInfo extInfo(extPath);
    if (extInfo.exists() && extInfo.isFile() && extInfo.isReadable()) {
        return extPath;
    }

    return qrcPath;
}

/** @brief 从缓存取得信号板 UI 图片，首次访问时加载。 */
QPixmap MultimediaDemo::cachedSignalUiPixmap(const QString &imagePath)
{
    const QString cacheKey = resolveResourcePath(imagePath);
    const auto it = m_signalUiPixmapCache.constFind(cacheKey);
    if (it != m_signalUiPixmapCache.constEnd()) {
        return it.value();
    }

    const QPixmap pixmap = loadPixmapWithIgnoreWarnings(imagePath);
    if (!pixmap.isNull()) {
        m_signalUiPixmapCache.insert(cacheKey, pixmap);
    }
    return pixmap;
}

/** @brief 启动时预加载楼层、方向和状态图片，降低串口更新抖动。 */
void MultimediaDemo::preloadSignalUiPixmaps()
{
    m_signalUiPixmapCache.clear();

    QStringList paths;
    for (int digit = 0; digit <= 9; ++digit) {
        paths << QString(":/static/demoResources/images/tl/%1.png").arg(digit);
    }
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        paths << QString(":/static/demoResources/images/tl/%1.png").arg(QChar(letter));
    }

    paths << QStringLiteral(":/static/demoResources/images/tl/B.png")
          << QStringLiteral(":/static/demoResources/images/tl/-.png")
          << QStringLiteral(":/static/demoResources/images/tl/up.png")
          << QStringLiteral(":/static/demoResources/images/tl/down.png")
          << QStringLiteral(":/static/demoResources/images/tl/full.png")
          << QStringLiteral(":/static/demoResources/images/tl/overload.png")
          << QStringLiteral(":/static/demoResources/images/tl/fire.png")
          << QStringLiteral(":/static/demoResources/images/tl/repair.png");

    int loadedCount = 0;
    for (const QString &path : paths) {
        if (!cachedSignalUiPixmap(path).isNull()) {
            ++loadedCount;
        }
    }

    LOG_INIT("信号板UI图片预加载完成: " << loadedCount << "/" << paths.size());
}


/** @brief 加载图片并屏蔽现场资源文件可能产生的非致命解码告警。 */
QPixmap MultimediaDemo::loadPixmapWithIgnoreWarnings(const QString &imagePath)
{
    QStringList candidates;

    // 如果传入的是 qrc 路径，则自动增加外部路径作为第一候选
    if (imagePath.startsWith(qrcRoot)) {
        QString relPath = imagePath.mid(qrcRoot.length());

        if (relPath.startsWith('/')) {
            relPath.remove(0, 1);
        }

        candidates << QDir(extRoot).filePath(relPath);
        candidates << imagePath;
    }
    // 如果传入的是资源相对路径，也按外部优先、qrc兜底处理
    else if (!imagePath.startsWith(":/") && !QDir::isAbsolutePath(imagePath)) {
        candidates << QDir(extRoot).filePath(imagePath);
        candidates << QDir(qrcRoot).filePath(imagePath);
    }
    // 普通绝对路径保持原逻辑
    else {
        candidates << imagePath;
    }

    candidates.removeDuplicates();

    for (const QString &path : candidates) {
        // 外部文件不存在或不可读时，不尝试读取，直接下一个候选
        if (!path.startsWith(":/")) {
            const QFileInfo fi(path);
            if (!fi.exists() || !fi.isFile() || !fi.isReadable()) {
                continue;
            }
        }

        QImageReader reader(path);
        QImage image = reader.read();

        if (!image.isNull()) {
            //LOG_DEBUG("图片加载成功: " << path);
            return QPixmap::fromImage(image);
        }

        LOG_DEBUG("图片读取失败: " << path
                  << ", error=" << reader.error()
                  << ", errorString=" << reader.errorString());
    }

    LOG_DEBUG("图片加载失败，所有候选路径均不可用: " << imagePath);
    return QPixmap();
}

/** @brief 设置标签几何并显示指定图片。 */
void MultimediaDemo::showImage(const QPixmap &pixmap, QLabel *label, const QString &imagePath, int x, int y, int width, int height)
{
    LOG_UI("显示图像 - 路径: " << imagePath << " 位置: (" << x << ", " << y << ") 大小: " << width << "x" << height);

    // 检查pixmap是否有效
    if (!pixmap.isNull()) {
        label->setPixmap(pixmap);
        label->move(x, y);
        label->resize(width, height);
        LOG_UI("图像显示成功");
    } else {
        LOG_UI("警告: pixmap无效，无法显示图像");
        label->hide();
    }
}


/** @brief 扫描图片目录并启动图片轮播。 */
void MultimediaDemo::initPicturePlayer()
{
    LOG_INIT("开始初始化图片播放器");

    // 图片标签的几何属性只设置一次。图片会在预加载阶段缩放到该尺寸，
    // 运行期间不再由 QLabel 动态缩放。
    const QSize targetSize(qMax(1, PicDisplay_x_Size),
                           qMax(1, PicDisplay_y_Size));
    label_picture->setGeometry(PicDisplay_x, PicDisplay_y,
                               targetSize.width(), targetSize.height());
    label_picture->setScaledContents(false);

    pictureFileList.clear();
    picturePixmapCache.clear();
    currentPictureIndex = 0;

    // 外部图片目录优先，目录不存在时使用 qrc 资源目录。
    QString pictureDirPath = QDir(extRoot).filePath("demoResources/images/pic");
    QDir pictureDir(pictureDirPath);
    if (!pictureDir.exists()) {
        LOG_FILE("外部图片目录不存在，尝试qrc目录:" << pictureDirPath);
        pictureDirPath = QDir(qrcRoot).filePath("demoResources/images/pic");
        pictureDir.setPath(pictureDirPath);
    }

    if (pictureDir.exists()) {
        const QStringList filters = QStringList()
                << "*.png" << "*.jpg" << "*.jpeg"
                << "*.bmp" << "*.gif";
        pictureDir.setNameFilters(filters);

        const QStringList fileNames = pictureDir.entryList(
                    QDir::Files | QDir::Readable, QDir::Name);
        for (const QString &fileName : fileNames) {
            pictureFileList.append(pictureDir.absoluteFilePath(fileName));
        }
    } else {
        LOG_FILE("警告: 外部和qrc图片目录均不存在:" << pictureDirPath);
    }

    LOG_IMAGE("扫描到图片文件数量:" << pictureFileList.size());
    LOG_IMAGE("图片文件列表:" << pictureFileList);

    // 在界面初始化阶段完成文件读取、图片解码和尺寸缩放。
    // 后续轮播只从 picturePixmapCache 取出 QPixmap，不再访问磁盘或解码图片。
    QList<QString> validPictureFiles;
    for (const QString &picturePath : pictureFileList) {
        QImageReader reader(picturePath);
        reader.setAutoTransform(true);
        reader.setScaledSize(targetSize);

        QImage image = reader.read();
        if (image.isNull()) {
            LOG_FILE("图片预加载失败:" << picturePath
                     << "error=" << reader.errorString());
            continue;
        }

        // 部分图片插件可能忽略 setScaledSize，这里保证缓存尺寸与标签一致。
        if (image.size() != targetSize) {
            image = image.scaled(targetSize,
                                 Qt::IgnoreAspectRatio,
                                 Qt::SmoothTransformation);
        }

        QPixmap pixmap = QPixmap::fromImage(image);
        if (pixmap.isNull()) {
            LOG_FILE("图片转换为QPixmap失败:" << picturePath);
            continue;
        }

        validPictureFiles.append(picturePath);
        picturePixmapCache.append(pixmap);
    }
    pictureFileList = validPictureFiles;

    if (picturePixmapCache.isEmpty()) {
        label_picture->clear();
        label_picture->hide();
        LOG_IMAGE("没有可显示的有效图片，图片播放器保持关闭");
        return;
    }

    // 第一张图片只设置一次。
    label_picture->setPixmap(picturePixmapCache.first());
    label_picture->show();
    LOG_IMAGE("图片预加载完成，有效图片数量:" << picturePixmapCache.size());

    // 只有一张图片时永久显示，不创建、不启动轮播定时器。
    if (picturePixmapCache.size() == 1) {
        LOG_IMAGE("仅有一张图片，保持静态显示，不启动图片定时器");
        return;
    }

    // 多张图片时，定时器回调仅切换缓存索引和 setPixmap。
    pictureTimer = new QTimer(this);
    pictureTimer->setTimerType(Qt::CoarseTimer);
    pictureTimer->setInterval(qMax(1, PicDisplay_Time) * 1000);
    connect(pictureTimer, &QTimer::timeout,
            this, &MultimediaDemo::playNextPicture);
    pictureTimer->start();

    LOG_IMAGE("图片轮播已启动，间隔秒数:" << qMax(1, PicDisplay_Time));
}

/** @brief 轮换普通图片显示区域中的下一张图片。 */
void MultimediaDemo::playNextPicture()
{
    const int pictureCount = picturePixmapCache.size();
    if (pictureCount <= 1) {
        return;
    }

    currentPictureIndex = (currentPictureIndex + 1) % pictureCount;
    label_picture->setPixmap(picturePixmapCache.at(currentPictureIndex));

    LOG_IMAGE("切换到缓存图片:" << pictureFileList.value(currentPictureIndex));
}

/** @brief 页面显示时恢复定时器、网络监测和本地播放。 */
void MultimediaDemo::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);

    m_localPlaybackEnabled = true;

    // 如果当前不是LIVE，并且本地列表为空，恢复空列表监测
    if (!m_isLiveMode && !m_liveDesired && videoFileList.isEmpty()) {
        startEmptyVideoListWatch();
    }

    qApp->installEventFilter(this);     // ✅ 只在本页可见时生效

}

/** @brief 页面隐藏时暂停不应在后台运行的交互状态。 */
void MultimediaDemo::hideEvent(QHideEvent *e)
{
    m_localPlaybackEnabled = false;
    stopEmptyVideoListWatch();

    // 离开本页时，彻底停止8秒隐藏逻辑
    if (m_panelAutoHideTimer) {
        m_panelAutoHideTimer->stop();
    }

    // 离开本页时不要保留“隐藏光标”状态到别的界面
    if (m_cursorHidden) {
        while (QApplication::overrideCursor()) {
            QApplication::restoreOverrideCursor();
        }
        m_cursorHidden = false;
    }

    // 避免切页面时提示残留
    if (m_passwordToastTimer) {
        m_passwordToastTimer->stop();
    }
    if (m_passwordToastLabel) {
        m_passwordToastLabel->hide();
    }


    qApp->removeEventFilter(this);      // ✅ 离开本页立刻不再影响其它界面
    QWidget::hideEvent(e);
}


// 事件是否属于本页
bool MultimediaDemo::isEventFromThisPage(QObject *watched, QEvent *event) const
{
    Q_UNUSED(event);

    if (!this->isVisible()) {
        return false;
    }

    QWidget *w = qobject_cast<QWidget*>(watched);
    if (!w) {
        return false;
    }

    // watched 就是本页，或者是本页的子控件，才算本页事件
    return (w == this) || this->isAncestorOf(w);
}


/** @brief 出现模态窗口时暂停页面空闲隐藏逻辑。 */
void MultimediaDemo::suspendUiIdleForModal()
{
    showCursorIfHidden();

    if (m_panelAutoHideTimer) {
        m_panelAutoHideTimer->stop();
    }
}


/** @brief 页面离开时停用空闲隐藏并恢复光标。 */
void MultimediaDemo::deactivateUiIdle()
{
    if (m_panelAutoHideTimer) {
        m_panelAutoHideTimer->stop();
    }

    if (m_volPanel && m_volPanel->isShown()) {
        m_volPanel->hideSlideOut();
    }

    if (m_netInfoPanel && m_netInfoPanel->isShown()) {
        m_netInfoPanel->hideSlideOut();
    }

    showCursorIfHidden();
}


// 右键触发密码 + 左键点击日期区域弹出校时
bool MultimediaDemo::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched);

    // 先过滤：不是 MultimediaDemo 页面及其子控件的事件，不处理
    if (!isEventFromThisPage(watched, event)) {
        return false;
    }

    // ★有任何弹窗/键盘/校时窗口在，主界面热点不工作
    if (m_uiModalActive) return false;

    // 任何鼠标活动：如果当前光标隐藏，则先显示；
    // 如果当前面板显示或光标显示状态存在，则重置8秒计时
    if (event->type() == QEvent::MouseMove) {
        // 光标隐藏时，唤醒光标
        if (m_cursorHidden) {
            showCursorIfHidden();
        } else {
            refreshUiIdleStateByActivity();
        }
    }
    else if (event->type() == QEvent::MouseButtonPress ||
             event->type() == QEvent::MouseButtonRelease ||
             event->type() == QEvent::Wheel) {
        refreshUiIdleStateByActivity();
    }

    // ★ 面板显示时：只要鼠标在面板内，任何鼠标事件都放行
    if ((m_volPanel && m_volPanel->isShown()) || (m_netInfoPanel && m_netInfoPanel->isShown())){
        if (event->type() == QEvent::MouseButtonPress ||
            event->type() == QEvent::MouseButtonRelease ||
            event->type() == QEvent::MouseMove) {

            auto *me = static_cast<QMouseEvent*>(event);
            //QPoint inPanel = m_volPanel->mapFromGlobal(me->globalPos());

            bool inVolPanel = false;
            bool inNetPanel = false;

            if (m_volPanel && m_volPanel->isShown()) {
                QPoint pt = m_volPanel->mapFromGlobal(me->globalPos());
                inVolPanel = m_volPanel->rect().contains(pt);
            }

            if (m_netInfoPanel && m_netInfoPanel->isShown()) {
                QPoint pt = m_netInfoPanel->mapFromGlobal(me->globalPos());
                inNetPanel = m_netInfoPanel->rect().contains(pt);
            }

            // 点在任意面板内部，放行
            if (inVolPanel || inNetPanel) {
                refreshUiIdleStateByActivity();
                return false;
            }

            // 点在外部，两个都收起
            if (event->type() == QEvent::MouseButtonPress &&
                me->button() == Qt::LeftButton) {

                if (m_volPanel && m_volPanel->isShown()) {
                    m_volPanel->hideSlideOut();
                }
                if (m_netInfoPanel && m_netInfoPanel->isShown()) {
                    m_netInfoPanel->hideSlideOut();
                }

                // 收起
                updateIdleTimerState();

                return true;
            }
        }
    }


    if (event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent*>(event);
        // 统一转成 this 的坐标：不依赖 watched，避免段错误/坐标乱
        const QPoint p = this->mapFromGlobal(me->globalPos());

        // 0) 中键：切换鼠标光标显示/隐藏
        if (me->button() == Qt::MiddleButton) {
            if (!m_cursorHidden) {
                QApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
                m_cursorHidden = true;
            } else {
                while (QApplication::overrideCursor()) {
                    QApplication::restoreOverrideCursor();
                }
                m_cursorHidden = false;
            }

            // 中键切换后维护8秒计时状态
            updateIdleTimerState();

            return true;
        }

        // 1) 右键：全屏触发密码
        if (me->button() == Qt::RightButton) {
            suspendUiIdleForModal();   // 先显示光标，并停掉8秒隐藏
            m_uiModalActive = true;

            const bool ok = requestPasswordAndCheck();

            m_uiModalActive = false;

            if (ok) {
                deactivateUiIdle();          // 真正离开前，先彻底关掉本页空闲逻辑
                emit backToMenuRequested();
            } else {
                // 键盘/校时结束后，回到本页逻辑
                updateIdleTimerState();    // 只有没离开本页，才恢复8秒逻辑
            }
            return true;
        }

        // 2) 左键：点中日期区域 -> 弹出设置日期/时间
        if (me->button() == Qt::LeftButton) {

            // A) 日期 / 时间 / 星期 任一区域命中 -> 弹出校时
            const QRect dateRect(DateDisplay_x, DateDisplay_y,
                                 DateDisplay_x_Size, DateDisplay_y_Size);

            const QRect timeRect(TimeDisplay_x, TimeDisplay_y,
                                 TimeDisplay_x_Size, TimeDisplay_y_Size);

            const QRect weekRect(WeekDisplay_x, WeekDisplay_y,
                                 WeekDisplay_x_Size, WeekDisplay_y_Size);

            const bool hitDate = DateDisplay_en && dateRect.contains(p);
            const bool hitTime = TimeDisplay_en && timeRect.contains(p);
            const bool hitWeek = WeekDisplay_en && weekRect.contains(p);

            if (hitDate || hitTime || hitWeek) {
                suspendUiIdleForModal();   // 先显示光标，并停掉8秒隐藏
                m_uiModalActive = true;

                openDateTimeSetting();

                m_uiModalActive = false;

                // 键盘/校时结束后，回到本页逻辑
                updateIdleTimerState();
                return true;
            }

            // B) 视频区域命中 -> 弹出音量滑动条
            if (m_volumeRect.contains(p)) {
                const bool volShown = (m_volPanel && m_volPanel->isShown());
                const bool netShown = (m_netInfoPanel && m_netInfoPanel->isShown());

                // 已显示时，左键再次点击立即收回
                if (volShown || netShown) {
                    if (m_volPanel && volShown) {
                        m_volPanel->hideSlideOut();
                    }
                    if (m_netInfoPanel && netShown) {
                        m_netInfoPanel->hideSlideOut();
                    }

                    // 手动收回时，停止8秒定时
                    updateIdleTimerState();
                } else {
                    // 显示音量
                    int v = m_ampCtrl->loadVolumeFromFile();
                    m_volPanel->setCurrentVolume(v);
                    m_volPanel->showSlideIn();

                    // 显示网络信息
                    readNetConfiguration();
                    m_netInfoPanel->setInfo(m_netIp,
                                            m_mqttClientId,
                                            m_mqttRouteName,
                                            m_mqttHost,
                                            m_mqttSubTopics,
                                            m_streamUrl);

                    // 弹出后开始8秒倒计时
                    m_netInfoPanel->showSlideIn();
                    updateIdleTimerState();
                }

                return true;
            }

            // 空白区域左键：不处理，放行
            return false;
        }
    }

    return false; // 默认放行
}

/** @brief 使用屏幕键盘请求退出密码并与配置值校验。 */
bool MultimediaDemo::requestPasswordAndCheck()
{
    bool ok = false;
    const QString entered = KeyboardDialog::getText(
        this,
        QStringLiteral("请输入密码"),
        QString(),                 // 初始文本
        QLineEdit::Password,        // ✅ 密码回显模式
        32,                        // 最大长度
        &ok
    );

    if (!ok) return false; // 用户取消

    const QString correct = "123456"; // 或从配置读取

    if (entered == correct) return true;

    showPasswordErrorToast(QStringLiteral("密码错误"), 1800);
    return false;
}

bool MultimediaDemo::allowsPresenceSwitch() const
{
    return !m_uiModalActive;
}

//设置时间窗口
void MultimediaDemo::openDateTimeSetting()
{
    // 覆盖层：在当前窗口内绘制（不是顶层窗口）
    QWidget overlay(this);
    overlay.setAttribute(Qt::WA_TranslucentBackground, true);
    overlay.setAttribute(Qt::WA_NoSystemBackground, true);
    overlay.setGeometry(this->rect());
    overlay.show();
    overlay.raise();

    // 圆角面板
    QWidget panel(&overlay);
    panel.setObjectName("panel");
    panel.setFixedSize(440, 260);
    panel.move((overlay.width() - panel.width()) / 2,
               (overlay.height() - panel.height()) / 2);
    panel.setAttribute(Qt::WA_StyledBackground, true);

    // 轻量样式：只做圆角与基础配色，不加阴影/动画
    overlay.setStyleSheet(
        "QWidget#panel { background:#101418; border-radius:18px; }"
        "QLabel { color:#E8EAED; }"
        "QDateEdit, QTimeEdit {"
        "  background:#1B2229; color:#E8EAED;"
        "  border:1px solid #2C3944; border-radius:10px;"
        "  padding:8px 12px;"
        "}"
        "QPushButton { border-radius:10px; padding:10px 18px; font-weight:600; }"
        "QPushButton#okBtn { background:#2D7DFF; color:white; }"
        "QPushButton#cancelBtn { background:#25303A; color:#E8EAED; }"
    );

    auto *main = new QVBoxLayout(&panel);
    main->setContentsMargins(18, 16, 18, 16);
    main->setSpacing(14);

    auto *title = new QLabel(QStringLiteral("设置日期和时间"), &panel);
    QFont titleFont = title->font();
    titleFont.setPointSize(20);
    titleFont.setBold(true);
    title->setFont(titleFont);
    main->addWidget(title);

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(12);

    auto *labDate = new QLabel(QStringLiteral("日期"), &panel);
    auto *labTime = new QLabel(QStringLiteral("时间"), &panel);

    QFont labFont = labDate->font();
    labFont.setPointSize(16);
    labFont.setBold(true);
    labDate->setFont(labFont);
    labTime->setFont(labFont);

    auto *dateEdit = new QDateEdit(&panel);
    dateEdit->setDisplayFormat("yyyy-MM-dd");
    dateEdit->setCalendarPopup(false);
    dateEdit->setWrapping(true);                // 自动回绕
    dateEdit->setDate(QDate::currentDate());
    dateEdit->setMinimumHeight(52);

    auto *timeEdit = new QTimeEdit(&panel);
    timeEdit->setDisplayFormat("HH:mm:ss");
    timeEdit->setWrapping(true);
    timeEdit->setTime(QTime::currentTime());
    timeEdit->setMinimumHeight(52);

    QFont editFont = dateEdit->font();
    editFont.setPointSize(20);
    editFont.setBold(true);
    dateEdit->setFont(editFont);
    timeEdit->setFont(editFont);

    grid->addWidget(labDate, 0, 0);
    grid->addWidget(dateEdit, 0, 1);
    grid->addWidget(labTime, 1, 0);
    grid->addWidget(timeEdit, 1, 1);
    grid->setColumnStretch(1, 1);

    main->addLayout(grid);

    // 按钮
    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch(1);

    auto *btnCancel = new QPushButton(QStringLiteral("取消"), &panel);
    auto *btnOk     = new QPushButton(QStringLiteral("确认"), &panel);
    btnCancel->setObjectName("cancelBtn");
    btnOk->setObjectName("okBtn");
    btnCancel->setMinimumSize(120, 46);
    btnOk->setMinimumSize(120, 46);

    btnRow->addWidget(btnCancel);
    btnRow->addWidget(btnOk);
    main->addLayout(btnRow);

    // 用事件循环模拟 exec，避免顶层窗口黑角问题
    bool accepted = false;
    QEventLoop loop;

    QObject::connect(btnCancel, &QPushButton::clicked, [&](){
        accepted = false;
        loop.quit();
    });
    QObject::connect(btnOk, &QPushButton::clicked, [&](){
        accepted = true;
        loop.quit();
    });

    panel.show();
    panel.raise();

    // 监测用户点击 OK 或 Cancel
    loop.exec();
    if (!accepted) return;

    // 合成用户选择的本地时间
    const QDate d = dateEdit->date();
    const QTime t = timeEdit->time();
    const QDateTime userLocal(QDate(d.year(), d.month(), d.day()),
                              QTime(t.hour(), t.minute(), t.second()),
                              Qt::LocalTime);

    QString err;

    // 先设置系统时间Qt读取
    if (!setSystemTimeFromLocal(userLocal, &err)) {
        QMessageBox::warning(this, QStringLiteral("失败"),
                             QStringLiteral("设置系统时间失败：") + err);
        return;
    }

    // 再写入 RTC
    if (!setRtcFromLocal(userLocal, &err)) {
        QMessageBox::warning(this, QStringLiteral("失败"),
                             QStringLiteral("写入RTC失败：") + err);
        return;
    }

    // 立即刷新时间
    getTimeWithQt();
}

// 重置时间,防止系统刚启动时系统时间还没被 RTC 同步好，UI 刷新
bool MultimediaDemo::restoreSystemTime(QString* err)
{
    // 先看系统时间是否明显是 1970（或接近 1970）
    QDateTime now = QDateTime::currentDateTime();
    if (now.date().year() > 1971) {
        return true; // 系统时间看起来正常
    }

    const QString rtcPath = Rk3566Platform::rtcDevice();
    int fd = ::open(QFile::encodeName(rtcPath).constData(), O_RDONLY);
    if (fd < 0) {
        if (err) *err = QStringLiteral("open(%1)失败: ").arg(rtcPath) + QString::fromLocal8Bit(strerror(errno));
        return false;
    }

    rtc_time rt{};
    int rc = ::ioctl(fd, RTC_RD_TIME, &rt);
    ::close(fd);

    if (rc < 0) {
        if (err) *err = QStringLiteral("ioctl(RTC_RD_TIME)失败: ") + QString::fromLocal8Bit(strerror(errno));
        return false;
    }

    // 把 RTC 读到的时间当成 UTC
    QDate d(rt.tm_year + 1900, rt.tm_mon + 1, rt.tm_mday);
    QTime t(rt.tm_hour, rt.tm_min, rt.tm_sec);
    if (!d.isValid() || !t.isValid()) {
        if (err) *err = QStringLiteral("RTC读到的时间无效");
        return false;
    }

    QDateTime rtcUtc(d, t, Qt::UTC);

    // 如果 RTC 本身也是 1970，那说明 RTC 没被正确保持（硬件/供电/rtc1 指向问题）
    if (rtcUtc.date().year() <= 1971) {
        if (err) *err = QStringLiteral("RTC读到的时间也是1970附近，无法用RTC修复系统时间");
        return false;
    }

    // 用 RTC 的 UTC 写回系统时间
    timespec ts{};
    ts.tv_sec = rtcUtc.toSecsSinceEpoch();
    ts.tv_nsec = 0;

    if (::clock_settime(CLOCK_REALTIME, &ts) != 0) {
        if (err) *err = QString::fromLocal8Bit(strerror(errno));
        return false;
    }

    return true;
}

/* 设置系统时间 */
bool MultimediaDemo::setSystemTimeFromLocal(const QDateTime& localDT, QString* err)
{
    return Rk3566Platform::setSystemDateTime(localDT, err);
}


/* 写入 RTC */
bool MultimediaDemo::setRtcFromLocal(const QDateTime& localDT, QString* err)
{
    Q_UNUSED(localDT);
    return Rk3566Platform::syncRtcFromSystem(err);
}


/** @brief 将完整信号板状态映射为楼层、方向和状态 UI。 */
void MultimediaDemo::onSignalBoardState(const SignalBoardState& st)
{
    if (st.raw.size() != 12) return;
    if ((unsigned char)st.raw[0] != 0x02 || (unsigned char)st.raw[11] != 0x03) return;

    // 该槽通过 Qt::QueuedConnection 在GUI线程执行，可安全地立即刷新界面。
    m_signalBoardFrameReceived = true;
    my_buf[1] = st.dirAscii;
    my_buf[2] = st.modeAscii;

    my_buf[3] = 0;
    my_buf[4] = 0;
    m_floorBlank = false;
    m_floorPrefix = 0;
    m_floorLetter = 0;

    if (!st.floorLetter.isNull()) {
        // 单字母楼层：A、M、G 等
        m_floorLetter = st.floorLetter.toLatin1();
    } else if (!st.floorPrefix.isNull()) {
        // 负层 / 地下层：-1、B1
        m_floorPrefix = st.floorPrefix.toLatin1();
        my_buf[4] = st.floorOneDigit;
    } else if (st.floorValid) {
        // 普通数字楼层：11、05、/5、 5
        my_buf[3] = st.floorTenOrPrefix;
        my_buf[4] = st.floorOneDigit;
    } else {
        // 空楼层 / 无效楼层
        m_floorBlank = true;
    }

    // 完整帧一到立即更新；内部变化检测会过滤重复帧。
    if (m_signalUiReady) {
        onTimeout();
    }
}



/** @return 配置允许持久化在线播放状态时返回 true。 */
bool MultimediaDemo::isOnlinePlaybackPersistEnabled() const
{
    if (!QFile::exists(netCfgPath())) {
        return false;
    }

    QSettings ini(netCfgPath(), QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    const bool offlineV1 = ini.value("feature/offline_v1", false).toBool();
    const bool onlineV1  = ini.value("feature/online_v1", false).toBool();
    const bool onlineV2  = ini.value("feature/online_v2", false).toBool();

    // offline_v1 下不能写入/恢复播放模式；只有 MQTT 网络链路启用时才允许持久化。
    return !offlineV1 && (onlineV1 || onlineV2);
}

/** @brief 把期望模式、实际模式和切换原因写入网络配置。 */
void MultimediaDemo::persistPlaybackStateToNetCfg(const QString &targetMode,
                                                  const QString &currentMode,
                                                  const QString &reason)
{
    if (!isOnlinePlaybackPersistEnabled()) {
        LOG_CONFIG("当前不是 online_v1/online_v2 模式，跳过播放状态持久化");
        return;
    }

    const QString target = normalizePlaybackModeForCfg(targetMode);
    const QString current = normalizePlaybackModeForCfg(currentMode);
    if (target.isEmpty() || current.isEmpty()) {
        LOG_CONFIG("播放状态持久化失败：非法模式 target=" << targetMode << " current=" << currentMode);
        return;
    }

    QFileInfo fi(netCfgPath());
    if (!fi.exists() || !fi.isFile()) {
        LOG_CONFIG("播放状态持久化失败：net_cfg.ini 不存在");
        return;
    }
    if (!fi.isWritable()) {
        LOG_CONFIG("播放状态持久化失败：net_cfg.ini 不可写");
        return;
    }

    const QString liveUrl = !m_desiredLiveUrl.trimmed().isEmpty()
            ? m_desiredLiveUrl.trimmed()
            : m_streamUrl.trimmed();
    QString liveKind = !m_desiredLiveKind.trimmed().isEmpty()
            ? m_desiredLiveKind.trimmed().toLower()
            : detectLiveKindFromUrlForCfg(liveUrl);

    QSettings ini(netCfgPath(), QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    ini.beginGroup("playback");
    ini.setValue("command_received", true);          // 只有收到 LIVE/RECORDED/NONE 后才会创建该标记
    ini.setValue("target_mode", target);             // 云端期望状态：LIVE/RECORDED/NONE
    ini.setValue("current_mode", current);           // 当前实际状态：LIVE/RECORDED/NONE
    ini.setValue("fallback_from_live", target == QStringLiteral("LIVE") && current == QStringLiteral("RECORDED"));
    ini.setValue("live_source_validated", m_liveSourceValidated);
    ini.setValue("last_reason", reason);
    ini.setValue("updated_at", QDateTime::currentDateTime().toString(Qt::ISODate));
    if (!liveUrl.isEmpty()) {
        ini.setValue("live_url", liveUrl);
        ini.setValue("live_kind", liveKind);
    }
    ini.endGroup();

    // LIVE 指令下同步维护 [stream].url，保证重启恢复时即使 MQTT 未重新下发也能拉回原直播流。
    if (target == QStringLiteral("LIVE") && !liveUrl.isEmpty()) {
        ini.beginGroup("stream");
        ini.setValue("url", liveUrl);
        ini.endGroup();
    }

    ini.sync();
    if (ini.status() != QSettings::NoError) {
        LOG_CONFIG("播放状态持久化失败：QSettings sync error=" << ini.status());
        return;
    }

    LOG_CONFIG("播放状态已写入 net_cfg.ini target=" << target
               << " current=" << current
               << " fallback_from_live=" << (target == QStringLiteral("LIVE") && current == QStringLiteral("RECORDED"))
               << " reason=" << reason);

    readNetConfiguration();
    if (m_netInfoPanel) {
        m_netInfoPanel->setInfo(m_netIp, m_mqttClientId, m_mqttRouteName,
                                m_mqttHost, m_mqttSubTopics, m_streamUrl);
    }
}

/** @brief 启动时从网络配置恢复上次播放目标。 */
void MultimediaDemo::restorePlaybackStateFromNetCfg()
{
    if (!isOnlinePlaybackPersistEnabled()) {
        LOG_CONFIG("当前不是 online_v1/online_v2 模式，跳过播放状态恢复");
        return;
    }

    QSettings ini(netCfgPath(), QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    ini.beginGroup("playback");
    const bool commandReceived = ini.value("command_received", false).toBool();
    const QString target = normalizePlaybackModeForCfg(ini.value("target_mode").toString());
    QString liveUrl = ini.value("live_url").toString().trimmed();
    QString liveKind = ini.value("live_kind").toString().trimmed().toLower();
    const bool liveSourceValidated = ini.value("live_source_validated", false).toBool();
    ini.endGroup();

    // 开机默认没有 [playback].command_received 时，不主动写、不主动切，保持原本本地播放逻辑。
    if (!commandReceived || target.isEmpty()) {
        LOG_CONFIG("未发现云端播放指令持久化记录，保持默认播放逻辑");
        return;
    }

    if (target == QStringLiteral("LIVE")) {
        if (liveUrl.isEmpty()) {
            liveUrl = ini.value("stream/url").toString().trimmed();
        }
        if (liveUrl.isEmpty()) {
            LOG_CONFIG("恢复 LIVE 失败：[playback].live_url 与 [stream].url 均为空");
            return;
        }
        if (liveKind.isEmpty()) {
            liveKind = detectLiveKindFromUrlForCfg(liveUrl);
        }

        m_liveDesired = true;
        m_desiredLiveUrl = liveUrl;
        m_desiredLiveKind = liveKind;
        m_liveSourceValidated = liveSourceValidated;
        m_liveRecovering = false;
        m_leaving = false;

        startLiveRetry();

        QTimer::singleShot(0, this, [this]() {
            if (!m_liveDesired || m_desiredLiveUrl.trimmed().isEmpty()) {
                return;
            }

            // 重启恢复时不要无条件直接打开 LIVE。
            // 如果网线/交换机/上游组播还没恢复，直接进入 GStreamer LIVE 容易再次卡死。
            // 保持默认本地轮播，后台定时探测；网络恢复后 probeLiveStream() 会切回 LIVE。
            if (!isWiredLinkUpForLiveRetry()) {
                persistPlaybackStateToNetCfg(QStringLiteral("LIVE"), QStringLiteral("RECORDED"),
                                             QStringLiteral("restore_live_wait_link_down"));
                LOG_VIDEO("恢复 LIVE 目标但网口未连通，保持本地轮播并后台监测:" << m_desiredLiveUrl);
                return;
            }

            probeLiveStream(m_desiredLiveUrl);
        });

        LOG_CONFIG("已根据 net_cfg.ini 恢复 LIVE 目标，等待后台探测: " << liveUrl << " kind=" << liveKind);
        return;
    }

    if (target == QStringLiteral("RECORDED")) {
        QTimer::singleShot(0, this, [this]() {
            switchToRecordedMode();
        });
        LOG_CONFIG("已根据 net_cfg.ini 恢复 RECORDED 目标");
        return;
    }

    if (target == QStringLiteral("NONE")) {
        QTimer::singleShot(0, this, [this]() {
            stopAllPlayback();
        });
        LOG_CONFIG("已根据 net_cfg.ini 恢复 NONE 目标");
        return;
    }
}


/** @brief 保存直播地址和协议类型（如 rtsp、hls），但不立即切换。 */
void MultimediaDemo::setStreamUrl(const QString& url, const QString& kind)
{

    const QString u = url.trimmed();
    if (u.isEmpty()) return;

    // 判定是不是直播：从 FtpPage 传过来的 kind 已经是 rtsp/hls/rtmp...
    const QString k = kind.toLower();
    const bool isLive = (k == "rtsp" || k == "hls" || k == "rtmp" || k == "rtmps" ||
                         k == "srt" || k == "udp" || k == "live" || k == "http-ts" ||
                         k == "multicast" || k == "rtsp-multicast" || k == "rtsp_multicast");

    if (!isLive) {
        // 非LIVE：清除直播目标，停止重试
        stopDesiredLive();
        LOG_VIDEO("收到非LIVE流，停止直播重试");
        return;
    }

    // 组播场景下服务端/云端可能重复下发同一个 LIVE，不能在已经打开/播放时反复 setMedia。
    if (m_liveDesired && m_desiredLiveUrl == u && m_desiredLiveKind == k &&
        (m_isLiveMode || m_liveSwitching)) {
        LOG_VIDEO("LIVE目标未变化且正在打开/播放，忽略重复切换:" << u);
        emit statusMessageRequested(m_liveSwitching
                                        ? QStringLiteral("当前正在切换到该直播流")
                                        : QStringLiteral("当前已处于该直播模式"),
                                    3000);
        return;
    }

    // 新的LIVE地址必须重新用真实解码帧验证，不能只凭TCP端口可达判定。
    const bool sourceChanged = !m_liveDesired ||
                               m_desiredLiveUrl != u ||
                               m_desiredLiveKind != k;

    // LIVE目标：保存“业务目标”
    m_liveDesired = true;
    m_desiredLiveUrl = u;
    m_desiredLiveKind = k;
    if (sourceChanged) {
        m_liveSourceValidated = false;
        m_liveProbeFailureLogElapsed.invalidate();
        m_liveProbeFailureLogUrl.clear();
    }

    persistPlaybackStateToNetCfg(QStringLiteral("LIVE"), QStringLiteral("LIVE"), QStringLiteral("live_received"));

    LOG_VIDEO("收到LIVE目标，准备播放/重试: " << m_desiredLiveUrl << " kind=" << m_desiredLiveKind);

    if (m_modulePlaybackSuspended) {
        m_resumeLiveAfterModuleSwitch = true;
        m_resumeLiveDirectlyAfterModuleSwitch = true;
        LOG_VIDEO("多媒体模块当前已暂停，仅记录LIVE目标，返回界面后恢复:" << m_desiredLiveUrl);
        return;
    }

    // 第一次收到 LIVE，可以立即切一次
    switchToLiveStream(m_desiredLiveUrl, m_desiredLiveKind);

    //tryStartLiveStream(m_desiredLiveUrl, m_desiredLiveKind);

    // 后续失败后再走后台探测重试
    startLiveRetry();
}


/** @brief 使用独立播放器预探测直播首帧，成功后再切换主播放器。 */
void MultimediaDemo::probeLiveStream(const QString& url)
{
    const QString probeUrl = url.trimmed();
    if (probeUrl.isEmpty()) return;
    if (m_liveProbeRunning) return;

    const bool logProbeAttempt = !m_liveProbeFailureLogElapsed.isValid() ||
            m_liveProbeFailureLogUrl != probeUrl ||
            m_liveProbeFailureLogElapsed.elapsed() >= kLiveProbeFailureLogIntervalMs;

    if (!isWiredLinkUpForLiveRetry()) {
        LOG_VIDEO("后台探测直播流跳过：网口 carrier=0，继续本地轮播等待网线恢复");
        return;
    }

    const QUrl u(probeUrl);
    QString scheme = u.scheme().toLower();
    QString desiredKind = m_desiredLiveKind.trimmed().toLower();
    if (desiredKind.isEmpty() || desiredKind == QStringLiteral("live")) {
        desiredKind = detectLiveKindFromUrlForCfg(probeUrl);
    }
    if (scheme.isEmpty()) {
        scheme = desiredKind;
    }

    auto switchIfStillWanted = [this, probeUrl, desiredKind, scheme](const QString &detail) {
        // 一旦探测成功，结束本轮失败日志限流；以后再次失败时立即打印首条提示。
        m_liveProbeFailureLogElapsed.invalidate();
        m_liveProbeFailureLogUrl.clear();

        if (!m_liveDesired || m_desiredLiveUrl.trimmed() != probeUrl || m_isLiveMode) {
            LOG_VIDEO("后台探测直播流成功，但LIVE目标已变化，忽略切换:" << probeUrl);
            return;
        }

        // 轻量探测是异步的；其回调可能晚于人体感应的模块切换。
        // 人脸模块活动期间不能在后台重建解码链，只记录返回后直接恢复。
        if (m_modulePlaybackSuspended) {
            m_resumeLiveAfterModuleSwitch = true;
            m_resumeLiveDirectlyAfterModuleSwitch = true;
            LOG_VIDEO("后台探测直播流成功，但多媒体模块已暂停，等待返回后恢复:"
                      << probeUrl << detail);
            return;
        }

        const QString kind = desiredKind.isEmpty()
                ? (scheme.isEmpty() ? detectLiveKindFromUrlForCfg(probeUrl) : scheme)
                : desiredKind;

        LOG_VIDEO("后台探测直播流确认可用，准备切回正式LIVE:" << probeUrl << detail);
        switchToLiveStream(probeUrl, kind);
    };

    auto startUdpTrafficProbeIfPossible = [this, probeUrl, switchIfStillWanted](const UdpLiveProbeTargetForLiveRetry &target) {
        if (!target.ok || target.port == 0) {
            LOG_VIDEO("后台 UDP 直播探测失败：URL 中没有有效端口，继续本地轮播:" << probeUrl);
            return;
        }

        m_liveProbeRunning = true;

        QUdpSocket *udp = new QUdpSocket(this);
        udp->setProperty("yc_udp_probe_done", false);

        auto finishUdpProbe = [this, udp, probeUrl, switchIfStillWanted](bool ok, const QString &detail) {
            if (!udp || udp->property("yc_udp_probe_done").toBool()) {
                return;
            }

            udp->setProperty("yc_udp_probe_done", true);
            m_liveProbeRunning = false;

            udp->close();
            udp->deleteLater();

            if (!ok) {
                // 网线 carrier=1 但没有有效 UDP 媒体包时，保持本地轮播。
                // 不在后台反复重启；只有已经切到 LIVE 后 watchdog 发现无真实帧，才记录状态并重启。
                LOG_VIDEO("后台 UDP 直播探测失败:" << detail << " url=" << probeUrl);
                return;
            }

            switchIfStillWanted(detail);
        };

        const bool bindOk = udp->bind(QHostAddress::AnyIPv4, target.port,
                                      QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
        if (!bindOk) {
            finishUdpProbe(false, QStringLiteral("udp bind failed port=") + QString::number(target.port) +
                           QStringLiteral(" error=") + udp->errorString());
            return;
        }

        if (target.multicast) {
            const bool joinOk = udp->joinMulticastGroup(QHostAddress(target.host));
            if (!joinOk) {
                finishUdpProbe(false, QStringLiteral("join multicast group failed group=") + target.host +
                               QStringLiteral(" port=") + QString::number(target.port) +
                               QStringLiteral(" error=") + udp->errorString());
                return;
            }
        }

        connect(udp, &QUdpSocket::readyRead, this, [udp, finishUdpProbe]() {
            while (udp->hasPendingDatagrams()) {
                QByteArray datagram;
                const qint64 pendingSize = udp->pendingDatagramSize();
                datagram.resize(static_cast<int>(qMin<qint64>(pendingSize, 2048)));
                udp->readDatagram(datagram.data(), datagram.size());
            }
            finishUdpProbe(true, QStringLiteral("UDP media packet received"));
        });

        QTimer::singleShot(3500, udp, [finishUdpProbe]() {
            finishUdpProbe(false, QStringLiteral("no UDP media packet within timeout"));
        });

        LOG_VIDEO("后台 UDP 直播探测等待媒体包:" << probeUrl
                  << " group/host=" << target.host
                  << " port=" << target.port
                  << " multicast=" << target.multicast);
    };

    // 原始 UDP 组播不能只看 carrier。网线插着但上游没流量时，carrier-only 会误切 LIVE，
    // 随后 GStreamer 无帧卡住再重启，形成循环。这里先加入组播/绑定端口等待真实 UDP 包。
    if (isRawUdpLiveKindForLiveRetry(scheme, desiredKind)) {
        startUdpTrafficProbeIfPossible(parseUdpLiveProbeTargetForLiveRetry(probeUrl));
        return;
    }

    // SRT 没有引入 libsrt 的情况下做不了可靠轻量握手，仍只作为恢复触发；
    // 如果实际切 LIVE 后没有真实视频帧，LIVE watchdog 会记录状态并重启。
    if (isRawUdpOrSrtLiveKindForLiveRetry(scheme, desiredKind)) {
        LOG_VIDEO("后台 SRT/特殊组播探测采用 carrier-only 策略:" << probeUrl);
        QTimer::singleShot(0, this, [switchIfStillWanted]() {
            switchIfStillWanted(QStringLiteral("SRT/special carrier up"));
        });
        return;
    }

    // HTTP/HTTPS/HLS/HTTP-TS/HTTP-FLV：使用 QNetworkAccessManager 做真正 HTTP 探测。
    // 不能用裸 QTcpSocket 发 HEAD：HTTPS 不支持，很多流媒体服务也不支持 HEAD。
    if (scheme == QStringLiteral("http") || scheme == QStringLiteral("https") ||
        desiredKind == QStringLiteral("hls") || desiredKind == QStringLiteral("http-ts")) {
        if (!u.isValid() || u.host().isEmpty()) {
            LOG_VIDEO("后台 HTTP 探测直播流失败：URL 无效或 host 为空:" << probeUrl);
            return;
        }

        m_liveProbeRunning = true;

        QNetworkAccessManager *manager = new QNetworkAccessManager(this);
        manager->setProperty("yc_probe_done", false);

        QNetworkRequest req(u);
        req.setRawHeader("User-Agent", "qt_ycest-live-probe");
        req.setRawHeader("Accept", "*/*");
        req.setRawHeader("Connection", "close");
        req.setRawHeader("Range", "bytes=0-4095");
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
        req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif

        QNetworkReply *reply = manager->get(req);

        auto finishHttpProbe = [this, manager, reply, probeUrl, switchIfStillWanted](bool ok, const QString &detail) {
            if (!manager || manager->property("yc_probe_done").toBool()) {
                return;
            }

            manager->setProperty("yc_probe_done", true);
            m_liveProbeRunning = false;

            if (reply) {
                reply->abort();
                reply->deleteLater();
            }
            manager->deleteLater();

            if (!ok) {
                LOG_VIDEO("后台 HTTP 探测直播流失败:" << detail << " url=" << probeUrl);
                return;
            }

            switchIfStillWanted(detail);
        };

        connect(reply, &QNetworkReply::finished, this, [reply, finishHttpProbe, probeUrl]() {
            const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString contentType = QString::fromLatin1(reply->rawHeader("Content-Type"));
            const QByteArray body = reply->readAll();

            if (isHttpRedirectStatusForLiveRetry(statusCode)) {
                // FollowRedirectsAttribute 正常会自动跟随；如果仍拿到重定向，说明没有最终媒体响应，不直接切 LIVE。
                finishHttpProbe(false, QStringLiteral("HTTP redirect not resolved status=") + QString::number(statusCode));
                return;
            }

            if (!isHttpSuccessStatusForLiveRetry(statusCode)) {
                finishHttpProbe(false, QStringLiteral("HTTP status=") + QString::number(statusCode));
                return;
            }

            QString detail;
            if (!isLikelyLiveHttpBodyForLiveRetry(probeUrl, contentType, body, &detail)) {
                finishHttpProbe(false, detail);
                return;
            }

            finishHttpProbe(true, detail);
        });

        connect(reply, QOverload<QNetworkReply::NetworkError>::of(&QNetworkReply::error),
                this, [finishHttpProbe](QNetworkReply::NetworkError) {
            finishHttpProbe(false, QStringLiteral("network reply error"));
        });

        QTimer::singleShot(4000, reply, [finishHttpProbe]() {
            finishHttpProbe(false, QStringLiteral("timeout"));
        });

        LOG_VIDEO("后台 HTTP 探测直播流:" << probeUrl);
        return;
    }

    const QString host = u.host();
    if (host.isEmpty()) {
        LOG_VIDEO("后台探测直播流失败：URL host 为空:" << probeUrl);
        return;
    }

    int port = u.port();
    if (port <= 0) {
        if (scheme == QStringLiteral("rtsp")) {
            port = 554;
        } else if (scheme == QStringLiteral("rtmp")) {
            port = 1935;
        } else if (scheme == QStringLiteral("rtmps")) {
            port = 443;
        } else {
            // udp/srt 没有可靠的轻量探测能力。后台重试阶段不直接切 LIVE，
            // 否则会造成“本地轮播 -> LIVE失败 -> 本地轮播”的循环。
            LOG_VIDEO("后台探测直播流失败：当前协议不支持安全后台探测 scheme=" << scheme << " url=" << probeUrl);
            return;
        }
    }

    m_liveProbeRunning = true;

    QTcpSocket *sock = new QTcpSocket(this);
    sock->setProperty("yc_probe_done", false);
    sock->setProperty("yc_probe_buffer", QByteArray());
    sock->setProperty("yc_probe_stage", scheme == QStringLiteral("rtsp")
                      ? QStringLiteral("rtsp_describe")
                      : QStringLiteral("tcp_connect"));

    auto finishProbe = [this, sock, probeUrl, switchIfStillWanted, logProbeAttempt](bool ok, const QString &detail) {
        if (!sock) return;
        if (sock->property("yc_probe_done").toBool()) return;

        sock->setProperty("yc_probe_done", true);
        m_liveProbeRunning = false;

        sock->abort();
        sock->deleteLater();

        if (!ok) {
            if (logProbeAttempt) {
                LOG_VIDEO("后台探测直播流失败:" << detail << " url=" << probeUrl);
                m_liveProbeFailureLogUrl = probeUrl;
                m_liveProbeFailureLogElapsed.start();
            }
            return;
        }

        switchIfStillWanted(detail);
    };

    connect(sock, &QTcpSocket::connected, this, [sock, probeUrl, scheme, finishProbe]() {
        if (scheme == QStringLiteral("rtsp")) {
            // RTSP 不能只看 TCP 连通或 DESCRIBE 200。
            // 这里先 DESCRIBE 拿 SDP，再对 video track 执行 SETUP。
            // 只有 SETUP 200 才认为直播流真正可用，避免无流时反复切前台 LIVE。
            sendRtspDescribeForLiveRetry(sock, probeUrl);
            return;
        }

        if (scheme == QStringLiteral("rtmp")) {
            // RTMP 做最小握手探测，比单纯 TCP connect 更可靠；
            // 但它仍不能百分百确认具体 playpath 有流，只能确认 RTMP 服务可握手。
            QByteArray req;
            req.resize(1537);
            req.fill('\0');
            req[0] = 0x03; // C0 version
            sock->write(req);
            sock->flush();
            return;
        }

        // rtmps 需要 TLS + RTMP 握手，当前不用正式播放器做后台探测，避免误切。
        finishProbe(false, QStringLiteral("unsupported tcp live scheme without safe probe: ") + scheme);
    });

    connect(sock, &QTcpSocket::readyRead, this, [sock, probeUrl, scheme, finishProbe]() {
        QByteArray buf = sock->property("yc_probe_buffer").toByteArray();
        buf += sock->readAll();
        sock->setProperty("yc_probe_buffer", buf);

        if (scheme == QStringLiteral("rtsp")) {
            const RtspProbeResponse resp = parseRtspResponseForLiveRetry(buf);
            if (!resp.complete) {
                return;
            }

            sock->setProperty("yc_probe_buffer", buf.mid(resp.consumedBytes));

            const QString stage = sock->property("yc_probe_stage").toString();
            if (stage == QStringLiteral("rtsp_describe")) {
                if (resp.statusCode != 200) {
                    finishProbe(false, QStringLiteral("RTSP DESCRIBE failed: ") + resp.statusLine);
                    return;
                }

                const QString videoControl = findRtspVideoControlForLiveRetry(resp.body);
                if (videoControl.isEmpty()) {
                    finishProbe(false, QStringLiteral("RTSP DESCRIBE ok but no video track in SDP"));
                    return;
                }

                // RTSP 组播 SDP 通常会在 c=IN IP4 里给出 224.0.0.0/4 地址。
                // 这类流用 TCP unicast SETUP 探测经常失败，导致重启后一直停留本地。
                // 这里 DESCRIBE 已确认有 video track 且 SDP 指向组播地址，就允许切回 LIVE，
                // 后续由真实视频帧 watchdog 判断是否真正恢复。
                if (sdpLooksLikeMulticastForLiveRetry(resp.body)) {
                    finishProbe(true, QStringLiteral("RTSP multicast DESCRIBE ok"));
                    return;
                }

                const QString setupUrl = buildRtspControlUrlForLiveRetry(QUrl(probeUrl), videoControl);
                if (setupUrl.trimmed().isEmpty()) {
                    finishProbe(false, QStringLiteral("RTSP video control url empty"));
                    return;
                }

                sock->setProperty("yc_probe_stage", QStringLiteral("rtsp_setup"));
                sendRtspSetupForLiveRetry(sock, setupUrl);
                return;
            }

            if (stage == QStringLiteral("rtsp_setup")) {
                if (resp.statusCode == 200) {
                    finishProbe(true, QStringLiteral("RTSP DESCRIBE+SETUP ok"));
                    return;
                }

                finishProbe(false, QStringLiteral("RTSP SETUP failed: ") + resp.statusLine);
                return;
            }

            finishProbe(false, QStringLiteral("RTSP invalid probe stage"));
            return;
        }

        if (scheme == QStringLiteral("rtmp")) {
            if (buf.size() >= 1537 && static_cast<unsigned char>(buf.at(0)) == 0x03) {
                finishProbe(true, QStringLiteral("RTMP handshake ok"));
            }
            return;
        }
    });

    connect(sock, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
            this, [finishProbe](QAbstractSocket::SocketError) {
        finishProbe(false, QStringLiteral("socket error"));
    });

    QTimer::singleShot(3500, sock, [finishProbe]() {
        finishProbe(false, QStringLiteral("timeout"));
    });

    if (logProbeAttempt) {
        LOG_VIDEO("后台探测直播流:" << probeUrl);
    }
    sock->connectToHost(host, static_cast<quint16>(port));
}


// 切换到直播流
void MultimediaDemo::switchToLiveStream(const QString& url, const QString& kind)
{
    if (url.trimmed().isEmpty()) return;

    m_localPlaybackEnabled = false;
    invalidatePendingLocalPlaybackStarts();
    stopEmptyVideoListWatch();

    // 切换到直播时，停止 videoRect 图片计时并隐藏图片 QLabel
    stopVideoRectPictureDisplay(true);

    stopLiveWatchdog();
    m_liveWatchdogStartMs = 0;
    m_lastLiveFrameMs = 0;

    m_liveSwitching = true;
    m_isLiveMode = true;
    m_liveRecovering = false;
    m_leaving = false;

    disconnect(mediaPlayer, &GstPlayerWidget::videoFinished,
               this, &MultimediaDemo::playNextVideo);
    if (nextVideoPlayer) {
        disconnect(nextVideoPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);
    }

    isPreloading = false;

    if (videoWidget) {
        // LIVE 切换也重新校正配置区域，但保持现有 Qt 层级，避免视频画布
        // 抢到提示条、弹窗和软件光标之上。
        videoWidget->resize(VideoDisplay_x_Size, VideoDisplay_y_Size);
        videoWidget->move(VideoDisplay_x, VideoDisplay_y);
        videoWidget->show();
        videoWidget->update();
    }

    if (nextVideoPlayer) {
        nextVideoPlayer->stop();
        nextVideoPlayer->setVideoOutput(nullptr);
        nextVideoPlayer->setPreloadMode(true);
    }

    if (mediaPlayer) {
        mediaPlayer->stop();
        mediaPlayer->setPreloadMode(false);
        mediaPlayer->setVideoOutput(videoWidget);
        mediaPlayer->setMedia(url);
        mediaPlayer->requestDisplayRectUpdate();
        mediaPlayer->play();

        startLiveWatchdog();
    }

    m_liveSwitching = false;

    persistPlaybackStateToNetCfg(QStringLiteral("LIVE"), QStringLiteral("LIVE"), QStringLiteral("live_playing"));

    LOG_VIDEO("正式切换到直播流: " << url << " kind=" << kind);

    emit statusMessageRequested(QStringLiteral("正在切换到直播流"), 3000);
}

//void MultimediaDemo::tryStartLiveStream(const QString& url, const QString& kind)
//{
//    if (url.trimmed().isEmpty()) return;

//    m_isLiveMode = true;
//    m_liveRecovering = false;
//    m_leaving = false;

//    disconnect(mediaPlayer, &GstPlayerWidget::videoFinished,
//               this, &MultimediaDemo::playNextVideo);
//    if (nextVideoPlayer) {
//        disconnect(nextVideoPlayer, &GstPlayerWidget::videoFinished,
//                   this, &MultimediaDemo::playNextVideo);
//    }

//    isPreloading = false;

//    if (videoWidget) {
//        videoWidget->resize(VideoDisplay_x_Size, VideoDisplay_y_Size);
//        videoWidget->move(VideoDisplay_x, VideoDisplay_y);
//        videoWidget->show();
//        videoWidget->raise();
//    }

//    if (nextVideoPlayer) {
//        nextVideoPlayer->stop();
//        nextVideoPlayer->setVideoOutput(nullptr);
//        nextVideoPlayer->setPreloadMode(true);
//    }

//    if (mediaPlayer) {
//        mediaPlayer->stop();
//        mediaPlayer->setPreloadMode(false);
//        mediaPlayer->setVideoOutput(videoWidget);

//        mediaPlayer->setMedia(url);
//        mediaPlayer->play();

//        m_lastLiveFrameMs = QDateTime::currentMSecsSinceEpoch();
//        startLiveWatchdog();
//    }

//    LOG_VIDEO("开始尝试直播拉流: " << url << " kind=" << kind);
//}


// 组播/UDP 断流专用：播放器底层不一定抛 TCP/RTSP 错误，
// 必须先强制隐藏旧 LIVE 图层并 reset，避免最后一帧继续压在本地轮播上。
// 这里只处理“当前前台播放器脱离直播”，不清 m_liveDesired，保持原有后台重试逻辑。
void MultimediaDemo::forceStopCurrentLivePlayerForFallback(const QString& reason)
{
    LOG_VIDEO("强制脱离当前LIVE播放器: " << reason);

    if (mediaPlayer) {
        disconnect(mediaPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);

        mediaPlayer->stop();
        mediaPlayer->setVideoOutput(nullptr);
        mediaPlayer->setPreloadMode(false);
    }

    if (nextVideoPlayer) {
        disconnect(nextVideoPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);

        nextVideoPlayer->stop();
        nextVideoPlayer->setVideoOutput(nullptr);
        nextVideoPlayer->setPreloadMode(true);
    }

    if (videoWidget) {
        videoWidget->resize(VideoDisplay_x_Size, VideoDisplay_y_Size);
        videoWidget->move(VideoDisplay_x, VideoDisplay_y);
        videoWidget->show();
        videoWidget->update();
    }
}

/**
 * @brief 在压缩视频仍持续进入、但 MPP 长时间无输出时原地重建直播管线。
 *
 * 该路径不启动本地 4K 轮播，也不再用仅能证明 RTP 可达的轻量探测作为
 * 恢复条件；完整销毁 playbin 后直接重新建立当前 LIVE 会话，以释放异常的
 * MPP 解码上下文和 DMA-BUF 池。
 */
void MultimediaDemo::restartLivePipelineAfterDecoderStall(const QString& reason)
{
    const QString url = m_desiredLiveUrl.trimmed();
    if (!m_liveDesired || url.isEmpty() || !mediaPlayer) {
        m_liveRecovering = false;
        onLiveStreamInterrupted(reason, true);
        return;
    }

    stopLiveWatchdog();
    m_liveRecovering = true;
    m_liveSwitching = true;
    m_isLiveMode = true;
    m_localPlaybackEnabled = false;
    invalidatePendingLocalPlaybackStarts();
    stopEmptyVideoListWatch();
    stopVideoRectPictureDisplay(true);

    LOG_VIDEO("检测到解码器停滞，完整重建当前LIVE管线，不启动本地4K轮播:"
              << reason << url);

    disconnect(mediaPlayer, &GstPlayerWidget::videoFinished,
               this, &MultimediaDemo::playNextVideo);
    if (nextVideoPlayer) {
        disconnect(nextVideoPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);
        nextVideoPlayer->stop();
        nextVideoPlayer->setVideoOutput(nullptr);
        nextVideoPlayer->setPreloadMode(true);
    }
    if (m_liveProbePlayer) {
        m_liveProbePlayer->stop();
    }
    m_liveProbeRunning = false;

    if (!mediaPlayer->recreatePipeline()) {
        m_isLiveMode = false;
        m_liveSwitching = false;
        m_liveRecovering = false;
        persistPlaybackStateToNetCfg(QStringLiteral("LIVE"), QStringLiteral("NONE"),
                                     QStringLiteral("live_decoder_pipeline_recreate_failed"));
        emit statusMessageRequested(QStringLiteral("直播解码器重建失败，等待下一次重试"), 3000);
        startLiveRetry();
        return;
    }

    mediaPlayer->setPreloadMode(false);
    mediaPlayer->setVideoOutput(videoWidget);
    mediaPlayer->setMedia(url);
    mediaPlayer->requestDisplayRectUpdate();
    mediaPlayer->play();

    if (videoWidget) {
        videoWidget->resize(VideoDisplay_x_Size, VideoDisplay_y_Size);
        videoWidget->move(VideoDisplay_x, VideoDisplay_y);
        videoWidget->show();
        videoWidget->update();
    }

    m_liveSwitching = false;
    m_liveRecovering = false;
    startLiveWatchdog();
    persistPlaybackStateToNetCfg(QStringLiteral("LIVE"), QStringLiteral("LIVE"),
                                 QStringLiteral("live_decoder_pipeline_recreated"));
    emit statusMessageRequested(QStringLiteral("直播解码器已重建，正在等待视频帧"), 3000);
}

// RK3566 的 GStreamer 播放器可安全停止。首次播放始终没有真实视频帧，
// 视为地址错误或直播源本身无数据，彻底回到本地轮播；只有已经正常出过帧
// 后发生断流，才保留 LIVE 业务目标并继续后台探测。
void MultimediaDemo::onLiveStreamInterrupted(const QString& reason, bool keepLiveRetry)
{
    stopLiveWatchdog();
    LOG_VIDEO("直播中断，切回本地轮播:"
              << reason << " keepLiveRetry=" << keepLiveRetry);

    m_isLiveMode = false;
    m_liveRecovering = true;
    m_liveSwitching = false;
    m_localPlaybackEnabled = true;

    if (!keepLiveRetry) {
        // 首帧从未到达：直播地址不可用或源端没有媒体数据。
        // 取消LIVE业务目标，避免轻量网络探测成功后反复切入无数据的直播。
        m_liveDesired = false;
        m_desiredLiveUrl.clear();
        m_desiredLiveKind.clear();
        m_liveSourceValidated = false;
        stopLiveRetry();
        if (m_liveProbePlayer) {
            m_liveProbePlayer->stop();
        }
        m_liveProbeRunning = false;
    }

    forceStopCurrentLivePlayerForFallback(reason);

    if (keepLiveRetry) {
        // 已经正常出过帧后断流：保留LIVE目标，网络恢复后自动切回。
        persistPlaybackStateToNetCfg(QStringLiteral("LIVE"), QStringLiteral("RECORDED"),
                                     QStringLiteral("live_network_fault_local_fallback: ") + reason);
        emit statusMessageRequested(
                    QStringLiteral("直播网络中断，已切回本地轮播，后台继续探测直播源"), 3000);
    } else {
        persistPlaybackStateToNetCfg(QStringLiteral("RECORDED"), QStringLiteral("RECORDED"),
                                     QStringLiteral("live_source_unavailable_local_fallback: ") + reason);
        emit statusMessageRequested(
                    QStringLiteral("直播地址无效或无数据，已切回本地轮播"), 3000);
    }

    resumeLocalPlaylistFromStart();
    m_liveRecovering = false;
    if (keepLiveRetry) {
        startLiveRetry();
    }
}

/** @brief 从列表首项恢复本地轮播。 */
void MultimediaDemo::resumeLocalPlaylistFromStart()
{
    if (m_downloadPauseActive) {
        LOG_VIDEO("下载进行中，忽略恢复本地轮播请求");
        return;
    }
    stopLiveWatchdog();

    LOG_VIDEO("开始恢复本地轮播");

    m_isLiveMode = false;
    m_leaving = false;

    // 重新扫描本地目录
    rebuildVideoRectMediaList();
    pruneMissingVideoRectMedia();

    LOG_VIDEO("恢复轮播时当前列表数量:" << videoRectMediaList.size());

    if (videoRectMediaList.isEmpty()) {
        LOG_VIDEO("恢复混播失败：videoRect 混播列表为空，启动空列表监测");

        currentVideoIndex = -1;
        currentVideoRectMediaIndex = -1;

        startEmptyVideoListWatch();

        if (videoWidget) {
            videoWidget->hide();
        }

        stopVideoRectPictureDisplay(true);
        return;
    }

    // 非空时,停掉空列表监测
    stopEmptyVideoListWatch();

    if (nextVideoPlayer) {
        disconnect(nextVideoPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);
        nextVideoPlayer->stop();
        nextVideoPlayer->setVideoOutput(nullptr);
        nextVideoPlayer->setPreloadMode(true);
    }

    if (mediaPlayer) {
        disconnect(mediaPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);
        mediaPlayer->stop();
        mediaPlayer->setPreloadMode(false);
    }

    currentVideoRectMediaIndex = 0;

    const quint64 startSerial = issueLocalPlaybackStartSerial();

    QTimer::singleShot(0, this, [this, startSerial]() {
        if (startSerial != m_localPlaybackStartSerial) return;
        if (m_downloadPauseActive) return;
        if (!m_localPlaybackEnabled) return;
        if (!isVisible()) return;
        if (videoRectMediaList.isEmpty()) return;

        playVideoRectMediaAt(currentVideoRectMediaIndex);

        LOG_VIDEO("恢复 videoRect 本地混播");
    });
}


/** @brief 本地列表为空时启动短周期目录观察。 */
void MultimediaDemo::startEmptyVideoListWatch()
{

    if (!m_localPlaybackEnabled) {
        LOG_VIDEO("当前不允许本地播放，忽略空列表监测启动请求");
        return;
    }

    if (!m_emptyVideoListTimer) return;

    if (!m_emptyVideoListTimer->isActive()) {
        m_emptyVideoListTimer->start();
        LOG_VIDEO("本地视频列表为空，已启动空列表监测");
    }
}

/** @brief 有可播文件或页面退出时停止空列表观察。 */
void MultimediaDemo::stopEmptyVideoListWatch()
{
    if (!m_emptyVideoListTimer) return;

    if (m_emptyVideoListTimer->isActive()) {
        m_emptyVideoListTimer->stop();
        LOG_VIDEO("已停止空列表监测");
    }
}

/** @brief 重新扫描目录并在发现文件后启动首项。 */
void MultimediaDemo::onEmptyVideoListWatchTimeout()
{
    if (m_downloadPauseActive) {
        return;
    }
    if (!m_localPlaybackEnabled) {
        stopEmptyVideoListWatch();
        return;
    }

    // 当前是LIVE或LIVE目标时，不允许本地补播
    if (m_isLiveMode || m_liveDesired) {
        stopEmptyVideoListWatch();
        return;
    }

    // 页面不可见，不允许自动播
    if (!isVisible()) {
        stopEmptyVideoListWatch();
        return;
    }


    rebuildVideoRectMediaList();
    pruneMissingVideoRectMedia();

    if (videoRectMediaList.isEmpty()) {
        return;
    }

    LOG_VIDEO("空列表监测发现新资源文件，立即开始本地播放");
    stopEmptyVideoListWatch();
    startFirstLocalVideoFromList();
}


// 开始播放本地首个视频
void MultimediaDemo::playDownloadedVideoNow(const QString& filePath)
{
    const QString absPath = QFileInfo(filePath).absoluteFilePath();
    if (absPath.isEmpty()) {
        LOG_VIDEO("playDownloadedVideoNow失败：路径为空");
        return;
    }

    rebuildVideoRectMediaList();
    pruneMissingVideoRectMedia();

    int targetVideoIndex = videoFileList.indexOf(absPath);
    if (targetVideoIndex < 0) {
        videoFileList.append(absPath);
        targetVideoIndex = videoFileList.size() - 1;
    }

    int targetMediaIndex = -1;
    for (int i = 0; i < videoRectMediaList.size(); ++i) {
        if (videoRectMediaList.at(i).type == VideoRectMedia_Video &&
            videoRectMediaList.at(i).filePath == absPath) {
            targetMediaIndex = i;
            break;
        }
    }

    if (targetMediaIndex < 0) {
        videoRectMediaList << VideoRectMediaItem(VideoRectMedia_Video, absPath);
        targetMediaIndex = videoRectMediaList.size() - 1;
    }

    m_isLiveMode = false;
    m_leaving = false;
    stopEmptyVideoListWatch();

    currentVideoIndex = targetVideoIndex;
    currentVideoRectMediaIndex = targetMediaIndex;

    if (nextVideoPlayer) {
        disconnect(nextVideoPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);
        nextVideoPlayer->stop();
        nextVideoPlayer->setVideoOutput(nullptr);
        nextVideoPlayer->setPreloadMode(true);
    }

    if (mediaPlayer) {
        disconnect(mediaPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);
        mediaPlayer->stop();
        mediaPlayer->setPreloadMode(false);
    }

    QTimer::singleShot(0, this, [this, targetMediaIndex]() {
        if (m_downloadPauseActive) return;
        if (!m_localPlaybackEnabled) return;
        if (targetMediaIndex < 0 || targetMediaIndex >= videoRectMediaList.size()) return;

        playVideoRectMediaAt(targetMediaIndex);

        LOG_VIDEO("下载完成后立即开始播放 videoRect 混播项");
    });
}




/** @brief 从当前列表首个可用文件启动本地轮播。 */
void MultimediaDemo::startFirstLocalVideoFromList()
{
    if (m_downloadPauseActive) {
        LOG_VIDEO("下载进行中，忽略本地首视频开播");
        return;
    }
    if (!m_localPlaybackEnabled) {
        LOG_VIDEO("当前不允许本地播放，忽略自动开播");
        return;
    }

    if (!isVisible()) {
        LOG_VIDEO("页面不可见，忽略自动开播");
        return;
    }


    if (videoRectMediaList.isEmpty()) {
        rebuildVideoRectMediaList();
        pruneMissingVideoRectMedia();
    }


    if (videoRectMediaList.isEmpty()) {
        LOG_VIDEO("startFirstLocalVideoFromList失败：videoRect 混播列表为空");
        startEmptyVideoListWatch();
        return;
    }

    stopEmptyVideoListWatch();

    if (nextVideoPlayer) {
        disconnect(nextVideoPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);
        nextVideoPlayer->stop();
        nextVideoPlayer->setVideoOutput(nullptr);
        nextVideoPlayer->setPreloadMode(true);
    }

    if (mediaPlayer) {
        disconnect(mediaPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);
        mediaPlayer->stop();
        mediaPlayer->setPreloadMode(false);
    }

    currentVideoRectMediaIndex = 0;

    const quint64 startSerial = issueLocalPlaybackStartSerial();

    QTimer::singleShot(0, this, [this, startSerial]() {
        if (startSerial != m_localPlaybackStartSerial) return;
        if (m_downloadPauseActive) return;
        if (!m_localPlaybackEnabled) return;
        if (!isVisible()) return;
        if (videoRectMediaList.isEmpty()) return;

        playVideoRectMediaAt(currentVideoRectMediaIndex);

        LOG_VIDEO("开始播放 videoRect 本地混播首项");
    });
}

/** @brief 启动直播首帧/持续无帧 watchdog。 */
void MultimediaDemo::startLiveWatchdog()
{
    if (m_liveWatchdogTimer && !m_liveWatchdogTimer->isActive()) {
        m_liveWatchdogStartMs = QDateTime::currentMSecsSinceEpoch();
        m_lastLiveFrameMs = 0;
        m_liveWatchdogTimer->start();
        LOG_VIDEO("直播watchdog已启动，等待真实视频帧");
    }
}

/** @brief 停止直播 watchdog 并清空计时状态。 */
void MultimediaDemo::stopLiveWatchdog()
{
    if (m_liveWatchdogTimer && m_liveWatchdogTimer->isActive()) {
        m_liveWatchdogTimer->stop();
        LOG_VIDEO("直播watchdog已停止");
    }
}


/** @brief 判定首帧或持续无帧超时并触发回退。 */
void MultimediaDemo::onLiveWatchdogTimeout()
{
    if (!m_isLiveMode) return;
    if (m_liveRecovering) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 decoderInputMs = mediaPlayer
            ? mediaPlayer->lastDecoderInputMSecs() : 0;
    const qint64 decoderInputIdleMs = decoderInputMs > 0
            ? now - decoderInputMs : -1;
    const bool decoderInputActive = decoderInputIdleMs >= 0 &&
            decoderInputIdleMs <= m_liveDecoderInputActiveWindowMs;

    // 关键：优先读取 GStreamer pad probe 记录的真实解码视频帧时间。
    // 对组播/UDP 来说，RTSP/TCP 控制连接状态不能代表媒体数据是否还在到达。
    const qint64 playerVideoMs = mediaPlayer ? mediaPlayer->lastVideoFrameMSecs() : 0;
    if (playerVideoMs > m_lastLiveFrameMs) {
        m_lastLiveFrameMs = playerVideoMs;
    }

    const bool hasRealFrame = (m_lastLiveFrameMs > 0 &&
                               m_liveWatchdogStartMs > 0 &&
                               m_lastLiveFrameMs >= m_liveWatchdogStartMs);

    if (!hasRealFrame) {
        const qint64 waitFirstFrameMs = now - m_liveWatchdogStartMs;
        if (waitFirstFrameMs < m_liveStartupGraceMs) {
            LOG_VIDEO("直播watchdog等待首个真实视频帧(ms): " << waitFirstFrameMs);
            return;
        }

        m_liveRecovering = true;
        if (decoderInputActive) {
            LOG_VIDEO("直播首帧超时，但解码器输入仍活跃，按解码器停滞重建: waitMs="
                      << waitFirstFrameMs << " decoderInputIdleMs=" << decoderInputIdleMs);
            restartLivePipelineAfterDecoderStall(
                        QStringLiteral("live decoder first-frame stall"));
            return;
        }
        LOG_VIDEO("直播首帧超时，未收到真实解码视频帧(ms): " << waitFirstFrameMs);
        onLiveStreamInterrupted(QStringLiteral("live watchdog first video frame timeout"),
                                m_liveSourceValidated);
        return;
    }

    const qint64 idleMs = now - m_lastLiveFrameMs;

    if (idleMs >= m_liveTimeoutMs) {
        m_liveRecovering = true;
        if (decoderInputActive) {
            LOG_VIDEO("直播无解码输出但输入仍活跃，按解码器停滞重建: outputIdleMs="
                      << idleMs << " decoderInputIdleMs=" << decoderInputIdleMs);
            restartLivePipelineAfterDecoderStall(
                        QStringLiteral("live decoder output stall"));
            return;
        }
        LOG_VIDEO("直播超时，无真实解码视频帧时长(ms): " << idleMs
                  << " lastVideoFrameMs=" << m_lastLiveFrameMs
                  << " decoderInputIdleMs=" << decoderInputIdleMs);

        onLiveStreamInterrupted(QStringLiteral("live watchdog decoded video frame timeout"), true);
    }
}


/** @brief 在仍期望直播时启动定时重试。 */
void MultimediaDemo::startLiveRetry()
{
    if (m_liveRetryTimer && !m_liveRetryTimer->isActive()) {
        m_liveRetryTimer->start();
        LOG_VIDEO("直播重试定时器已启动");
    }
}

/** @brief 停止直播重试并清除重入标记。 */
void MultimediaDemo::stopLiveRetry()
{
    if (m_liveRetryTimer && m_liveRetryTimer->isActive()) {
        m_liveRetryTimer->stop();
        LOG_VIDEO("直播重试定时器已停止");
    }
}

/** @brief 重试定时到达时重新探测期望直播源。 */
void MultimediaDemo::onLiveRetryTimeout()
{
    // 只有“业务目标仍是LIVE”时才重试
    if (!m_liveDesired) return;
    if (m_desiredLiveUrl.trimmed().isEmpty()) return;

    // 当前正在直播，就不用重试
    if (m_isLiveMode) return;

    // 已经有探测任务在跑，不重复发起
    if (m_liveProbeRunning) return;

    if (!isWiredLinkUpForLiveRetry()) {
        LOG_VIDEO("直播重试跳过：网口 carrier=0，继续本地轮播等待网线恢复");
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_localPlayerBusyUntilMs > 0 && now < m_localPlayerBusyUntilMs) {
        LOG_VIDEO("直播重试延后：本地视频正在切文件，避免后台探测抢占 GStreamer 资源");
        return;
    }

    const bool logProbeAttempt = !m_liveProbeFailureLogElapsed.isValid() ||
            m_liveProbeFailureLogUrl != m_desiredLiveUrl.trimmed() ||
            m_liveProbeFailureLogElapsed.elapsed() >= kLiveProbeFailureLogIntervalMs;
    if (logProbeAttempt) {
        LOG_VIDEO("直播重试触发，开始轻量探测: " << m_desiredLiveUrl);
    }

    probeLiveStream(m_desiredLiveUrl);
}


/** @brief 清除“期望直播”状态并停止后台探测和重试。 */
void MultimediaDemo::stopDesiredLive()
{
    m_liveDesired = false;
    m_desiredLiveUrl.clear();
    m_desiredLiveKind.clear();
    m_liveSourceValidated = false;
    m_liveProbeFailureLogElapsed.invalidate();
    m_liveProbeFailureLogUrl.clear();
    //m_isLiveMode = false;

    if (m_liveProbePlayer) {
        m_liveProbePlayer->stop();
    }
    m_liveProbeRunning = false;


    stopLiveRetry();
    stopLiveWatchdog();
    stopEmptyVideoListWatch();
    LOG_VIDEO("已取消LIVE目标，不再重试");
}


/** @brief 仅停止主播放器中的直播，不清除业务期望状态。 */
void MultimediaDemo::stopCurrentLivePlaybackOnly()
{
    LOG_VIDEO("仅停止当前前台LIVE播放");

    stopLiveWatchdog();

    // 这里只停止“前台直播状态”，不取消LIVE目标
    m_isLiveMode = false;
    m_liveRecovering = false;
    m_liveSwitching = false;
    m_leaving = false;

    // 停止当前正式播放器
    if (mediaPlayer) {
        disconnect(mediaPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);

        mediaPlayer->stop();
        mediaPlayer->setVideoOutput(nullptr);
        mediaPlayer->setVideoOutput(videoWidget);
        mediaPlayer->setPreloadMode(false);
    }

    // 停止预加载播放器
    if (nextVideoPlayer) {
        disconnect(nextVideoPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);

        nextVideoPlayer->stop();
        nextVideoPlayer->setVideoOutput(nullptr);
        nextVideoPlayer->setPreloadMode(true);
    }
}

/** @brief 切换到录播模式；已在本地轮播时仅同步业务状态。 */
void MultimediaDemo::switchToRecordedMode()
{
    // 重复收到RECORDED时只更新业务状态，不重启正在运行的本地轮播。
    const bool localImageRunning = m_currentVideoRectIsImage &&
                                   videoRectPictureTimer &&
                                   videoRectPictureTimer->isActive();
    const bool localVideoRunning = !m_currentVideoRectIsImage &&
                                   mediaPlayer &&
                                   mediaPlayer->hasActiveMedia();
    const bool alreadyInLocalPlaylist = !m_isLiveMode &&
                                        !m_leaving &&
                                        m_localPlaybackEnabled &&
                                        !m_downloadPauseActive &&
                                        !m_currentVideoRectMediaPath.isEmpty() &&
                                        (localImageRunning || localVideoRunning);

    LOG_VIDEO("收到RECORDED：停止LIVE目标，切回本地轮播");

    emit statusMessageRequested(alreadyInLocalPlaylist
                                    ? QStringLiteral("当前已处于本地轮播模式")
                                    : QStringLiteral("正在切换到本地轮播模式"),
                                3000);

    m_liveSourceValidated = false;
    persistPlaybackStateToNetCfg(QStringLiteral("RECORDED"), QStringLiteral("RECORDED"), QStringLiteral("recorded_received"));

    // RECORDED 说明当前业务目标已经不是 LIVE
    m_liveDesired = false;

    // 保留 url 缓存，便于下次收到LIVE时复用/排查
    // m_desiredLiveUrl 可不清
    // m_desiredLiveKind 可不清

    stopLiveRetry();
    stopLiveWatchdog();

    if (m_liveProbePlayer) {
        m_liveProbePlayer->stop();
    }
    m_liveProbeRunning = false;

    if (alreadyInLocalPlaylist) {
        LOG_VIDEO("当前已处于本地轮播，仅更新RECORDED状态，跳过播放器重启");
        return;
    }

    stopCurrentLivePlaybackOnly();
    resumeLocalPlaylistFromStart();
}


/** @brief 停止直播、本地轮播及预加载播放器。 */
void MultimediaDemo::stopAllPlayback()
{
    const bool localImageRunning = m_currentVideoRectIsImage &&
                                   videoRectPictureTimer &&
                                   videoRectPictureTimer->isActive();
    const bool playerActive = (mediaPlayer && mediaPlayer->hasActiveMedia()) ||
                              (nextVideoPlayer && nextVideoPlayer->hasActiveMedia());
    const bool alreadyStopped = !m_isLiveMode &&
                                !m_liveDesired &&
                                !localImageRunning &&
                                !playerActive &&
                                m_currentVideoRectMediaPath.isEmpty();

    LOG_VIDEO("收到NONE：停止所有播放");

    emit statusMessageRequested(alreadyStopped
                                    ? QStringLiteral("当前已处于停止播放状态")
                                    : QStringLiteral("正在停止所有播放"),
                                3000);

    m_liveSourceValidated = false;
    persistPlaybackStateToNetCfg(QStringLiteral("NONE"), QStringLiteral("NONE"), QStringLiteral("none_received"));

    if (alreadyStopped) {
        LOG_VIDEO("当前已处于停止播放状态，仅更新NONE状态");
        return;
    }

    // 1) 停止直播目标与重试
    m_liveDesired = false;
    m_desiredLiveUrl.clear();
    m_desiredLiveKind.clear();

    stopLiveRetry();
    stopLiveWatchdog();
    stopEmptyVideoListWatch();

    if (m_liveProbePlayer) {
        m_liveProbePlayer->stop();
    }
    m_liveProbeRunning = false;

    // 2) 重置状态
    m_isLiveMode = false;
    m_liveRecovering = false;
    m_liveSwitching = false;
    m_leaving = false;

    // 这里只表示“当前停止播放”，不是永久禁止本地播放
    // 不要写 m_localPlaybackEnabled = false;
    // 否则切回页面后可能无法恢复

    // 3) 停正式播放器
    if (mediaPlayer) {
        disconnect(mediaPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);
        mediaPlayer->stop();
        mediaPlayer->setVideoOutput(nullptr);
        mediaPlayer->setVideoOutput(videoWidget);
        mediaPlayer->setPreloadMode(false);
    }

    // 4) 停预加载播放器
    if (nextVideoPlayer) {
        disconnect(nextVideoPlayer, &GstPlayerWidget::videoFinished,
                   this, &MultimediaDemo::playNextVideo);
        nextVideoPlayer->stop();
        nextVideoPlayer->setVideoOutput(nullptr);
        nextVideoPlayer->setPreloadMode(true);
    }

    isPreloading = false;
    currentVideoIndex = -1;
    m_currentVideoRectMediaPath.clear();

    // 5) 隐藏视频区域
    if (videoWidget) {
        videoWidget->hide();
    }

    // 图片停止播放
    stopVideoRectPictureDisplay(true);
    currentVideoRectMediaIndex = -1;

    // 6) 清理当前播放状态
    emit currentRecordedFileChanged(QString());

    LOG_VIDEO("当前直播和本地播放均已停止");
}


/** @brief 应用 MQTT 下发音量并回报执行结果。 */
void MultimediaDemo::onMqttVolumeChanged(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    if (m_ampCtrl) {
        m_ampCtrl->setVolume(volume);
    }

    if (m_volPanel) {
        m_volPanel->setCurrentVolume(volume);
    }

    emit currentPlayVolumeChanged(volume);
}


/** @brief 合成 UI 与当前视频帧后异步编码为 Base64。 */
void MultimediaDemo::captureCompositeSnapshot()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // 下载态：播放器保持暂停，直接抓 Qt 当前界面
    if (m_downloadPauseActive) {
        if (m_snapshotTimeoutTimer) {
            m_snapshotTimeoutTimer->stop();
        }
        m_snapshotPending = false;

        if (m_snapshotWorkerBusy) {
            qDebug() << "[截图] 下载态后台处理仍在进行，返回缓存";
            if (!m_lastSnapshotBase64.isEmpty()) {
                emit currentPlayImageChanged(m_lastSnapshotBase64);
            }
            return;
        }

        const QPixmap uiShot = this->grab();
        if (uiShot.isNull()) {
            qWarning() << "[截图] 下载态 this->grab() failed";
            if (!m_lastSnapshotBase64.isEmpty()) {
                emit currentPlayImageChanged(m_lastSnapshotBase64);
            }
            return;
        }

        const QImage uiImage = uiShot.toImage();
        if (uiImage.isNull()) {
            qWarning() << "[截图] 下载态 uiShot.toImage() failed";
            if (!m_lastSnapshotBase64.isEmpty()) {
                emit currentPlayImageChanged(m_lastSnapshotBase64);
            }
            return;
        }

        m_snapshotWorkerBusy = true;
        m_snapshotRequestMs = now;
        emit processUiSnapshotAsync(uiImage);
        qDebug() << "[截图] 下载态：已请求抓取 Qt 当前界面";
        return;
    }


    // 1) 2秒内直接返回缓存，避免频繁重复抓图
    if (now - m_lastSnapshotMs < 2000 && !m_lastSnapshotBase64.isEmpty()) {
        emit currentPlayImageChanged(m_lastSnapshotBase64);
        return;
    }


    // 当前 videoRect 播放的是图片时，图片属于 Qt QLabel，
    // 直接抓 Qt 当前界面即可，不需要走 GStreamer 视频帧截图。
    if (m_currentVideoRectIsImage) {
        if (m_snapshotWorkerBusy) {
            qDebug() << "[截图] 当前为图片播放，后台处理仍在进行，返回缓存";
            if (!m_lastSnapshotBase64.isEmpty()) {
                emit currentPlayImageChanged(m_lastSnapshotBase64);
            }
            return;
        }

        const QPixmap uiShot = this->grab();
        if (uiShot.isNull()) {
            qWarning() << "[截图] 当前为图片播放，this->grab() failed";

            /*
            // 备用逻辑：
            // 如果实际验证发现 this->grab() 截不到完整图片，
            // 可以启用下面逻辑，直接把当前播放图片缩放到 videoRect 尺寸后上传。

            if (!m_currentVideoRectImagePath.isEmpty()) {
                QPixmap fallbackPixmap = loadPixmapWithIgnoreWarnings(m_currentVideoRectImagePath);
                if (!fallbackPixmap.isNull()) {
                    QImage fallbackImage = fallbackPixmap
                            .scaled(VideoDisplay_x_Size,
                                    VideoDisplay_y_Size,
                                    Qt::KeepAspectRatio,
                                    Qt::SmoothTransformation)
                            .toImage();

                    if (!fallbackImage.isNull()) {
                        m_snapshotWorkerBusy = true;
                        m_snapshotRequestMs = now;
                        emit processUiSnapshotAsync(fallbackImage);
                        return;
                    }
                }
            }
            */

            if (!m_lastSnapshotBase64.isEmpty()) {
                emit currentPlayImageChanged(m_lastSnapshotBase64);
            }
            return;
        }

        const QImage uiImage = uiShot.toImage();
        if (uiImage.isNull()) {
            qWarning() << "[截图] 当前为图片播放，uiShot.toImage() failed";
            if (!m_lastSnapshotBase64.isEmpty()) {
                emit currentPlayImageChanged(m_lastSnapshotBase64);
            }
            return;
        }

        m_snapshotWorkerBusy = true;
        m_snapshotRequestMs = now;
        emit processUiSnapshotAsync(uiImage);

        qDebug() << "[截图] 当前为图片播放：已抓取 Qt 当前界面";
        return;
    }

    // 2) 上一次播放器抓帧还没回来
    if (m_snapshotPending) {
        if (now - m_snapshotRequestMs > 3000) {
            qWarning() << "[截图] pending 超时，强制重置";
            m_snapshotPending = false;
        } else {
            qDebug() << "[截图] 上一次截图尚未完成，返回缓存图";
            if (!m_lastSnapshotBase64.isEmpty()) {
                emit currentPlayImageChanged(m_lastSnapshotBase64);
            }
            return;
        }
    }

    // 3) 后台线程还在处理，直接返回缓存
    if (m_snapshotWorkerBusy) {
        qDebug() << "[截图] 后台处理仍在进行，返回缓存";
        if (!m_lastSnapshotBase64.isEmpty()) {
            emit currentPlayImageChanged(m_lastSnapshotBase64);
        }
        return;
    }

    if (!mediaPlayer || !mediaPlayer->isAvailable()) {
        qWarning() << "[截图] mediaPlayer not available";
        return;
    }

    m_snapshotPending = true;
    if (m_snapshotTimeoutTimer) {
        m_snapshotTimeoutTimer->start();
    }

    m_snapshotRequestMs = now;

    mediaPlayer->requestSnapshot();
    qDebug() << "[截图] 已请求抓取下一帧";
}


/** @brief 收到播放器视频帧后与 UI 图像一起提交后台编码。 */
void MultimediaDemo::onVideoSnapshotReady(const QImage& videoImage)
{
    if (m_snapshotTimeoutTimer) {
        m_snapshotTimeoutTimer->stop();
    }

    m_snapshotPending = false;

    if (m_downloadPauseActive) {
        qDebug() << "[截图] 下载态忽略播放器视频帧回调";
        return;
    }

    if (videoImage.isNull()) {
        qWarning() << "[截图] videoImage is null";
        return;
    }

    if (m_snapshotWorkerBusy) {
        qWarning() << "[截图] 后台线程忙，丢弃本次截图";
        return;
    }

    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen) {
        qWarning() << "[截图] primaryScreen is null";
        return;
    }

    QPixmap uiShot = screen->grabWindow(0);
    if (uiShot.isNull()) {
        qWarning() << "[截图] grabWindow(0) failed";
        return;
    }

    QImage finalImage = uiShot.toImage();
    if (finalImage.isNull()) {
        qWarning() << "[截图] uiShot.toImage() failed";
        return;
    }

    QPoint videoTopLeft = this->mapToGlobal(QPoint(VideoDisplay_x, VideoDisplay_y));
    QRect videoRect(videoTopLeft.x(),
                    videoTopLeft.y(),
                    VideoDisplay_x_Size,
                    VideoDisplay_y_Size);

    m_snapshotWorkerBusy = true;

    // 把整屏图、视频帧、视频区域交给后台线程处理
    emit processSnapshotAsync(finalImage, videoImage, videoRect);

}


/** @brief 结束失败的截图请求并上报原因。 */
void MultimediaDemo::onVideoSnapshotFailed(const QString& reason)
{
    if (m_snapshotTimeoutTimer) {
        m_snapshotTimeoutTimer->stop();
    }
    m_snapshotPending = false;
    qWarning() << "[截图] 视频帧抓取失败:" << reason;
}

/** @brief 向 MQTT 管理器上报当前录播文件或清除状态。 */
void MultimediaDemo::notifyCurrentRecordedFile()
{
    if (currentVideoRectMediaIndex >= 0 &&
        currentVideoRectMediaIndex < videoRectMediaList.size()) {

        const VideoRectMediaItem item = videoRectMediaList.at(currentVideoRectMediaIndex);

        if (item.type == VideoRectMedia_Video) {
            emit currentRecordedFileChanged(item.filePath);
        } else {
            emit currentRecordedFileChanged(QString());
        }
    }
}

/** @brief 保存后台生成的 Base64 快照并通知 MQTT 状态。 */
void MultimediaDemo::onSnapshotProcessFinished(const QString &base64)
{
    m_snapshotWorkerBusy = false;
    m_lastSnapshotMs = QDateTime::currentMSecsSinceEpoch();
    m_lastSnapshotBase64 = base64;

    qDebug() << "[截图] Base64长度 =" << base64.length();

    emit currentPlayImageChanged(base64);
}

/** @brief 释放截图忙状态并记录后台处理失败。 */
void MultimediaDemo::onSnapshotProcessFailed(const QString &reason)
{
    m_snapshotWorkerBusy = false;
    qWarning() << "[截图] 后台处理失败:" << reason;
}



/** @brief 从配置读取并刷新网络/MQTT 信息面板字段。 */
void MultimediaDemo::readNetConfiguration()
{
    const QString cfgPath = netCfgPath();

    m_netIp.clear();
    m_mqttClientId.clear();
    m_mqttHost.clear();
    m_mqttSubTopics.clear();
    m_mqttRouteName.clear();
    m_streamUrl.clear();

    if (!QFile::exists(cfgPath )) {
        LOG_CONFIG("net_cfg.ini 不存在: " << cfgPath);
        return;
    }

    QSettings netSettings(cfgPath, QSettings::IniFormat);
    netSettings.setIniCodec("UTF-8");

    netSettings.beginGroup("network");
    m_netIp = netSettings.value("ip", "").toString().trimmed();
    netSettings.endGroup();

    m_mqttClientId = netSettings.value("mqtt/client_id", "").toString().trimmed();

    const ActiveMqttRouteInfo routeInfo = resolveActiveMqttRoute(netSettings);
    m_mqttRouteName = routeInfo.routeName;
    m_mqttHost = routeInfo.host;
    m_mqttSubTopics = routeInfo.subTopics;

    netSettings.beginGroup("stream");
    m_streamUrl = netSettings.value("url", "").toString().trimmed();
    netSettings.endGroup();

    // 如果network.ip为空，可以显示 DHCP
//    if (m_netIp.isEmpty()) {
//        netSettings.beginGroup("network");
//        const bool dhcp = netSettings.value("dhcp", false).toBool();
//        netSettings.endGroup();

//        if (dhcp) {
//            m_netIp = "DHCP";
//        }
//    }

    LOG_CONFIG("读取 net_cfg.ini 成功:");
    LOG_CONFIG("  ip=" << m_netIp);
    LOG_CONFIG("  mqtt.client_id=" << m_mqttClientId);
    LOG_CONFIG("  mqtt.host=" << m_mqttHost);
    LOG_CONFIG("  mqtt.route=" << routeInfo.routeName);
    LOG_CONFIG("  mqtt.sub_topics=" << m_mqttSubTopics);
    LOG_CONFIG("  stream.url=" << m_streamUrl);

    if (!routeInfo.ok) {
        LOG_CONFIG("  mqtt.route 警告：未匹配到有效的 host/topic，当前 route=" << routeInfo.routeName);
        if (routeInfo.routeName == QStringLiteral("invalid")) {
            LOG_CONFIG("  mqtt.route 警告：online_v1 与 online_v2 同时启用，已停止使用旧 host/topic 回退。");
        }
    }
}


/** @brief 根据 MQTT 连接标志刷新网络信息显示。 */
void MultimediaDemo::onMqttConnMarkChanged(int mark)
{
    if (!m_netInfoPanel) return;
    m_netInfoPanel->setMqttConnMark(static_cast<MqttConnMark>(mark));
}


/** @brief 隐藏音量、网络等临时信息面板。 */
void MultimediaDemo::hideInfoPanels()
{
    bool volShown = (m_volPanel && m_volPanel->isShown());
    bool netShown = (m_netInfoPanel && m_netInfoPanel->isShown());


    if (volShown) {
        m_volPanel->hideSlideOut();
    }

    if (netShown) {
        m_netInfoPanel->hideSlideOut();
    }

    if (!m_cursorHidden) {
        QApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
        m_cursorHidden = true;
    }

    if (m_panelAutoHideTimer) {
        m_panelAutoHideTimer->stop();
    }
}

/** @brief 根据弹窗和用户活动刷新空闲计时状态。 */
void MultimediaDemo::refreshUiIdleStateByActivity()
{
    const bool volShown = (m_volPanel && m_volPanel->isShown());
    const bool netShown = (m_netInfoPanel && m_netInfoPanel->isShown());
    const bool needKeepAlive = volShown || netShown || !m_cursorHidden;

    if (!needKeepAlive) {
        if (m_panelAutoHideTimer) {
            m_panelAutoHideTimer->stop();
        }
        return;
    }
    updateIdleTimerState();    // 重新开始8秒计时
}


/** @brief 按页面可见性和模态状态启停光标隐藏定时。 */
void MultimediaDemo::updateIdleTimerState()
{
    const bool volShown = (m_volPanel && m_volPanel->isShown());
    const bool netShown = (m_netInfoPanel && m_netInfoPanel->isShown());
    const bool cursorVisible = !m_cursorHidden;

    if (volShown || netShown || cursorVisible) {
        if (m_panelAutoHideTimer) {
            m_panelAutoHideTimer->start(8000);
        }
    } else {
        if (m_panelAutoHideTimer) {
            m_panelAutoHideTimer->stop();
        }
    }
}


/** @brief 若软件光标已隐藏则恢复显示。 */
void MultimediaDemo::showCursorIfHidden()
{
    if (m_cursorHidden) {
        while (QApplication::overrideCursor()) {
            QApplication::restoreOverrideCursor();
        }
        m_cursorHidden = false;
    }

    updateIdleTimerState();
}
