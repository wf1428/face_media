/**
 * @file network_status_monitor.cpp
 * @brief 网络链路、外网和直播源可用性的分阶段监测告警控件的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "network_status_monitor.h"

#include <QApplication>
#include <QPainter>
#include <QFontMetrics>
#include <QScreen>
#include <QEvent>
#include <QLinearGradient>
#include <QSettings>
#include <QFile>
#include <QDir>
#include <QRegExp>
#include <QDebug>
#include <QUrl>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QDateTime>
#include "ic_board/ic_event_bridge.h"
#include "platform/rk3566_platform.h"

/** @brief 创建网络检查、告警闪现和探测超时定时器。 */
NetworkStatusMonitor::NetworkStatusMonitor(QWidget *mainWindow,
                                           const QString &configPath)
    : QWidget(mainWindow),
      main_(mainWindow),
      configPath_(configPath.isEmpty() ? Rk3566Platform::netConfigPath() : configPath)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);

    baseFont_ = QApplication::font();

    if (main_) {
        main_->installEventFilter(this);
        setGeometry(main_->rect());
    }

    // 只用 IP，避免“DNS 本身坏了”时把探测卡在域名解析上。
    // 任意一个目标能 ping 通就认为可上网；不要因为单个公共 DNS 不通误报。
    // 114.114.114.114 在部分网络会被屏蔽或丢包，因此不作为默认探测目标。
    pingTargets_ << QStringLiteral("223.5.5.5")
                 << QStringLiteral("8.8.8.8")
                 << QStringLiteral("119.29.29.29");

    checkTimer_.setInterval(checkIntervalMs_);
    connect(&checkTimer_, &QTimer::timeout,
            this, &NetworkStatusMonitor::checkNetworkStatus);

    blinkTimer_.setInterval(blinkIntervalMs_);
    connect(&blinkTimer_, &QTimer::timeout,
            this, &NetworkStatusMonitor::onBlinkTimeout);

    autoHideTimer_.setSingleShot(true);
    connect(&autoHideTimer_, &QTimer::timeout, this, [this]() {
        hideWarningNow();
    });

    pingTimeoutTimer_.setSingleShot(true);
    connect(&pingTimeoutTimer_, &QTimer::timeout,
            this, &NetworkStatusMonitor::onPingTimeout);

    connect(&pingProcess_, SIGNAL(finished(int,QProcess::ExitStatus)),
            this, SLOT(onPingFinished(int,QProcess::ExitStatus)));

    hide();
}

/** @brief 启动周期检查并立即执行首轮检测。 */
void NetworkStatusMonitor::start()
{
    checkNetworkStatus();
    checkTimer_.start();
}

/** @brief 停止定时器、进程和所有进行中的流探测。 */
void NetworkStatusMonitor::stop()
{
    checkTimer_.stop();
    blinkTimer_.stop();
    autoHideTimer_.stop();
    pingTimeoutTimer_.stop();

    if (pingProcess_.state() != QProcess::NotRunning) {
        pingProcess_.kill();
        pingProcess_.waitForFinished(100);
    }

    cancelStreamProbe();

    checkState_ = CheckState::Idle;
    warningKind_ = WarningKind::None;
    warningActive_ = false;
    warningVisible_ = false;
    hide();
}

/** @return 当前存在网络或直播源告警时返回 true。 */
bool NetworkStatusMonitor::isWarningActive() const
{
    return warningActive_;
}

/** @return 配置允许联网业务时返回 true。 */
bool NetworkStatusMonitor::isOnlineFeatureEnabled() const
{
    QSettings ini(configPath_, QSettings::IniFormat);
    const bool onlineV1  = ini.value(QStringLiteral("feature/online_v1"), false).toBool();
    const bool onlineV2  = ini.value(QStringLiteral("feature/online_v2"), false).toBool();
    const bool offlineV1 = ini.value(QStringLiteral("feature/offline_v1"), false).toBool();

    // offline_v1 或未启用在线模式时，不显示网络异常提示。
    return !offlineV1 && (onlineV1 || onlineV2);
}

/** @brief 读取有线网卡 carrier；valid 表示系统节点是否可读。 */
bool NetworkStatusMonitor::isWiredLinkUp(bool *valid) const
{
    if (valid) {
        *valid = false;
    }

    auto readCarrier = [&](const QString &iface, bool *ok) -> bool {
        if (ok) {
            *ok = false;
        }

        QFile f(QStringLiteral("/sys/class/net/%1/carrier").arg(iface));
        if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
            return false;
        }

        const QString v = QString::fromLatin1(f.readAll()).trimmed();
        if (v.isEmpty()) {
            return false;
        }

        if (ok) {
            *ok = true;
        }
        return v == QStringLiteral("1");
    };

    const QStringList preferredIfaces = {
        QStringLiteral("eth0"),
        QStringLiteral("end0")
    };

    for (const QString &iface : preferredIfaces) {
        bool ok = false;
        const bool up = readCarrier(iface, &ok);
        if (ok) {
            if (valid) {
                *valid = true;
            }
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

        bool ok = false;
        const bool up = readCarrier(iface, &ok);
        if (!ok) {
            continue;
        }

        sawCarrier = true;
        if (up) {
            if (valid) {
                *valid = true;
            }
            return true;
        }
    }

    if (valid) {
        *valid = sawCarrier;
    }

    // 没有 carrier 文件时，不直接判定断网，继续走默认路由/外网探测。
    return !sawCarrier;
}

