/**
 * @file network_status_monitor.cpp
 * @brief 网络链路、外网和直播源可用性的分阶段监测告警控件的实现。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-13；修改人：Du lin；播放器与告警的直播源探测在现有网络线程共用 HTTP 管理器；配置读取、超时和清理在后台，UI 仅应用有效结果。
 */

#include "network_status_monitor.h"
#include <utility>

#include <QApplication>
#include <QProcess>
#include <QThread>
#include <atomic>
#include <mutex>
#include <functional>
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
#include <QHostAddress>
#include <QDateTime>
#include "ic_board/ic_event_bridge.h"
#include "platform/rk3566_platform.h"

/** @brief 后台检测只通过短锁门向仍存活的UI投递，不等待UI或持有控件引用。 */
struct NetworkCheckGuard {
    std::mutex mutex;QObject *target=nullptr; /**< mutex仅保护投递地址，不覆盖文件或进程操作。 */
    std::atomic<quint64> generation{0}; /**< 告警检查代次，stop 或新轮次使旧结果无效。 */
    std::atomic<quint64> playbackGeneration{0}; /**< 播放器独立代次；取消告警不会影响它。 */
};
/** @brief 网络事件线程独占的路由/公网探测器，不访问控件。
 * @details QProcess与超时定时器是本对象子对象，随moveToThread迁移；配置和同步路由回退也只在此线程执行。
 * 新请求只保留最新代次，先异步终止旧ping再启动；stop通过共享原子代次立即阻止旧结果回到UI。
 * 直播探测作为此对象的子对象，复用 HTTP 管理器；独立代次控制告警和播放请求。
 * 最终关闭先取消直播探测并 kill ping，子进程退出后才 quit；UI 不等待。
 */
