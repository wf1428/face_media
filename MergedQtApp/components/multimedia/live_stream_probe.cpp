/**
 * @file live_stream_probe.cpp
 * @brief 实现网络线程内的直播源探测、超时取消和按用途判定。
 * @author Du lin
 * @date 2026-11-13
 * @note 最近修改：2026-11-14；修改人：Du lin；移除逐轮开始及取消日志，同一调用方和直播地址连续失败只记录首次，恢复时记录一次成功。
 */

#include "live_stream_probe.h"
#include "common/monotonic_clock.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegExp>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>
#include <utility>

namespace {
/**
 * @brief 在网络线程读取 Linux 网卡 carrier。
 * @param[in] ifaceName 网卡接口名。
 * @param[out] valid 节点可读且有内容时为 true；可空。
 * @return true 表示链路接通，节点不可读时返回 false。
 */
static bool readNetworkCarrier(const QString &ifaceName, bool *valid)
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

/**
 * @return 链路接通或系统无可读 carrier 节点时返回 true。
 */
static bool isWiredLinkUp()
{
    // 优先检查明确的有线网口；如果这些 carrier 文件不存在，再退回扫描所有非 lo 网口。
    const QStringList preferredIfaces = { QStringLiteral("eth0"), QStringLiteral("end0") };
    for (const QString &iface : preferredIfaces) {
        bool valid = false;
        const bool up = readNetworkCarrier(iface, &valid);
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
        const bool up = readNetworkCarrier(iface, &valid);
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

/**
 * @brief 判断地址和业务类型是否使用裸 UDP 探测。
 * @param[in] scheme 地址协议。
 * @param[in] kind 业务协议。
 * @return UDP 或业务类型声明组播时为 true。
 */
static bool isRawUdpLiveKind(const QString &scheme, const QString &kind)
{
    const QString s = scheme.trimmed().toLower();
    const QString k = kind.trimmed().toLower();
    return s == QStringLiteral("udp") ||
           k == QStringLiteral("udp") ||
           k == QStringLiteral("multicast") ||
           k.contains(QStringLiteral("multicast"));
}

/**
 * @return 文本地址属于 224.0.0.0/4 时返回 true。
 */
static bool isMulticastIpv4(const QString &ipText)
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

/** @brief 直播探测解析出的 UDP 单播或组播目标。 */
struct UdpProbeTarget
{
    QString host;          /**< 目标 IPv4 地址或主机名。 */
    quint16 port = 0;      /**< UDP 端口；0 表示未解析。 */
    bool ok = false;       /**< 地址与端口均有效时为 true。 */
    bool multicast = false; /**< 目标属于 IPv4 组播范围时为 true。 */
};

/**
 * @brief 从直播 URL 解析 UDP 地址、端口及组播属性。
 * @param[in] url 直播源地址，支持 udp://@host:port。
 * @return 解析失败时返回 ok=false 的目标。
 */
static UdpProbeTarget parseUdpProbeTarget(const QString &url)
{
    UdpProbeTarget target;

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
    target.multicast = isMulticastIpv4(host);
    return target;
}

/**
 * @return SDP 连接字段声明组播地址时返回 true。
 */
static bool sdpLooksLikeMulticast(const QString &sdp)
{
    const QStringList lines = sdp.split('\n', QString::SkipEmptyParts);
    for (QString line : lines) {
        line = line.trimmed();
        if (!line.startsWith(QStringLiteral("c=IN IP4"))) {
            continue;
        }

        const QStringList fields = line.split(QRegExp("\\s+"), QString::SkipEmptyParts);
        if (fields.size() >= 3 && isMulticastIpv4(fields.at(2))) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 从 RTSP 状态行解析三位状态码。
 */
static int parseRtspStatusCode(const QByteArray &statusLine)
{
    const QList<QByteArray> parts = statusLine.trimmed().split(' ');
    if (parts.size() < 2) {
        return 0;
    }

    bool ok = false;
    const int code = parts.at(1).toInt(&ok);
    return ok ? code : 0;
}

/**
 * @brief 从 RTSP 头部解析 Content-Length。
 */
static int parseRtspContentLength(const QByteArray &header)
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
    QByteArray body;        /**< SDP 或其他响应正文。 */
    int consumedBytes = 0;  /**< 当前响应在累计缓冲区中占用的字节数。 */
};

/**
 * @brief 从累计 TCP 缓冲区提取一个完整 RTSP 响应。
 */
static RtspProbeResponse parseRtspResponse(const QByteArray &buffer)
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
    const int contentLength = parseRtspContentLength(header);

    const int totalLen = headerEnd + sepLen + contentLength;
    if (buffer.size() < totalLen) {
        return resp;
    }

    resp.complete = true;
    resp.statusCode = parseRtspStatusCode(statusLine);
    resp.body = buffer.mid(headerEnd + sepLen, contentLength);
    resp.consumedBytes = totalLen;
    return resp;
}

/**
 * @brief 从 SDP 中选择视频媒体段的 control 值。
 * @param sdpBody RTSP DESCRIBE 响应中的 SDP 正文字节。
 * @return 首个视频媒体段内的非空 control；未找到时返回空字符串。
 * @note 不使用会话级或音频轨的 control 来证明视频轨存在。
 */
static QString findRtspVideoControl(const QByteArray &sdpBody)
{
    const QString sdp = QString::fromUtf8(sdpBody);
    const QStringList lines = sdp.split('\n', QString::SkipEmptyParts);

    bool inVideoBlock = false;

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
    }

    return QString();
}

/**
 * @brief 将 SDP control 字段解析为绝对 RTSP 地址。
 */
static QString buildRtspControlUrl(const QUrl &baseUrl, const QString &control)
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

/**
 * @return HTTP 状态码表示成功时返回 true。
 */
static bool isHttpSuccessStatus(int statusCode)
{
    return statusCode == 200 || statusCode == 204 || statusCode == 206;
}

/**
 * @return HTTP 状态码表示重定向时返回 true。
 */
static bool isHttpRedirectStatus(int statusCode)
{
    return statusCode == 301 || statusCode == 302 || statusCode == 303 ||
           statusCode == 307 || statusCode == 308;
}

/**
 * @return 响应正文更像 HTML 错误页而非媒体数据时返回 true。
 */
static bool isProbablyHtmlBody(const QByteArray &body)
{
    const QByteArray trimmed = body.left(256).trimmed().toLower();
    return trimmed.startsWith("<!doctype html") ||
           trimmed.startsWith("<html") ||
           trimmed.contains("<html");
}

/**
 * @return 正文包含 HLS 播放列表标记时返回 true。
 */
static bool isLikelyHlsPlaylist(const QByteArray &body)
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

/**
 * @return 数据具有 MPEG-TS 同步字节特征时返回 true。
 */
static bool isLikelyTsBody(const QByteArray &body)
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

/**
 * @return 数据具有 FLV 文件头时返回 true。
 */
static bool isLikelyFlvBody(const QByteArray &body)
{
    return body.size() >= 3 && body.left(3) == "FLV";
}

/**
 * @return 数据具有 MP4 box 文件头时返回 true。
 */
static bool isLikelyMp4Body(const QByteArray &body)
{
    return body.size() >= 12 && body.mid(4, 4) == "ftyp";
}

/**
 * @brief 综合 URL、Content-Type 和正文魔数判断 HTTP 直播内容。
 */
static bool isLikelyLiveHttpBody(const QString &url,
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
        if (isLikelyHlsPlaylist(body)) {
            if (detail) *detail = QStringLiteral("HLS playlist ok");
            return true;
        }
        if (detail) *detail = QStringLiteral("HLS playlist invalid or empty");
        return false;
    }

    if (lowerUrl.contains(QStringLiteral(".ts")) || ct.contains(QStringLiteral("mp2t"))) {
        if (isLikelyTsBody(body) || ct.contains(QStringLiteral("mp2t"))) {
            if (detail) *detail = QStringLiteral("HTTP-TS ok");
            return true;
        }
        if (detail) *detail = QStringLiteral("HTTP-TS body invalid");
        return false;
    }

    if (lowerUrl.contains(QStringLiteral(".flv")) || ct.contains(QStringLiteral("flv"))) {
        if (isLikelyFlvBody(body) || ct.contains(QStringLiteral("flv"))) {
            if (detail) *detail = QStringLiteral("HTTP-FLV ok");
            return true;
        }
        if (detail) *detail = QStringLiteral("HTTP-FLV body invalid");
        return false;
    }

    if (lowerUrl.contains(QStringLiteral(".mp4")) || ct.contains(QStringLiteral("mp4"))) {
        if (isLikelyMp4Body(body) || ct.contains(QStringLiteral("mp4"))) {
            if (detail) *detail = QStringLiteral("HTTP-MP4 ok");
            return true;
        }
        if (detail) *detail = QStringLiteral("HTTP-MP4 body invalid");
        return false;
    }

    // 未带典型扩展名的 HTTP 直播，例如 /live、/stream。
    // 不能只看 200，否则普通网页也会误切 LIVE；至少要求不是 HTML，且 content-type 或 body 看起来像媒体。
    if (isProbablyHtmlBody(body)) {
        if (detail) *detail = QStringLiteral("HTTP body is html, not media stream");
        return false;
    }

    if (ct.startsWith(QStringLiteral("video/")) ||
        ct.startsWith(QStringLiteral("audio/")) ||
        ct.contains(QStringLiteral("octet-stream")) ||
        ct.contains(QStringLiteral("mpegurl")) ||
        isLikelyTsBody(body) ||
        isLikelyFlvBody(body) ||
        isLikelyMp4Body(body)) {
        if (detail) *detail = QStringLiteral("generic HTTP media ok");
        return true;
    }

    if (detail) *detail = QStringLiteral("HTTP response is not recognized as media stream") +
                          QStringLiteral(" content-type=") + contentType;
    return false;
}
/** @return 响应正文看起来是 HTML 错误页时返回 true。 */
bool isWarningHtmlBody(const QByteArray &body)
{
    const QByteArray trimmed = body.left(512).trimmed().toLower();
    return trimmed.startsWith("<!doctype html") ||
           trimmed.startsWith("<html") ||
           trimmed.contains("<head") ||
           trimmed.contains("<body");
}

/** @brief 结合 URL、Content-Type 和正文特征判断是否为直播内容。 */
bool isWarningMediaBody(const QString &url,
                                                const QString &contentType,
                                                const QByteArray &body,
                                                QString *detail)
{
    const QString lowerUrl = url.toLower();
    const QString ct = contentType.toLower();

    if (body.isEmpty()) {
        if (detail) {
            *detail = QStringLiteral("HTTP 无媒体数据");
        }
        return false;
    }

    if (isWarningHtmlBody(body)) {
        if (detail) {
            *detail = QStringLiteral("HTTP 返回网页而不是媒体流");
        }
        return false;
    }

    if (ct.contains(QStringLiteral("mpegurl")) ||
        ct.contains(QStringLiteral("mp2t")) || ct.contains(QStringLiteral("video")) ||
        ct.contains(QStringLiteral("octet-stream"))) {
        if (detail) {
            *detail = QStringLiteral("HTTP media response ok");
        }
        return true;
    }

    if (lowerUrl.contains(QStringLiteral(".m3u8"))) {
        if (body.contains("#EXTM3U")) {
            if (detail) {
                *detail = QStringLiteral("HLS playlist ok");
            }
            return true;
        }
        if (detail) {
            *detail = QStringLiteral("HLS 播放列表无效");
        }
        return false;
    }

    if (lowerUrl.contains(QStringLiteral(".ts")) || lowerUrl.contains(QStringLiteral("live"))) {
        if (detail) {
            *detail = QStringLiteral("HTTP body ok");
        }
        return true;
    }

    if (detail) {
        *detail = QStringLiteral("HTTP 响应不像直播媒体");
    }
    return false;
}

/** @brief RTSP SDP 中声明的组播地址和端口。 */
struct RtspMulticastInfo {
    bool ok = false; /**< 组播地址和视频端口均有效。 */
    QString group; /**< IPv4 组播地址。 */
    quint16 port = 0; /**< 视频 UDP 端口；0 表示未解析。 */
};

/** @brief 从 SDP 媒体段提取视频组播端点。
 * @param[in] sdpBody RTSP DESCRIBE 响应正文。
 * @return 缺少组播地址或视频端口时返回 ok=false。
 */
RtspMulticastInfo findRtspMulticastInfo(const QByteArray &sdpBody)
{
    RtspMulticastInfo info;

    const QString sdp = QString::fromUtf8(sdpBody);
    const QStringList lines = sdp.split('\n', QString::SkipEmptyParts);

    QString sessionGroup;
    QString videoGroup;
    quint16 videoPort = 0;
    bool inVideoBlock = false;

    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }

        if (line.startsWith(QStringLiteral("m="))) {
            inVideoBlock = line.startsWith(QStringLiteral("m=video"));
            if (inVideoBlock) {
                const QStringList fields = line.split(QRegExp("\\s+"), QString::SkipEmptyParts);
                if (fields.size() >= 2) {
                    bool ok = false;
                    const int port = fields.at(1).toInt(&ok);
                    if (ok && port > 0 && port <= 65535) {
                        videoPort = static_cast<quint16>(port);
                    }
                }
            }
            continue;
        }

        if (line.startsWith(QStringLiteral("c=IN IP4"))) {
            const QStringList fields = line.split(QRegExp("\\s+"), QString::SkipEmptyParts);
            if (fields.size() >= 3) {
                QString ip = fields.at(2).trimmed();
                const int slash = ip.indexOf('/');
                if (slash >= 0) {
                    ip = ip.left(slash);
                }
                if (isMulticastIpv4(ip)) {
                    if (inVideoBlock) {
                        videoGroup = ip;
                    } else if (sessionGroup.isEmpty()) {
                        sessionGroup = ip;
                    }
                }
            }
        }
    }

    const QString group = !videoGroup.isEmpty() ? videoGroup : sessionGroup;
    if (!group.isEmpty() && videoPort > 0) {
        info.ok = true;
        info.group = group;
        info.port = videoPort;
    }
    return info;
}