/** @return 路由表中存在默认路由时返回 true。 */
bool NetworkStatusMonitor::hasDefaultRoute() const
{
    // /proc/net/route 是最轻量的判断方式，但不同 BusyBox/内核裁剪环境下，
    // 路由信息可能显示不完整；所以这里只作为“参考状态”，不能在 ping 前直接判死。
    QFile f(QStringLiteral("/proc/net/route"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!f.atEnd()) {
            const QString line = QString::fromLatin1(f.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QStringLiteral("Iface"))) {
                continue;
            }

            const QStringList fields = line.split(QRegExp("\\s+"), QString::SkipEmptyParts);
            if (fields.size() < 4) {
                continue;
            }

            const QString destination = fields.at(1).trimmed().toUpper();
            bool ok = false;
            const int flags = fields.at(3).trimmed().toInt(&ok, 16);

            // Destination=00000000 表示默认路由；RTF_UP=0x1。
            // 常见 Flags 为 0003，表示 UP + GATEWAY。
            if (destination == QStringLiteral("00000000") && (!ok || (flags & 0x1))) {
                return true;
            }
        }
    }

    // 兜底1：ip route。部分系统 /proc/net/route 不可靠，但 ip route 能显示 default。
    {
        QProcess p;
        p.start(QStringLiteral("ip"), QStringList() << QStringLiteral("route") << QStringLiteral("show") << QStringLiteral("default"));
        if (p.waitForFinished(800)) {
            const QString out = QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
            if (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0 && out.startsWith(QStringLiteral("default"))) {
                return true;
            }
        }
    }

    // 兜底2：BusyBox route -n。
    {
        QProcess p;
        p.start(QStringLiteral("route"), QStringList() << QStringLiteral("-n"));
        if (p.waitForFinished(800)) {
            const QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
            const QStringList lines = out.split(QRegExp("[\\r\\n]+"), QString::SkipEmptyParts);
            for (const QString &line : lines) {
                const QString t = line.trimmed();
                if (t.startsWith(QStringLiteral("0.0.0.0 ")) ||
                    t.startsWith(QStringLiteral("0.0.0.0\t")) ||
                    t.startsWith(QStringLiteral("default ")) ||
                    t.startsWith(QStringLiteral("default\t"))) {
                    return true;
                }
            }
        }
    }

    return false;
}

/** @brief 启动一轮非重入的分阶段网络检查。 */
void NetworkStatusMonitor::checkNetworkStatus()
{
    if (checkState_ == CheckState::Pinging || checkState_ == CheckState::StreamProbing) {
        return;
    }

    if (!isOnlineFeatureEnabled()) {
        // offline_v1 不参与本次 online_v1 断线通行判定。
        IcEventBridge::instance()->updateNetworkAvailability(true);
        clearWarning();
        return;
    }

    bool carrierValid = false;
    const bool linkUp = isWiredLinkUp(&carrierValid);
    if (carrierValid && !linkUp) {
        IcEventBridge::instance()->updateNetworkAvailability(false);
        setWarning(WarningKind::LinkDown);
        return;
    }

    // 注意：不能因为“默认网关检测失败”就立刻弹网络异常。
    // 实测 TinaLinux 上可能出现 /proc/net/route/route 命令解析不到默认网关，
    // 但实际 ping 223.5.5.5/8.8.8.8 正常的情况。
    // 所以默认网关只作为失败时的附加原因，最终以外网探测结果为准。
    lastDefaultRouteOk_ = hasDefaultRoute();

    beginInternetProbe();
}

/** @brief 依次异步 ping 配置的外网目标。 */
void NetworkStatusMonitor::beginInternetProbe()
{
    if (pingTargets_.isEmpty()) {
        finishInternetProbe(false);
        return;
    }

    checkState_ = CheckState::Pinging;
    pingTargetIndex_ = 0;

    const QString target = pingTargets_.at(pingTargetIndex_);
    QStringList args;
    args << QStringLiteral("-c") << QStringLiteral("1")
         << QStringLiteral("-W") << QStringLiteral("2")
         << target;

    pingProcess_.start(QStringLiteral("ping"), args);
    pingTimeoutTimer_.start(pingTimeoutMs_);
}

/** @brief 处理当前 ping 目标结果并决定重试或进入直播探测。 */
void NetworkStatusMonitor::onPingFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    pingTimeoutTimer_.stop();

    const bool ok = (exitStatus == QProcess::NormalExit && exitCode == 0);
    if (ok) {
        finishInternetProbe(true);
        return;
    }

    ++pingTargetIndex_;
    if (pingTargetIndex_ >= pingTargets_.size()) {
        finishInternetProbe(false);
        return;
    }

    const QString target = pingTargets_.at(pingTargetIndex_);
    QStringList args;
    args << QStringLiteral("-c") << QStringLiteral("1")
         << QStringLiteral("-W") << QStringLiteral("2")
         << target;

    pingProcess_.start(QStringLiteral("ping"), args);
    pingTimeoutTimer_.start(pingTimeoutMs_);
}