class NetworkProbeWorker final:public QObject {
public:
    /** @brief 创建可迁移的进程/定时器子对象；实际启动等待request进入工作线程。
     * @param[in] guard UI生命周期及取消代次，不持有控件所有权。
     * @param[in] path 原配置路径，只在工作线程读取。
     * @param[in] timeoutMs 原每目标超时毫秒。
     */
    NetworkProbeWorker(std::shared_ptr<NetworkCheckGuard> guard,QString path,int timeoutMs)
        :guard_(std::move(guard)),path_(std::move(path)),timeoutMs_(timeoutMs),process_(new QProcess(this)),timeout_(new QTimer(this)), liveProbe_(new LiveStreamProbe(this)) {
        timeout_->setSingleShot(true);
        connect(timeout_,&QTimer::timeout,this,[this] {
            if(!current()) return;
            if(process_->state()!=QProcess::NotRunning) process_->kill();else advance();
        });
        connect(process_,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[this](int code,QProcess::ExitStatus status) {
            timeout_->stop();
            if(closing_) { thread()->quit();return; }
            if(!current()) { startPending();return; }
            if(status==QProcess::NormalExit && code==0) complete(true);else advance();
        });
        connect(process_,&QProcess::started,this,[this] { if(!current()) process_->kill(); });
        connect(process_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError) {
            if(closing_ && process_->state()==QProcess::NotRunning) thread()->quit();
        });
    }
    /** @brief 工作线程接受最新检查；运行中的旧子进程退出前不复用QProcess。
     * @param[in] generation UI提交时保存的共享代次，已过期则忽略。
     */
    void request(quint64 generation) {
        if(closing_ || guard_->generation.load()!=generation) return;
        pending_=generation;active_=0;timeout_->stop();
        if(process_->state()!=QProcess::NotRunning) process_->kill();else startPending();
    }
    /** @brief 启动播放器直播探测，在网络线程完成后向 UI 排队。
     * @param[in] generation 播放器提交时的独立代次。
     * @param[in] url 直播源 URL。
     * @param[in] kind 业务协议类型。
     * @param[in] completion 仅 UI 执行的值结果回调。
     */
    void requestPlaybackProbe(quint64 generation, const QString &url, const QString &kind,
                              LiveStreamProbe::Completion completion)
    {
        if (closing_ || guard_->playbackGeneration.load() != generation) return;
        liveProbe_->start(LiveStreamProbe::Purpose::PlaybackRetry, url, kind,
                          [this, generation, completion=std::move(completion)](LiveStreamProbe::Result result) {
            publish(generation, [completion, result=std::move(result)](NetworkStatusMonitor *) {
                if (completion) completion(result);
            }, true);
        });
    }

    /** @brief 取消播放器的探测；告警请求与管理器继续使用。
     * @param[in] generation UI 发起取消时递增后的播放代次。
     */
    void cancelPlaybackProbe(quint64 generation)
    {
        if (guard_->playbackGeneration.load() == generation)
            liveProbe_->cancel(LiveStreamProbe::Purpose::PlaybackRetry);
    }

    /** @brief 外网检查通过后，在网络线程读取直播目标并按原告警规则检查。
     * @param[in] generation 当前告警轮次，过期时跳过请求或结果。
     */
    void requestWarningProbe(quint64 generation)
    {
        if (closing_ || guard_->generation.load() != generation) return;

        QSettings ini(path_, QSettings::IniFormat);
        ini.setIniCodec("UTF-8");
        ini.beginGroup(QStringLiteral("playback"));
        const bool commandReceived = ini.value(QStringLiteral("command_received"), false).toBool();
        const auto target = ini.value(QStringLiteral("target_mode")).toString().trimmed().toUpper();
        const auto current = ini.value(QStringLiteral("current_mode")).toString().trimmed().toUpper();
        const bool fallback = ini.value(QStringLiteral("fallback_from_live"), false).toBool();
        auto url = ini.value(QStringLiteral("live_url")).toString().trimmed();
        const auto kind = ini.value(QStringLiteral("live_kind")).toString();
        ini.endGroup();

        if (!commandReceived || target != QStringLiteral("LIVE") ||
            (current == QStringLiteral("LIVE") && !fallback)) {
            publish(generation, [](NetworkStatusMonitor *ui) { ui->finishStreamProbe(true, {}); });
            return;
        }
        if (url.isEmpty()) url = ini.value(QStringLiteral("stream/url")).toString().trimmed();
        if (url.isEmpty()) {
            publish(generation, [](NetworkStatusMonitor *ui) {
                ui->finishStreamProbe(false, QStringLiteral("未配置直播地址"));
            });
            return;
        }
        liveProbe_->start(LiveStreamProbe::Purpose::NetworkWarning, url, kind,
                          [this, generation](LiveStreamProbe::Result result) {
            const bool enabled = onlineEnabled();
            publish(generation, [enabled, result=std::move(result)](NetworkStatusMonitor *ui) {
                ui->finishStreamProbe(!enabled || result.available, result.detail);
            });
        });
    }

    /** @brief 工作线程取消告警探测，不做同步等待。
     * @param[in] closing true为最终释放，进程退出后终止所属事件循环；false保留线程供重启。
     */
    void cancel(bool closing=false) {
        closing_|=closing;pending_=active_=0;timeout_->stop();
        liveProbe_->cancel(LiveStreamProbe::Purpose::NetworkWarning);
        if (closing_) liveProbe_->cancel(LiveStreamProbe::Purpose::PlaybackRetry);
        if(process_->state()!=QProcess::NotRunning) process_->kill();else if(closing_) thread()->quit();
    }