/** @brief 向当前 RTSP 套接字发送 DESCRIBE 或 SETUP。
 * @param[in] socket 借用的当前请求套接字。
 * @param[in] url 原地址或已解析的媒体轨地址。
 * @param[in] agent 原调用方使用的 User-Agent。
 * @param[in] setup true 发送 SETUP，否则发送 DESCRIBE。
 */
void sendRtspRequest(QTcpSocket *socket, const QString &url, const QByteArray &agent, bool setup)
{
    QByteArray request = setup ? "SETUP " : "DESCRIBE ";
    request += QUrl(url).toEncoded(QUrl::FullyEncoded);
    request += " RTSP/1.0\r\n";
    request += setup ? "CSeq: 2\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                     : "CSeq: 1\r\nAccept: application/sdp\r\n";
    request += "User-Agent: " + agent + "\r\n\r\n";
    socket->write(request);
    socket->flush();
}

} // namespace

/**
 * @brief 一轮网络线程内的直播探测。
 * @details 计时器与传输回调都绑定本请求。停止时先撤销回调，再中止传输，
 * 最后递送结果，避免 abort 同步触发 finished 后读取已关闭的 reply。
 */
class LiveStreamProbe::Request final : public QObject
{
public:
    /** @brief 保存本轮探测快照并建立超时回调。
     * @param[in] owner 拥有本轮及共享 HTTP 管理器的服务。
     * @param[in] purpose 调用方及对应策略。
     * @param[in] url 待探测直播地址。
     * @param[in] kind 业务协议，可空。
     * @param[in] completion 本轮完成后执行的网络线程回调。
     */
    Request(LiveStreamProbe *owner, Purpose purpose, const QString &url,
            const QString &kind, Completion completion)
        : QObject(owner), owner_(owner), purpose_(purpose), source_(url), url_(url),
          kind_(kind.trimmed().toLower()), completion_(std::move(completion))
    {
        if (kind_.isEmpty() || (isPlayback() && kind_ == QStringLiteral("live"))) {
            kind_ = LiveStreamProbe::kindFromUrl(source_);
        }
        scheme_ = url_.scheme().toLower();
        if (scheme_.isEmpty()) scheme_ = kind_;
        timeout_.setSingleShot(true);
        connect(&timeout_, &QTimer::timeout, this, [this] { finish(false, timeoutDetail_); });
    }