/** @brief 终止超时的 ping 并尝试下一个目标。 */
void NetworkStatusMonitor::onPingTimeout()
{
    if (pingProcess_.state() != QProcess::NotRunning) {
        // kill 后 QProcess 会发 finished，统一在 onPingFinished() 里按失败处理并切到下一个目标。
        pingProcess_.kill();
        return;
    }

    ++pingTargetIndex_;
    if (pingTargetIndex_ >= pingTargets_.size()) {
        finishInternetProbe(false);
        return;
    }

    const QString target = pingTargets_.at(pingTargetIndex_);
    QStringList args;
    args << QStringLiteral("-c") << QStringLiteral("1")
         << QStringLiteral("-W") << QStringLiteral("2")
         << target;

    pingProcess_.start(QStringLiteral("ping"), args);
    pingTimeoutTimer_.start(pingTimeoutMs_);
}

/** @brief 结束外网探测并继续直播源检查或设置告警。 */
void NetworkStatusMonitor::finishInternetProbe(bool internetOk)
{
    checkState_ = CheckState::Idle;

    if (!isOnlineFeatureEnabled()) {
        IcEventBridge::instance()->updateNetworkAvailability(true);
        clearWarning();
        return;
    }

    // 只用网络链路/外网探测结果更新通行离线状态；直播源无流不属于网络断开。
    IcEventBridge::instance()->updateNetworkAvailability(internetOk);

    if (!internetOk) {
        if (lastDefaultRouteOk_) {
            setWarning(WarningKind::NoInternet, QStringLiteral("外网探测失败"));
        } else {
            setWarning(WarningKind::NoInternet, QStringLiteral("默认网关检测异常"));
        }
        return;
    }

    // 网络和外网都正常后，再判断业务目标 LIVE 是否真的有数据流。
    // 这样可以把“网络问题”和“服务器/推流方无流”分开提示。
    beginLiveStreamProbeIfNeeded();
}