private:
    /** @return 当前目标仍属于有效轮次，关闭或UI取消立即返回false。 */
    bool current() const { return !closing_ && active_ && active_==guard_->generation.load(); }
    /** @brief 将值结果安全投递UI；短锁只覆盖QObject地址校验与排队，不执行探测。
     * @param[in] generation 结果所属轮次。
     * @param[in] action UI收到后应用结果，不能携带后台可变对象引用。
     * @param[in] playback true 使用播放代次，否则使用告警检查代次。
     */
    void publish(quint64 generation, std::function<void(NetworkStatusMonitor*)> action,
                 bool playback=false)
    {
        const auto guard=guard_;
        std::lock_guard<std::mutex> lock(guard->mutex);
        QObject *target=guard->target;
        auto &counter = playback ? guard->playbackGeneration : guard->generation;
        if (!target || counter.load() != generation) return;
        QMetaObject::invokeMethod(target, [guard,generation,target,playback,action=std::move(action)] {
            const auto &counter = playback ? guard->playbackGeneration : guard->generation;
            if (counter.load() == generation) action(static_cast<NetworkStatusMonitor*>(target));
        }, Qt::QueuedConnection);
    }
    /** @brief 后台读取在线开关，保持offline优先规则，不触及UI QSettings。 */
    bool onlineEnabled() const {
        QSettings ini(path_,QSettings::IniFormat);
        return !ini.value("feature/offline_v1",false).toBool() &&
            (ini.value("feature/online_v1",false).toBool() || ini.value("feature/online_v2",false).toBool());
    }
    /** @brief 启动待处理最新轮次；原路由等待留在此线程，取消通过代次而非等待UI实现。 */
    void startPending() {
        if(closing_ || !pending_) return;
        active_=pending_;pending_=0;if(!current()) { active_=0;return; }
        const auto generation=active_;
        const bool enabled=onlineEnabled();bool carrierValid=false;
        const bool linkUp=enabled ? NetworkStatusMonitor::isWiredLinkUp(&carrierValid) : true;
        if(!current()) return;
        const bool route=enabled && (!carrierValid || linkUp) ? NetworkStatusMonitor::hasDefaultRoute() : false;
        if(!current()) return;
        publish(generation,[enabled,carrierValid,linkUp,route](NetworkStatusMonitor *ui) {
            ui->checkState_=NetworkStatusMonitor::CheckState::Idle;
            if(!enabled) { IcEventBridge::instance()->updateNetworkAvailability(true);ui->clearWarning();return; }
            if(carrierValid && !linkUp) { IcEventBridge::instance()->updateNetworkAvailability(false);ui->setWarning(NetworkStatusMonitor::WarningKind::LinkDown);return; }
            ui->lastDefaultRouteOk_=route;ui->checkState_=NetworkStatusMonitor::CheckState::Pinging;
        });
        if(!enabled || (carrierValid && !linkUp)) { active_=0;return; }
        targets_=QStringList()<<QStringLiteral("223.5.5.5")<<QStringLiteral("8.8.8.8")<<QStringLiteral("119.29.29.29");
        QSettings ini(path_,QSettings::IniFormat);ini.setIniCodec("UTF-8");
        const auto dns=ini.value("network/dns").toString().trimmed();QHostAddress address;
        if(address.setAddress(dns) && address.protocol()==QAbstractSocket::IPv4Protocol && !targets_.contains(dns)) targets_<<dns;
        index_=0;startTarget();
    }
    /** @brief 工作线程启动原ping参数并启用原超时；FailedToStart仍由原超时策略推进。 */
    void startTarget() {
        if(!current()) return;
        if(index_>=targets_.size()) { complete(false);return; }
        process_->start(QStringLiteral("ping"),QStringList()<<QStringLiteral("-c")<<QStringLiteral("1")<<QStringLiteral("-W")<<QStringLiteral("2")<<targets_.at(index_));
        timeout_->start(timeoutMs_);
    }
    /** @brief 当前目标失败/超时后尝试下一个；仅工作线程的finished或timeout调用。 */
    void advance() { if(current()) { ++index_;startTarget(); } }
    /** @brief 发布最终公网结果；再次后台读取在线开关，停用配置仍清除原告警。
     * @param[in] ok 当前轮次是否有成功目标。
     */
    void complete(bool ok) {
        const auto generation=active_;timeout_->stop();const bool enabled=onlineEnabled();
        publish(generation,[ok,enabled](NetworkStatusMonitor *ui) {
            if(ui->checkState_==NetworkStatusMonitor::CheckState::Pinging) ui->finishInternetProbe(ok,enabled);
        });active_=0;
    }
    std::shared_ptr<NetworkCheckGuard> guard_;QString path_;int timeoutMs_; /**< 借用取消门、自有配置路径与目标超时毫秒。 */
    QProcess *process_;QTimer *timeout_;QStringList targets_;int index_=0; /**< 工作线程独占，QObject持有子对象；原目标顺序不变。 */
    LiveStreamProbe *liveProbe_; /**< 网络线程内拥有两路直播探测及共享管理器的 QObject 子对象。 */
    quint64 active_=0,pending_=0;bool closing_=false; /**< 活动/最新待处理代次，0无任务；最终关闭后拒绝新请求。 */
};