    /** @brief 根据调用方和协议启动探测；配置无效或链路断开时直接给出结果。 */
    void start()
    {
        startedMs_ = MonotonicClock::nowMs();
        if (isPlayback() && !isWiredLinkUp()) {
            finish(false, QStringLiteral("网口 carrier=0"), true);
            return;
        }
        if (isRawUdpLiveKind(scheme_, kind_)) {
            startUdp(parseUdpProbeTarget(source_));
        } else if (isPlayback() && (scheme_ == QStringLiteral("srt") || kind_ == QStringLiteral("srt"))) {
            finish(true, QStringLiteral("SRT/special carrier up"));
        } else if (scheme_ == QStringLiteral("http") || scheme_ == QStringLiteral("https") ||
                   kind_ == QStringLiteral("hls") || kind_ == QStringLiteral("http-ts")) {
            if (url_.isValid() && !url_.host().isEmpty()) startHttp();
            else finish(false, QStringLiteral("直播地址无效"));
        } else if (scheme_ == QStringLiteral("rtsp") || scheme_ == QStringLiteral("rtmp") ||
                   (isPlayback() && scheme_ == QStringLiteral("rtmps"))) {
            if (!url_.host().isEmpty() && (isPlayback() || url_.isValid())) startTcp();
            else finish(false, QStringLiteral("直播地址无效"));
        } else {
            finish(false, QStringLiteral("当前协议暂不支持后台确认"));
        }
    }