/** @brief 从共享配置读取“期望直播/实际模式/URL/类型”。 */
NetworkStatusMonitor::LiveProbeTarget NetworkStatusMonitor::readLiveProbeTargetFromConfig() const
{
    LiveProbeTarget target;

    QSettings ini(configPath_, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    ini.beginGroup(QStringLiteral("playback"));
    const bool commandReceived = ini.value(QStringLiteral("command_received"), false).toBool();
    const QString targetMode = ini.value(QStringLiteral("target_mode")).toString().trimmed().toUpper();
    const QString currentMode = ini.value(QStringLiteral("current_mode")).toString().trimmed().toUpper();
    const bool fallbackFromLive = ini.value(QStringLiteral("fallback_from_live"), false).toBool();
    QString liveUrl = ini.value(QStringLiteral("live_url")).toString().trimmed();
    QString liveKind = ini.value(QStringLiteral("live_kind")).toString().trimmed().toLower();
    ini.endGroup();

    if (!commandReceived || targetMode != QStringLiteral("LIVE")) {
        return target;
    }

    // 正式已经在LIVE播放时不由提示层重复判断；LIVE内部watchdog负责无帧回退。
    if (currentMode == QStringLiteral("LIVE") && !fallbackFromLive) {
        return target;
    }

    target.needed = true;

    if (liveUrl.isEmpty()) {
        liveUrl = ini.value(QStringLiteral("stream/url")).toString().trimmed();
    }
    if (liveKind.isEmpty()) {
        liveKind = detectLiveKindFromUrl(liveUrl);
    }

    target.url = liveUrl;
    target.kind = liveKind;
    target.ok = !target.url.isEmpty();
    if (!target.ok) {
        target.detail = QStringLiteral("未配置直播地址");
    }
    return target;
}

/** @brief 按 URL scheme 和扩展名推断直播协议类型。 */
QString NetworkStatusMonitor::detectLiveKindFromUrl(const QString &url) const
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

/** @return 业务类型应按裸 UDP 数据流探测时返回 true。 */
bool NetworkStatusMonitor::isRawUdpLiveKind(const QString &scheme, const QString &kind) const
{
    const QString s = scheme.trimmed().toLower();
    const QString k = kind.trimmed().toLower();
    return s == QStringLiteral("udp") ||
           k == QStringLiteral("udp") ||
           k == QStringLiteral("multicast") ||
           k.contains(QStringLiteral("multicast"));
}

/** @return 文本是 IPv4 组播地址时返回 true。 */
bool NetworkStatusMonitor::isMulticastIpv4(const QString &ipText) const
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

/** @brief 解析 UDP URL 中的主机和端口。 */
NetworkStatusMonitor::UdpProbeTarget NetworkStatusMonitor::parseUdpProbeTarget(const QString &url) const
{
    UdpProbeTarget target;

    const QString text = url.trimmed();
    const QUrl u(text);

    QString host = u.host().trimmed();
    int port = u.port();

    // 兼容 udp://239.1.1.1:5000、udp://@239.1.1.1:5000、udp://0.0.0.0:5000。
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

    target.ok = true;
    target.host = host;
    target.port = static_cast<quint16>(port);
    target.multicast = isMulticastIpv4(host);
    return target;
}

/** @brief 仅在当前业务需要时启动直播源探测。 */
void NetworkStatusMonitor::beginLiveStreamProbeIfNeeded()
{
    const LiveProbeTarget target = readLiveProbeTargetFromConfig();
    if (!target.needed) {
        clearWarning();
        return;
    }

    if (!target.ok) {
        setWarning(WarningKind::StreamNoData, target.detail);
        return;
    }

    startLiveStreamProbe(target);
}

/** @brief 根据目标协议选择具体异步探测实现。 */
void NetworkStatusMonitor::startLiveStreamProbe(const LiveProbeTarget &target)
{
    cancelStreamProbe();

    QUrl u(target.url);
    QString scheme = u.scheme().trimmed().toLower();
    QString kind = target.kind.trimmed().toLower();
    if (kind.isEmpty() || kind == QStringLiteral("live")) {
        kind = detectLiveKindFromUrl(target.url);
    }
    if (scheme.isEmpty()) {
        scheme = kind;
    }

    checkState_ = CheckState::StreamProbing;
    streamProbeUrl_ = target.url;

    if (isRawUdpLiveKind(scheme, kind)) {
        startUdpStreamProbe(parseUdpProbeTarget(target.url), target.url);
        return;
    }

    if (scheme == QStringLiteral("http") || scheme == QStringLiteral("https") ||
        kind == QStringLiteral("hls") || kind == QStringLiteral("http-ts")) {
        if (!u.isValid() || u.host().isEmpty()) {
            finishStreamProbe(false, QStringLiteral("直播地址无效"));
            return;
        }
        startHttpStreamProbe(u, target.url);
        return;
    }

    if (scheme == QStringLiteral("rtsp")) {
        if (!u.isValid() || u.host().isEmpty()) {
            finishStreamProbe(false, QStringLiteral("RTSP 地址无效"));
            return;
        }
        startRtspStreamProbe(u, target.url);
        return;
    }

    if (scheme == QStringLiteral("rtmp")) {
        if (!u.isValid() || u.host().isEmpty()) {
            finishStreamProbe(false, QStringLiteral("RTMP 地址无效"));
            return;
        }
        startRtmpStreamProbe(u, target.url);
        return;
    }

    // SRT/未知协议不做 carrier-only 误判：既然当前目标是 LIVE 但还没切进去，直接提示直播源不可确认。
    finishStreamProbe(false, QStringLiteral("当前协议暂不支持后台确认"));
}

/** @brief 监听 UDP 数据，组播目标会加入对应组。 */
void NetworkStatusMonitor::startUdpStreamProbe(const UdpProbeTarget &target, const QString &sourceUrl, const QString &detailPrefix)
{
    if (!target.ok || target.port == 0) {
        finishStreamProbe(false, QStringLiteral("UDP 直播地址缺少端口"));
        return;
    }

    if (streamUdpSocket_) {
        streamUdpSocket_->deleteLater();
        streamUdpSocket_ = nullptr;
    }

    QUdpSocket *udp = new QUdpSocket(this);
    streamUdpSocket_ = udp;

    const bool bindOk = udp->bind(QHostAddress::AnyIPv4,
                                  target.port,
                                  QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!bindOk) {
        const QString detail = detailPrefix.isEmpty()
                ? QStringLiteral("UDP 端口绑定失败:%1").arg(target.port)
                : detailPrefix + QStringLiteral("，UDP 端口绑定失败:%1").arg(target.port);
        udp->deleteLater();
        streamUdpSocket_ = nullptr;
        finishStreamProbe(false, detail);
        return;
    }

    if (target.multicast) {
        if (!udp->joinMulticastGroup(QHostAddress(target.host))) {
            const QString detail = detailPrefix.isEmpty()
                    ? QStringLiteral("加入组播失败:%1:%2").arg(target.host).arg(target.port)
                    : detailPrefix + QStringLiteral("，加入组播失败:%1:%2").arg(target.host).arg(target.port);
            udp->close();
            udp->deleteLater();
            streamUdpSocket_ = nullptr;
            finishStreamProbe(false, detail);
            return;
        }
    }

    connect(udp, &QUdpSocket::readyRead, this, [this, udp]() {
        if (udp != streamUdpSocket_) {
            return;
        }
        while (udp->hasPendingDatagrams()) {
            QByteArray datagram;
            const qint64 pendingSize = udp->pendingDatagramSize();
            datagram.resize(static_cast<int>(qMin<qint64>(pendingSize, 2048)));
            udp->readDatagram(datagram.data(), datagram.size());
        }
        finishStreamProbe(true, QStringLiteral("UDP media packet received"));
    });

    QTimer::singleShot(streamProbeTimeoutMs_, udp, [this, udp, target, sourceUrl, detailPrefix]() {
        Q_UNUSED(sourceUrl);
        if (udp != streamUdpSocket_) {
            return;
        }
        const QString base = target.multicast
                ? QStringLiteral("未收到组播数据包:%1:%2").arg(target.host).arg(target.port)
                : QStringLiteral("未收到 UDP 数据包:%1").arg(target.port);
        finishStreamProbe(false, detailPrefix.isEmpty() ? base : detailPrefix + QStringLiteral("，") + base);
    });

    qDebug() << "[网络提示] 开始探测 UDP 直播流数据:" << sourceUrl
             << "host=" << target.host << "port=" << target.port
             << "multicast=" << target.multicast;
}

/** @brief 通过 HTTP 响应头和少量正文判断直播内容。 */
void NetworkStatusMonitor::startHttpStreamProbe(const QUrl &url, const QString &sourceUrl)
{
    streamHttpManager_ = new QNetworkAccessManager(this);
    streamHttpReply_ = nullptr;

    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "qt_ycest-network-status-stream-probe");
    req.setRawHeader("Accept", "*/*");
    req.setRawHeader("Connection", "close");
    req.setRawHeader("Range", "bytes=0-4095");
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif

    streamHttpReply_ = streamHttpManager_->get(req);

    connect(streamHttpReply_, &QNetworkReply::finished, this, [this, sourceUrl]() {
        if (!streamHttpReply_) {
            return;
        }

        const int statusCode = streamHttpReply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString contentType = QString::fromLatin1(streamHttpReply_->rawHeader("Content-Type"));
        const QByteArray body = streamHttpReply_->readAll();

        if (isHttpRedirectStatus(statusCode)) {
            finishStreamProbe(false, QStringLiteral("HTTP 重定向未解析:%1").arg(statusCode));
            return;
        }

        if (!isHttpSuccessStatus(statusCode)) {
            finishStreamProbe(false, QStringLiteral("HTTP 状态:%1").arg(statusCode));
            return;
        }

        QString detail;
        if (!isLikelyLiveHttpBody(sourceUrl, contentType, body, &detail)) {
            finishStreamProbe(false, detail);
            return;
        }

        finishStreamProbe(true, detail);
    });

    connect(streamHttpReply_,
            static_cast<void (QNetworkReply::*)(QNetworkReply::NetworkError)>(&QNetworkReply::error),
            this, [this](QNetworkReply::NetworkError) {
        finishStreamProbe(false, QStringLiteral("HTTP 连接错误"));
    });

    QTimer::singleShot(streamProbeTimeoutMs_ + 1000, streamHttpReply_, [this]() {
        if (!streamHttpReply_) {
            return;
        }
        finishStreamProbe(false, QStringLiteral("HTTP 探测超时"));
    });

    qDebug() << "[网络提示] 开始探测 HTTP 直播流数据:" << sourceUrl;
}