/** @brief 创建UI告警定时器与独立网络事件线程；进程/超时随worker归后台线程所有。
 * @param[in] mainWindow 借用并作为父对象的主窗口，可为空，沿用原覆盖层行为。
 * @param[in] configPath 原网络配置路径；为空时使用平台netConfigPath。
 */
NetworkStatusMonitor::NetworkStatusMonitor(QWidget *mainWindow,
                                           const QString &configPath)
    : QWidget(mainWindow),
      main_(mainWindow)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);

    networkGuard_=std::make_shared<NetworkCheckGuard>();networkGuard_->target=this;
    baseFont_ = QApplication::font();

    if (main_) {
        main_->installEventFilter(this);
        setGeometry(main_->rect());
    }

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

    networkThread_=new QThread;
    networkThread_->setObjectName(QStringLiteral("network.probe"));
    const auto path = configPath.isEmpty() ? Rk3566Platform::netConfigPath() : configPath;
    networkWorker_=new NetworkProbeWorker(networkGuard_,path,pingTimeoutMs_);
    networkWorker_->moveToThread(networkThread_);
    connect(networkThread_,&QThread::finished,networkWorker_,&QObject::deleteLater);
    connect(networkThread_,&QThread::finished,networkThread_,&QObject::deleteLater);
    networkThread_->start();

    hide();
}

/** @brief 关闭 UI 投递门，异步取消两路直播探测和 ping 后回收网络线程。 */
NetworkStatusMonitor::~NetworkStatusMonitor()
{
    stop();
    networkGuard_->playbackGeneration.fetch_add(1);
    { std::lock_guard<std::mutex> lock(networkGuard_->mutex);networkGuard_->target=nullptr; }
    auto *worker=networkWorker_;
    QMetaObject::invokeMethod(worker,[worker] { worker->cancel(true); },Qt::QueuedConnection);
}

/** @brief 启动周期检查并立即执行首轮检测。 */
void NetworkStatusMonitor::start()
{
    checkNetworkStatus();
    checkTimer_.start();
}