    /** @brief 取消本轮并排队清理；不递送业务结果。 */
    void cancel()
    {
        completion_ = {};
        finish(false, QString(), false, false);
    }

private:
    /** @return 播放器重试使用 true，网络告警使用 false。 */
    bool isPlayback() const { return purpose_ == Purpose::PlaybackRetry; }
    /** @return 用于日志区分调用方的名称。 */
    const char *purposeName() const { return isPlayback() ? "playback" : "warning"; }
    /** @return 原调用方的网络请求 User-Agent。 */
    QByteArray userAgent() const
    {
        return isPlayback() ? QByteArrayLiteral("qt_ycest-live-probe")
                            : QByteArrayLiteral("qt_ycest-network-status-stream-probe");
    }

    /** @brief 启动当前阶段的超时。
     * @param[in] ms 该阶段超时，单位毫秒。
     * @param[in] detail 超时结果的说明。
     */
    void armTimeout(int ms, const QString &detail)
    {
        timeoutDetail_ = detail;
        timeout_.start(ms);
    }

    /** @brief 在共享管理器中发起 GET，只有正常完成才读取仍打开的 reply。 */
    void startHttp()
    {
        if (!owner_->httpManager_) {
            owner_->httpManager_ = new QNetworkAccessManager(owner_);
            qDebug() << "[直播探测] 创建共享 HTTP 管理器" << "thread=" << QThread::currentThread()->objectName();
        }
        QNetworkRequest request(url_);
        request.setRawHeader("User-Agent", userAgent());
        request.setRawHeader("Accept", "*/*");
        request.setRawHeader("Connection", "close");
        request.setRawHeader("Range", "bytes=0-4095");
        request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);