/** @brief 执行 RTSP DESCRIBE/SETUP，并在需要时继续探测 SDP 组播。 */
void NetworkStatusMonitor::startRtspStreamProbe(const QUrl &url, const QString &sourceUrl)
{
    int port = url.port();
    if (port <= 0) {
        port = 554;
    }

    QTcpSocket *sock = new QTcpSocket(this);
    streamTcpSocket_ = sock;
    sock->setProperty("yc_probe_done", false);
    sock->setProperty("yc_probe_buffer", QByteArray());
    sock->setProperty("yc_probe_stage", QStringLiteral("rtsp_describe"));

    connect(sock, &QTcpSocket::connected, this, [this, sock, sourceUrl]() {
        if (sock != streamTcpSocket_) {
            return;
        }
        sendRtspDescribe(sock, sourceUrl);
    });

    connect(sock, &QTcpSocket::readyRead, this, [this, sock, sourceUrl]() {
        if (sock != streamTcpSocket_) {
            return;
        }

        QByteArray buf = sock->property("yc_probe_buffer").toByteArray();
        buf += sock->readAll();
        sock->setProperty("yc_probe_buffer", buf);

        const RtspResponse resp = parseRtspResponse(buf);
        if (!resp.complete) {
            return;
        }

        sock->setProperty("yc_probe_buffer", buf.mid(resp.consumedBytes));
        const QString stage = sock->property("yc_probe_stage").toString();

        if (stage == QStringLiteral("rtsp_describe")) {
            if (resp.statusCode != 200) {
                finishStreamProbe(false, QStringLiteral("RTSP DESCRIBE 失败:%1").arg(resp.statusCode));
                return;
            }

            const QString videoControl = findRtspVideoControl(resp.body);
            if (videoControl.isEmpty()) {
                finishStreamProbe(false, QStringLiteral("RTSP 未发现视频轨道"));
                return;
            }

            // RTSP 组播场景下，DESCRIBE 成功不代表推流方正在发 RTP。
            // 如果 SDP 能解析出组播地址和 video 端口，继续加入组播等待真实 RTP 包。
            const RtspMulticastInfo mcast = findRtspMulticastInfo(resp.body);
            if (mcast.ok) {
                UdpProbeTarget udpTarget;
                udpTarget.ok = true;
                udpTarget.multicast = true;
                udpTarget.host = mcast.group;
                udpTarget.port = mcast.port;

                sock->abort();
                sock->deleteLater();
                if (sock == streamTcpSocket_) {
                    streamTcpSocket_ = nullptr;
                }
                startUdpStreamProbe(udpTarget, sourceUrl, QStringLiteral("RTSP 组播 SDP 正常"));
                return;
            }

            const QString setupUrl = buildRtspControlUrl(QUrl(sourceUrl), videoControl);
            if (setupUrl.trimmed().isEmpty()) {
                finishStreamProbe(false, QStringLiteral("RTSP video control 为空"));
                return;
            }

            sock->setProperty("yc_probe_stage", QStringLiteral("rtsp_setup"));
            sendRtspSetup(sock, setupUrl);
            return;
        }

        if (stage == QStringLiteral("rtsp_setup")) {
            if (resp.statusCode == 200) {
                finishStreamProbe(true, QStringLiteral("RTSP DESCRIBE+SETUP ok"));
                return;
            }
            finishStreamProbe(false, QStringLiteral("RTSP SETUP 失败:%1").arg(resp.statusCode));
            return;
        }

        finishStreamProbe(false, QStringLiteral("RTSP 探测状态异常"));
    });

    connect(sock,
            static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(&QAbstractSocket::error),
            this, [this](QAbstractSocket::SocketError) {
        finishStreamProbe(false, QStringLiteral("RTSP 连接错误"));
    });

    QTimer::singleShot(streamProbeTimeoutMs_ + 1000, sock, [this, sock]() {
        if (sock != streamTcpSocket_) {
            return;
        }
        finishStreamProbe(false, QStringLiteral("RTSP 探测超时"));
    });

    qDebug() << "[网络提示] 开始探测 RTSP 直播流数据:" << sourceUrl;
    sock->connectToHost(url.host(), static_cast<quint16>(port));
}