/** @brief 停止告警定时器并异步取消告警探测；播放重试独立取消。 */
void NetworkStatusMonitor::stop()
{
    networkGuard_->generation.fetch_add(1);
    checkTimer_.stop();
    blinkTimer_.stop();
    autoHideTimer_.stop();
    auto *worker=networkWorker_;
    QMetaObject::invokeMethod(worker,[worker] { worker->cancel(); },Qt::QueuedConnection);

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

/** @brief 读取有线网卡 carrier；valid 表示系统节点是否可读。 */
bool NetworkStatusMonitor::isWiredLinkUp(bool *valid)
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
bool NetworkStatusMonitor::hasDefaultRoute()
{
    // /proc/net/route 是最轻量的判断方式，但不同 BusyBox/内核裁剪环境下，
    // 路由信息可能显示不完整；所以这里只作为“参考状态”，不能在 ping 前直接判死。
    QFile f(QStringLiteral("/proc/net/route"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // proc文件常报告size=0，不能在首次读取前用atEnd判空。
        const auto contents=f.readAll();
        for (const auto &bytes:contents.split('\n')) {
            const QString line = QString::fromLatin1(bytes).trimmed();
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

/** @brief UI仅提交非重入检查代次；配置、路由与ping均由网络事件线程处理。 */
void NetworkStatusMonitor::checkNetworkStatus()
{
    if(checkState_!=CheckState::Idle) return;
    checkState_=CheckState::CheckingRoute;
    const auto generation=networkGuard_->generation.fetch_add(1)+1;
    auto *worker=networkWorker_;
    QMetaObject::invokeMethod(worker,[worker,generation] { worker->request(generation); },Qt::QueuedConnection);
}

/** @brief 在 UI 应用外网结果；外网可用时排队提交直播源告警检查。 */
void NetworkStatusMonitor::finishInternetProbe(bool internetOk,bool onlineEnabled)
{
    checkState_ = CheckState::Idle;

    if (!onlineEnabled) {
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
    checkState_ = CheckState::StreamProbing;
    const auto generation = networkGuard_->generation.load();
    auto *worker = networkWorker_;
    QMetaObject::invokeMethod(worker, [worker, generation] { worker->requestWarningProbe(generation); },
                              Qt::QueuedConnection);
}

/** @brief 接收网络线程返回的告警结果并更新覆盖层。
 * @param[in] streamOk true 清除直播源告警。
 * @param[in] detail 失败时的直播源告警说明。
 */
void NetworkStatusMonitor::finishStreamProbe(bool streamOk, const QString &detail)
{
    if (checkState_ != CheckState::StreamProbing) return;
    checkState_ = CheckState::Idle;
    if (streamOk) clearWarning();
    else setWarning(WarningKind::StreamNoData, detail);
}

void NetworkStatusMonitor::requestPlaybackProbe(const QString &url, const QString &kind,
                                               LiveStreamProbe::Completion completion)
{
    const auto generation = networkGuard_->playbackGeneration.fetch_add(1) + 1;
    auto *worker = networkWorker_;
    QMetaObject::invokeMethod(worker, [worker, generation, url, kind, completion=std::move(completion)] {
        worker->requestPlaybackProbe(generation, url, kind, completion);
    }, Qt::QueuedConnection);
}

void NetworkStatusMonitor::cancelPlaybackProbe()
{
    const auto generation = networkGuard_->playbackGeneration.fetch_add(1) + 1;
    auto *worker = networkWorker_;
    QMetaObject::invokeMethod(worker, [worker, generation] { worker->cancelPlaybackProbe(generation); },
                              Qt::QueuedConnection);
}


/** @brief 设置告警类型、文案及闪现定时器。 */
void NetworkStatusMonitor::setWarning(WarningKind kind, const QString &detail)
{
    const bool warningChanged = !warningActive_ || warningKind_ != kind;
    const QString previousText = text_;
    updateWarningText(kind, detail);
    warningKind_ = kind;
    warningActive_ = true;

    // 网络故障只记日志；从直播源告警切换过来时，也立即移除已有提示条。
    if ((kind == WarningKind::LinkDown || kind == WarningKind::NoInternet) &&
        blinkTimer_.isActive()) {
        hideWarningNow();
        if (warningChanged || previousText != text_) {
            showWarningNow();
        }
        return;
    }

    // 首次告警立即处理，之后每1分钟处理一次；直播源告警保留原有显示方式。
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

/** @brief 网络故障记录日志；其他告警显示一次并安排自动隐藏。 */
void NetworkStatusMonitor::showWarningNow()
{
    if (warningKind_ == WarningKind::LinkDown || warningKind_ == WarningKind::NoInternet) {
        qWarning().noquote() << "[网络告警]" << text_;
        hideWarningNow();
        return;
    }

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