        reply_ = owner_->httpManager_->get(request);
        // Qt 5 会在 error 之后发 finished；避免在 error 回调内再次 abort。
        connect(reply_, &QNetworkReply::finished, this, [this] {
            const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (isHttpRedirectStatus(status)) {
                finish(false, QStringLiteral("HTTP 重定向未解析:%1").arg(status));
                return;
            }
            if (!isHttpSuccessStatus(status) || reply_->error() != QNetworkReply::NoError ||
                !reply_->isOpen()) {
                finish(false, status ? QStringLiteral("HTTP 状态:%1").arg(status)
                                     : QStringLiteral("HTTP 连接错误"));
                return;
            }
            const auto contentType = QString::fromLatin1(reply_->rawHeader("Content-Type"));
            const auto body = reply_->readAll();
            QString detail;
            const bool ok = isPlayback()
                    ? isLikelyLiveHttpBody(source_, contentType, body, &detail)
                    : isWarningMediaBody(source_, contentType, body, &detail);
            finish(ok, detail);
        });
        armTimeout(isPlayback() ? 4000 : 4500, QStringLiteral("HTTP 探测超时"));
    }

    /** @brief 为单播或组播绑定 UDP 端口，收到数据包即完成本轮。
     * @param[in] target UDP 地址和端口，可能无效。
     * @param[in] prefix 告警的 RTSP 组播阶段说明，可空。
     */
    void startUdp(const UdpProbeTarget &target, const QString &prefix = {})
    {
        if (!target.ok || !target.port) {
            finish(false, QStringLiteral("UDP 直播地址缺少端口"));
            return;
        }

        udp_ = new QUdpSocket(this);
        if (!udp_->bind(QHostAddress::AnyIPv4, target.port,
                       QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            finish(false, prefix + QStringLiteral("UDP 端口绑定失败:%1").arg(target.port));
            return;
        }
        if (target.multicast && !udp_->joinMulticastGroup(QHostAddress(target.host))) {
            finish(false, prefix + QStringLiteral("加入组播失败:%1:%2").arg(target.host).arg(target.port));
            return;
        }

        connect(udp_, &QUdpSocket::readyRead, this, [this] {
            char datagram[2048];
            if (udp_->readDatagram(datagram, sizeof(datagram)) >= 0) {
                finish(true, QStringLiteral("UDP media packet received"));
            }
        });
        const QString detail = target.multicast
                ? QStringLiteral("未收到组播数据包:%1:%2").arg(target.host).arg(target.port)
                : QStringLiteral("未收到 UDP 数据包:%1").arg(target.port);
        armTimeout(3500, prefix + detail);
    }

    /** @brief 启动 RTSP 或 RTMP 的原 TCP 探测，按调用方采用原超时。 */
    void startTcp()
    {
        const bool rtsp = scheme_ == QStringLiteral("rtsp");
        const bool rtmp = scheme_ == QStringLiteral("rtmp");
        const int port = url_.port() > 0 ? url_.port() : (rtsp ? 554 : rtmp ? 1935 : 443);
        tcp_ = new QTcpSocket(this);
        connect(tcp_, &QTcpSocket::connected, this, [this, rtsp, rtmp] {
            if (rtsp) {
                sendRtspRequest(tcp_, source_, userAgent(), false);
            } else if (rtmp) {
                QByteArray request(1537, '\0');
                request[0] = 0x03;
                tcp_->write(request);
                tcp_->flush();
            } else {
                finish(false, QStringLiteral("RTMPS 当前没有可用的轻量探测"));
            }
        });
        connect(tcp_, &QTcpSocket::readyRead, this, [this, rtsp] {
            buffer_ += tcp_->readAll();
            if (rtsp) readRtspResponse();
            else if (buffer_.size() >= 1537 && static_cast<unsigned char>(buffer_.at(0)) == 0x03)
                finish(true, QStringLiteral("RTMP handshake ok"));
        });
        connect(tcp_, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
                this, [this, rtsp](QAbstractSocket::SocketError) {
            finish(false, rtsp ? QStringLiteral("RTSP 连接错误") : QStringLiteral("RTMP 连接错误"));
        });
        armTimeout(isPlayback() ? 3500 : 4500,
                   rtsp ? QStringLiteral("RTSP 探测超时") : QStringLiteral("RTMP 探测超时"));
        tcp_->connectToHost(url_.host(), static_cast<quint16>(port));
    }

    /** @brief 消费完整 RTSP 响应，保持重试与告警各自的组播确认规则。 */
    void readRtspResponse()
    {
        const auto response = parseRtspResponse(buffer_);
        if (!response.complete) return;
        buffer_.remove(0, response.consumedBytes);
        if (response.statusCode != 200) {
            finish(false, QStringLiteral("RTSP %1 失败:%2")
                   .arg(setup_ ? QStringLiteral("SETUP") : QStringLiteral("DESCRIBE")).arg(response.statusCode));
            return;
        }
        if (setup_) {
            finish(true, QStringLiteral("RTSP DESCRIBE+SETUP ok"));
            return;
        }

        const QString control = findRtspVideoControl(response.body);
        if (control.isEmpty()) {
            finish(false, QStringLiteral("RTSP 未发现视频轨道"));
            return;
        }
        if (isPlayback()) {
            if (sdpLooksLikeMulticast(QString::fromUtf8(response.body))) {
                finish(true, QStringLiteral("RTSP multicast DESCRIBE ok"));
                return;
            }
        } else {
            const auto multicast = findRtspMulticastInfo(response.body);
            if (multicast.ok) {
                closeTcp();
                startUdp({multicast.group, multicast.port, true, true}, QStringLiteral("RTSP 组播 SDP 正常，"));
                return;
            }
        }
        const QString setupUrl = buildRtspControlUrl(url_, control);
        if (setupUrl.trimmed().isEmpty()) {
            finish(false, QStringLiteral("RTSP video control 为空"));
            return;
        }
        setup_ = true;
        sendRtspRequest(tcp_, setupUrl, userAgent(), true);
    }

    /** @brief 解除并关闭本轮 TCP；RTSP 组播可以继续在同一轮中进行 UDP 探测。 */
    void closeTcp()
    {
        if (!tcp_) return;
        auto *socket = std::exchange(tcp_, nullptr);
        disconnect(socket, nullptr, this, nullptr);
        socket->abort();
        socket->deleteLater();
    }

    /** @brief 清理传输，记录首个失败或恢复，并在收尾后执行可选回调。
     * @param[in] available 探测是否通过。
     * @param[in] detail 探测结果说明。
     * @param[in] linkDown 是否因物理链路断开跳过。
     * @param[in] logResult false 表示请求被取消，保留已有日志去重状态。
     */
    void finish(bool available, const QString &detail, bool linkDown = false, bool logResult = true)
    {
        owner_->active(purpose_) = nullptr;
        timeout_.stop();
        const qint64 cleanupStart = MonotonicClock::nowMs();
        if (auto *reply = std::exchange(reply_, nullptr)) {
            disconnect(reply, nullptr, this, nullptr);
            if (!reply->isFinished()) reply->abort();
            reply->deleteLater();
        }
        closeTcp();
        if (udp_) {
            disconnect(udp_, nullptr, this, nullptr);
            udp_->close();
            udp_->deleteLater();
            udp_ = nullptr;
        }
        auto &loggedFailure = isPlayback() ? owner_->playbackLoggedFailure_ : owner_->warningLoggedFailure_;
        if (logResult && available == loggedFailure.has_value()) {
            const qint64 now = MonotonicClock::nowMs();
            qDebug() << "[直播探测] 完成" << "purpose=" << purposeName()
                     << "scheme=" << scheme_ << "host=" << url_.host()
                     << "available=" << available << "elapsedMs=" << now - startedMs_
                     << "cleanupMs=" << now - cleanupStart << "detail=" << detail;
            if (available) loggedFailure.reset();
            else loggedFailure = source_;
        }
        auto completion = std::move(completion_);
        deleteLater();
        if (completion) completion({available, linkDown, detail});
    }

    LiveStreamProbe *const owner_; /**< 借用 QObject 父对象，与本轮处于同一网络线程。 */
    const Purpose purpose_; /**< 本轮独立的协议判定策略。 */
    const QString source_; /**< 原直播地址，用于媒体判定和 RTSP 请求。 */
    const QUrl url_; /**< 解析后的目标地址。 */
    QString kind_; /**< 配置中指定或推断的协议。 */
    QString scheme_; /**< 本轮实际用于路由请求的协议。 */
    Completion completion_; /**< 网络线程回调，取消时清空。 */
    QTimer timeout_; /**< 本轮阶段超时，在所属网络线程启动和停止。 */
    QString timeoutDetail_; /**< 当前阶段超时使用的说明。 */
    qint64 startedMs_ = 0; /**< 本轮起点，单调毫秒。 */
    QNetworkReply *reply_ = nullptr; /**< 由共享管理器拥有，仅借用本轮 reply。 */
    QTcpSocket *tcp_ = nullptr; /**< 本轮拥有的 TCP 套接字，可空。 */
    QUdpSocket *udp_ = nullptr; /**< 本轮拥有的 UDP 套接字，可空。 */
    QByteArray buffer_; /**< 等待解析的 TCP 响应。 */
    bool setup_ = false; /**< true 表示 RTSP 已进入 SETUP 阶段。 */
};

LiveStreamProbe::LiveStreamProbe(QObject *parent) : QObject(parent)
{
}

LiveStreamProbe::Request *&LiveStreamProbe::active(Purpose purpose)
{
    return purpose == Purpose::PlaybackRetry ? playback_ : warning_;
}

void LiveStreamProbe::start(Purpose purpose, const QString &url, const QString &kind, Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    cancel(purpose);
    auto &loggedFailure = purpose == Purpose::PlaybackRetry ? playbackLoggedFailure_ : warningLoggedFailure_;
    if (loggedFailure && *loggedFailure != url) loggedFailure.reset();
    auto *request = new Request(this, purpose, url, kind, std::move(completion));
    active(purpose) = request;
    request->start();
}

void LiveStreamProbe::cancel(Purpose purpose)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (auto *request = active(purpose)) request->cancel();
}

QString LiveStreamProbe::kindFromUrl(const QString &url)
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