/** @brief 通过 TCP 握手可达性探测 RTMP 地址。 */
void NetworkStatusMonitor::startRtmpStreamProbe(const QUrl &url, const QString &sourceUrl)
{
    int port = url.port();
    if (port <= 0) {
        port = 1935;
    }

    QTcpSocket *sock = new QTcpSocket(this);
    streamTcpSocket_ = sock;
    sock->setProperty("yc_probe_buffer", QByteArray());

    connect(sock, &QTcpSocket::connected, this, [sock]() {
        QByteArray req;
        req.resize(1537);
        req.fill('\0');
        req[0] = 0x03;
        sock->write(req);
        sock->flush();
    });

    connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
        if (sock != streamTcpSocket_) {
            return;
        }
        QByteArray buf = sock->property("yc_probe_buffer").toByteArray();
        buf += sock->readAll();
        sock->setProperty("yc_probe_buffer", buf);
        if (buf.size() >= 1537 && static_cast<unsigned char>(buf.at(0)) == 0x03) {
            finishStreamProbe(true, QStringLiteral("RTMP handshake ok"));
        }
    });

    connect(sock,
            static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(&QAbstractSocket::error),
            this, [this](QAbstractSocket::SocketError) {
        finishStreamProbe(false, QStringLiteral("RTMP 连接错误"));
    });

    QTimer::singleShot(streamProbeTimeoutMs_ + 1000, sock, [this, sock]() {
        if (sock != streamTcpSocket_) {
            return;
        }
        finishStreamProbe(false, QStringLiteral("RTMP 探测超时"));
    });

    qDebug() << "[网络提示] 开始探测 RTMP 直播流数据:" << sourceUrl;
    sock->connectToHost(url.host(), static_cast<quint16>(port));
}

/** @brief 收束流探测资源并更新告警状态。 */
void NetworkStatusMonitor::finishStreamProbe(bool streamOk, const QString &detail)
{
    if (checkState_ != CheckState::StreamProbing) {
        return;
    }

    cancelStreamProbe();
    checkState_ = CheckState::Idle;

    if (!isOnlineFeatureEnabled()) {
        clearWarning();
        return;
    }

    if (streamOk) {
        clearWarning();
    } else {
        setWarning(WarningKind::StreamNoData, detail);
    }
}

/** @brief 中止并释放当前 HTTP/TCP/UDP 探测对象。 */
void NetworkStatusMonitor::cancelStreamProbe()
{
    if (streamHttpReply_) {
        streamHttpReply_->abort();
        streamHttpReply_->deleteLater();
        streamHttpReply_ = nullptr;
    }
    if (streamHttpManager_) {
        streamHttpManager_->deleteLater();
        streamHttpManager_ = nullptr;
    }
    if (streamTcpSocket_) {
        streamTcpSocket_->abort();
        streamTcpSocket_->deleteLater();
        streamTcpSocket_ = nullptr;
    }
    if (streamUdpSocket_) {
        streamUdpSocket_->close();
        streamUdpSocket_->deleteLater();
        streamUdpSocket_ = nullptr;
    }
    streamProbeUrl_.clear();
}

/** @brief 从 RTSP 状态行解析三位状态码。 */
int NetworkStatusMonitor::parseRtspStatusCode(const QByteArray &statusLine) const
{
    const QList<QByteArray> parts = statusLine.trimmed().split(' ');
    if (parts.size() < 2) {
        return 0;
    }

    bool ok = false;
    const int code = parts.at(1).toInt(&ok);
    return ok ? code : 0;
}

/** @brief 从 RTSP 头解析 Content-Length。 */
int NetworkStatusMonitor::parseRtspContentLength(const QByteArray &header) const
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

/** @brief 从累计缓冲区解析完整 RTSP 响应及消费字节数。 */
NetworkStatusMonitor::RtspResponse NetworkStatusMonitor::parseRtspResponse(const QByteArray &buffer) const
{
    RtspResponse resp;

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
    resp.statusLine = QString::fromLatin1(statusLine);
    resp.body = buffer.mid(headerEnd + sepLen, contentLength);
    resp.consumedBytes = totalLen;
    return resp;
}

/** @brief 从 SDP 找到优先的视频 track control。 */
QString NetworkStatusMonitor::findRtspVideoControl(const QByteArray &sdpBody) const
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

        if (!inVideoBlock || !line.startsWith(QStringLiteral("a=control:"))) {
            continue;
        }

        const QString control = line.mid(QStringLiteral("a=control:").size()).trimmed();
        if (!control.isEmpty()) {
            return control;
        }
    }

    return QString();
}

/** @brief 将 SDP control 值解析为可请求的绝对 RTSP URL。 */
QString NetworkStatusMonitor::buildRtspControlUrl(const QUrl &baseUrl, const QString &control) const
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

/** @brief 从 SDP 媒体段提取视频组播端点。 */
NetworkStatusMonitor::RtspMulticastInfo NetworkStatusMonitor::findRtspMulticastInfo(const QByteArray &sdpBody) const
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

/** @brief 向套接字发送 RTSP DESCRIBE 请求。 */
void NetworkStatusMonitor::sendRtspDescribe(QTcpSocket *sock, const QString &url)
{
    if (!sock) {
        return;
    }

    QByteArray req;
    req += "DESCRIBE ";
    req += QUrl(url).toEncoded(QUrl::FullyEncoded);
    req += " RTSP/1.0\r\n";
    req += "CSeq: 1\r\n";
    req += "Accept: application/sdp\r\n";
    req += "User-Agent: qt_ycest-network-status-stream-probe\r\n";
    req += "\r\n";
    sock->write(req);
    sock->flush();
}

/** @brief 向套接字发送 RTSP SETUP 请求。 */
void NetworkStatusMonitor::sendRtspSetup(QTcpSocket *sock, const QString &trackUrl)
{
    if (!sock) {
        return;
    }

    QByteArray req;
    req += "SETUP ";
    req += QUrl(trackUrl).toEncoded(QUrl::FullyEncoded);
    req += " RTSP/1.0\r\n";
    req += "CSeq: 2\r\n";
    req += "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n";
    req += "User-Agent: qt_ycest-network-status-stream-probe\r\n";
    req += "\r\n";
    sock->write(req);
    sock->flush();
}

/** @return HTTP 状态码表示成功时返回 true。 */
bool NetworkStatusMonitor::isHttpSuccessStatus(int statusCode) const
{
    return statusCode == 200 || statusCode == 204 || statusCode == 206;
}

/** @return HTTP 状态码表示重定向时返回 true。 */
bool NetworkStatusMonitor::isHttpRedirectStatus(int statusCode) const
{
    return statusCode == 301 || statusCode == 302 || statusCode == 303 ||
           statusCode == 307 || statusCode == 308;
}

/** @return 响应正文看起来是 HTML 错误页时返回 true。 */
bool NetworkStatusMonitor::isProbablyHtmlBody(const QByteArray &body) const
{
    const QByteArray trimmed = body.left(512).trimmed().toLower();
    return trimmed.startsWith("<!doctype html") ||
           trimmed.startsWith("<html") ||
           trimmed.contains("<head") ||
           trimmed.contains("<body");
}

/** @brief 结合 URL、Content-Type 和正文特征判断是否为直播内容。 */
bool NetworkStatusMonitor::isLikelyLiveHttpBody(const QString &url,
                                                const QString &contentType,
                                                const QByteArray &body,
                                                QString *detail) const
{
    const QString lowerUrl = url.toLower();
    const QString ct = contentType.toLower();

    if (body.isEmpty()) {
        if (detail) {
            *detail = QStringLiteral("HTTP 无媒体数据");
        }
        return false;
    }

    if (isProbablyHtmlBody(body)) {
        if (detail) {
            *detail = QStringLiteral("HTTP 返回网页而不是媒体流");
        }
        return false;
    }

    if (ct.contains(QStringLiteral("mpegurl")) || ct.contains(QStringLiteral("mpegurl")) ||
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

/** @brief 设置告警类型、文案及闪现定时器。 */
void NetworkStatusMonitor::setWarning(WarningKind kind, const QString &detail)
{
    updateWarningText(kind, detail);
    warningKind_ = kind;
    warningActive_ = true;

    // 第一次进入告警或告警类型变化时立即提示一次，之后每1分钟提示一次。
    if (!blinkTimer_.isActive()) {
        showWarningNow();
        blinkTimer_.start();
    } else if (warningVisible_) {
        resizeToFitText();
        moveBoxToScreenCenter();
        update();
    }
}

/** @brief 清除告警并隐藏控件。 */
void NetworkStatusMonitor::clearWarning()
{
    warningKind_ = WarningKind::None;
    warningActive_ = false;
    warningVisible_ = false;
    blinkTimer_.stop();
    autoHideTimer_.stop();
    hide();
}

/** @brief 按故障类型生成用户可读文案。 */
void NetworkStatusMonitor::updateWarningText(WarningKind kind, const QString &detail)
{
    if (kind == WarningKind::LinkDown) {
        text_ = QStringLiteral("网络未连接！请检查网线或网络设备");
        return;
    }

    if (kind == WarningKind::NoInternet) {
        if (detail.isEmpty()) {
            text_ = QStringLiteral("网络异常！无法访问互联网");
        } else {
            text_ = QStringLiteral("网络异常！无法访问互联网，%1").arg(detail);
        }
        return;
    }

    if (kind == WarningKind::StreamNoData) {
        if (detail.isEmpty()) {
            text_ = QStringLiteral("直播源异常！未检测到直播流数据");
        } else {
            text_ = QStringLiteral("直播源异常！未检测到直播流数据，%1").arg(detail);
        }
        return;
    }

    text_.clear();
}

/** @brief 告警持续存在时周期显示提示框。 */
void NetworkStatusMonitor::onBlinkTimeout()
{
    if (!warningActive_) {
        hideWarningNow();
        blinkTimer_.stop();
        return;
    }

    showWarningNow();
}

/** @brief 显示一次告警并安排自动隐藏。 */
void NetworkStatusMonitor::showWarningNow()
{
    if (main_) {
        setGeometry(main_->rect());
    }

    resizeToFitText();
    moveBoxToScreenCenter();

    raise();
    show();
    update();

    warningVisible_ = true;

    autoHideTimer_.stop();
    autoHideTimer_.start(visibleDurationMs_);
}

/** @brief 隐藏告警但保留 active 状态供下次闪现。 */
void NetworkStatusMonitor::hideWarningNow()
{
    autoHideTimer_.stop();
    warningVisible_ = false;
    hide();
}

/** @brief 主窗口尺寸变化时重新定位告警框。 */
bool NetworkStatusMonitor::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == main_) {
        if (event->type() == QEvent::Resize ||
            event->type() == QEvent::Move ||
            event->type() == QEvent::WindowStateChange) {
            if (main_) {
                setGeometry(main_->rect());
            }
            moveBoxToScreenCenter();
            update();
        }
    }

    return QWidget::eventFilter(watched, event);
}

/** @brief 按当前文字和宽度上限调整告警框尺寸。 */
void NetworkStatusMonitor::resizeToFitText()
{
    // 样式对齐 UdiskStatusOverlay：底部横向黑色半透明提示条，白字，圆角。
    QFont f = baseFont_;
    f.setPixelSize(22);
    f.setWeight(QFont::Normal);
    setFont(f);

    QFontMetrics fm(f);

    const int margin = 14;
    const int minH = 86;
    const int parentW = main_ ? main_->width() : width();
    const int parentH = main_ ? main_->height() : height();

    int newW = parentW - margin * 2;
    if (newW < 200) {
        newW = 200;
    }

    QRect br = fm.boundingRect(QRect(0, 0, newW - padX_ * 2, 1000),
                               Qt::TextWordWrap,
                               text_);

    int newH = br.height() + padY_ * 2;
    if (newH < minH) {
        newH = minH;
    }

    // 防止三行以上的错误详情把底部提示条撑得过高。
    const int maxH = qMax(minH, parentH / 3);
    if (newH > maxH) {
        newH = maxH;
    }

    boxRect_.setSize(QSize(newW, newH));
}

/** @brief 将告警框移动到主窗口可视区域中央。 */
void NetworkStatusMonitor::moveBoxToScreenCenter()
{
    // 保留旧函数名，实际改为底部布局，避免扩大头文件和调用点修改范围。
    if (!main_) {
        return;
    }

    const int margin = 14;

    QRect r = boxRect_;
    if (r.width() <= 0) {
        r.setWidth(qMax(200, main_->width() - margin * 2));
    }
    if (r.height() <= 0) {
        r.setHeight(86);
    }

    r.moveLeft(margin);
    r.moveTop(qMax(margin, main_->height() - margin - r.height()));
    boxRect_ = r;
}

/** @brief 绘制居中的半透明告警框和文字。 */
void NetworkStatusMonitor::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    if (text_.isEmpty() || boxRect_.isEmpty()) {
        return;
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal radius = 10.0;

    // 与 UdiskStatusOverlay 保持一致：黑底半透明 + 白字 + 圆角。
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 160));
    p.drawRoundedRect(QRectF(boxRect_), radius, radius);

    p.setPen(QColor(255, 255, 255, 245));
    p.drawText(boxRect_.adjusted(padX_, padY_, -padX_, -padY_),
               Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap,
               text_);
}
