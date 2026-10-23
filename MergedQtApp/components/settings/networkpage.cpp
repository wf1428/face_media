/**
 * @file networkpage.cpp
 * @brief 有线网络 DHCP/静态地址配置与自动恢复页面的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "networkpage.h"
#include "ui_networkpage.h"

#include "components/dialogs/ui_message_box.h"
#include "common/network_config.h"
#include <QProcess>
#include <QHostAddress>
#include <QAbstractSocket>
#include <QFile>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QDebug>
#include <QTextStream>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QRegExp>
#include <QThread>
#include <QSignalBlocker>

#include <signal.h>
#include <unistd.h>

#include "platform/rk3566_platform.h"


// 判断字符串是否是合法 IPv4
/**
 * @brief 判断文本是否为有效 IPv4 地址。
 * @param s 待规范化或验证的文本。
 * @return 文本是合法 IPv4 时为 true。
 */
static bool isIPv4(const QString &s)
{
    QHostAddress addr;
    return addr.setAddress(s) && addr.protocol() == QAbstractSocket::IPv4Protocol;
}

// 过滤 -
/**
 * @brief 清理网络字段中的空白及无效占位文本。
 * @param s 待规范化或验证的文本。
 * @return 去除无效占位值并规范化后的网络文本。
 */
static QString normalizeNetValue(const QString& s)
{
    const QString v = s.trimmed();
    return (v.isEmpty() || v == "-") ? QString() : v;
}

//判断链路状态
/**
 * @brief 读取内核网卡载波状态。
 * @param ifname 目标网卡名称。
 * @return 内核报告网卡载波已连接时为 true。
 */
static bool isIfaceCarrierUp(const QString& ifname)
{
    QFile f(QString("/sys/class/net/%1/carrier").arg(ifname));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    const QString v = QString::fromUtf8(f.readAll()).trimmed();
    return (v == "1");
}


// 将 255.255.255.0 转为 24；若掩码不合法返回 -1
/**
 * @brief 将连续的 IPv4 子网掩码转换为前缀位数。
 * @param mask 点分十进制 IPv4 子网掩码。
 * @return 连续子网掩码对应的前缀位数，无效时返回 -1。
 */
static int netmaskToPrefix(const QString &mask)
{
    // IPv4 合法性判断
    QHostAddress addr;
    if (!addr.setAddress(mask) || addr.protocol() != QAbstractSocket::IPv4Protocol)
        return -1;

    // 不直接依赖 toIPv4Address() 的主机字节序，改用字符串分段法（更稳）
    const auto parts = mask.split('.');
    if (parts.size() != 4) return -1;

    int prefix = 0;
    bool zeroSeen = false;

    for (const QString &p : parts) {
        bool ok = false;
        int byte = p.toInt(&ok);
        if (!ok || byte < 0 || byte > 255) return -1;

        // 逐 bit (从高位到低位) 检查连续性
        for (int i = 7; i >= 0; --i) {
            bool bit = (byte >> i) & 1;
            if (bit) {
                if (zeroSeen) return -1; // 出现过 0 之后又出现 1 => 非连续掩码
                prefix++;
            } else {
                zeroSeen = true;
            }
        }
    }
    return prefix;
}


//同步执行外部命令，带超时
/**
 * @brief 限时运行固定系统命令并收集错误说明。
 * @param program 固定系统命令的程序路径。
 * @param args 操作对应的结构化参数。
 * @param[out] errOut 可选输出，接收命令或网络操作错误。
 * @param timeoutMs 命令执行超时，单位为毫秒。
 * @return 程序正常结束且退出码为 0 时为 true，失败或超时为 false。
 */
static bool runCmd(const QString &program, const QStringList &args, QString *errOut = nullptr, int timeoutMs = 5000)
{
    const QString executable = Rk3566Platform::findExecutable({program});
    if (executable.isEmpty()) {
        if (errOut) *errOut = QStringLiteral("命令不存在: %1").arg(program);
        return false;
    }

    QProcess process;
    process.start(executable, args);
    if (!process.waitForStarted(1000)) {
        if (errOut) *errOut = QStringLiteral("命令启动失败: %1").arg(executable);
        return false;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(200);
        if (errOut) *errOut = QStringLiteral("命令超时: %1").arg(executable);
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errOut) {
            const QString stderrText = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            const QString stdoutText = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
            *errOut = stderrText.isEmpty() ? stdoutText : stderrText;
        }
        return false;
    }
    return true;
}


// 读出 iface 的 IPv4 和 netmask
/**
 * @brief 读取指定网卡的 IPv4 地址及掩码。
 * @param ifname 目标网卡名称。
 * @param[out] ipOut 接收网卡 IPv4 地址。
 * @param[out] maskOut 接收网卡子网掩码。
 */
static void getIfaceIPv4AndMask(const QString& ifname, QString* ipOut, QString* maskOut)
{
    if (ipOut) ipOut->clear();
    if (maskOut) maskOut->clear();

    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        if (iface.humanReadableName() != ifname && iface.name() != ifname)
            continue;

        for (const auto& e : iface.addressEntries()) {
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                if (ipOut) *ipOut = e.ip().toString();
                if (maskOut) *maskOut = e.netmask().toString();
                return;
            }
        }
    }
}

// 从 `ip route show default dev eth0` 里解析网关
/**
 * @brief 查询指定网卡的实际默认网关。
 * @param ifname 目标网卡名称。
 * @return 目标网卡默认网关；未查到时为空。
 */
static QString getDefaultGateway(const QString& ifname)
{
    const QString ipTool = Rk3566Platform::findExecutable({QStringLiteral("ip")});
    if (ipTool.isEmpty()) return {};
    QProcess p;
    p.start(ipTool, {"route", "show", "default", "dev", ifname});
    if (!p.waitForFinished(2000)) return {};

    const QString out = QString::fromLocal8Bit(p.readAllStandardOutput());
    // 典型：default via 192.168.1.1 dev eth0
    QRegularExpression re(R"(default\s+via\s+(\d+\.\d+\.\d+\.\d+))");
    auto m = re.match(out);
    return m.hasMatch() ? m.captured(1) : QString();
}

// 读取 resolv.conf 的 nameserver
/**
 * @brief 读取系统解析器中的 DNS 地址。
 * @return 当前解析器配置的 DNS 地址文本。
 */
static QString getDnsFromResolvConf()
{
    QFile f("/etc/resolv.conf");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};

    QStringList dns;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith("nameserver")) {
            const auto parts = line.split(QRegExp("\\s+"), QString::SkipEmptyParts);
            if (parts.size() >= 2) dns << parts[1];
        }
    }
    return dns.join(" ");
}


/** @brief 工作线程采集的值对象，不包含任何界面对象。 */
struct NetworkRuntimeState
{
    QString ip; /**< 网卡当前 IPv4 地址，未取得时为空。 */
    QString mask; /**< 当前 IPv4 子网掩码。 */
    QString gateway; /**< 通过后台命令查询的默认网关。 */
    QString dns; /**< 从解析器配置采集的 DNS 文本。 */
    bool carrierUp = false; /**< 内核载波状态。 */
    bool dhcpRunning = false; /**< 采样时 DHCP 客户端是否存活。 */
};


/**
 * @brief 构造指定网卡的 DHCP 客户端 PID 文件路径。
 * @param iface 目标有线网卡名称。
 * @return 指定网卡 udhcpc 进程的 PID 文件路径。
 */
static QString dhcpPidFile(const QString& iface)
{
    const QString runDir = QStringLiteral("/run/qt_ycest");
    if (QDir().mkpath(runDir)) {
        return QDir(runDir).filePath(QStringLiteral("dhcp.%1.pid").arg(iface));
    }
    return QDir(QDir::tempPath()).filePath(QStringLiteral("qt_ycest.dhcp.%1.pid").arg(iface));
}


/**
 * @brief 检查指定网卡的 DHCP 客户端进程是否存活。
 * @param iface 目标有线网卡名称。
 * @return PID 文件对应的 DHCP 客户端仍存活时返回 true。
 */
static bool isDhcpRunning(const QString& iface)
{
    QFile f(dhcpPidFile(iface));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    const QByteArray pid = f.readAll().trimmed();
    if (pid.isEmpty())
        return false;

    bool ok = false;
    const qint64 value = QString::fromLatin1(pid).toLongLong(&ok);
    return ok && value > 1 && ::kill(static_cast<pid_t>(value), 0) == 0;
}


/**
 * @brief 终止指定网卡的 DHCP 客户端并清理 PID 文件。
 * @param iface 目标有线网卡名称。
 * @param[out] errOut 可选输出，接收命令或网络操作错误。
 * @return 停止操作成功时为 true，否则为 false。
 */
static bool stopDhcpClient(const QString& iface, QString* errOut = nullptr)
{
    const QString pidFile = dhcpPidFile(iface);
    QFile file(pidFile);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return true;
    }

    bool ok = false;
    const qint64 value = QString::fromLatin1(file.readAll().trimmed()).toLongLong(&ok);
    file.close();
    QFile::remove(pidFile);

    if (!ok || value <= 1) {
        if (errOut) *errOut = QStringLiteral("DHCP pidfile 内容无效: %1").arg(pidFile);
        return false;
    }

    const pid_t pid = static_cast<pid_t>(value);
    if (::kill(pid, 0) != 0) return true;

    ::kill(pid, SIGTERM);
    for (int i = 0; i < 10 && ::kill(pid, 0) == 0; ++i) {
        QThread::msleep(100);
    }
    if (::kill(pid, 0) == 0) {
        ::kill(pid, SIGKILL);
    }
    return true;
}

/**
 * @brief 按既有候选路径查找可用的 DHCP 配置脚本。
 * @return 系统中第一个可用的 udhcpc 事件脚本。
 */
static QString findDhcpScript()
{
    const QStringList candidates = {
        QStringLiteral("/usr/share/udhcpc/default.script"),
        QStringLiteral("/etc/udhcpc/default.script"),
        QStringLiteral("/usr/lib/udhcpc/default.script")
    };
    for (const QString &path : candidates) {
        if (QFileInfo(path).isExecutable() || QFileInfo::exists(path)) return path;
    }
    return QString();
}

/**
 * @brief 以后台守护方式为指定网卡启动 udhcpc。
 * @param iface 目标有线网卡名称。
 * @param[out] errOut 可选输出，接收命令或网络操作错误。
 * @return 客户端成功启动时为 true，否则为 false。
 */
static bool startDhcpDaemon(const QString& iface, QString* errOut = nullptr)
{
    if (isDhcpRunning(iface)) return true;

    const QString pidFile = dhcpPidFile(iface);
    const QString udhcpc = Rk3566Platform::findExecutable({QStringLiteral("udhcpc")});
    if (!udhcpc.isEmpty()) {
        QStringList args{QStringLiteral("-i"), iface,
                         QStringLiteral("-b"),
                         QStringLiteral("-t"), QStringLiteral("5"),
                         QStringLiteral("-T"), QStringLiteral("3"),
                         QStringLiteral("-p"), pidFile};
        const QString script = findDhcpScript();
        if (!script.isEmpty()) args << QStringLiteral("-s") << script;

        const bool started = QProcess::startDetached(udhcpc, args);
        if (!started && errOut) *errOut = QStringLiteral("启动 udhcpc 失败");
        return started;
    }

    // Ubuntu/Debian RK3566 根文件系统通常提供 ISC dhclient。
    const QString dhclient = Rk3566Platform::findExecutable({QStringLiteral("dhclient")});
    if (!dhclient.isEmpty()) {
        const QString leaseFile = QDir(Rk3566Platform::configDir())
                .filePath(QStringLiteral("dhclient.%1.leases").arg(iface));
        const QStringList args{QStringLiteral("-4"), QStringLiteral("-nw"),
                               QStringLiteral("-pf"), pidFile,
                               QStringLiteral("-lf"), leaseFile,
                               iface};
        const bool started = QProcess::startDetached(dhclient, args);
        if (!started && errOut) *errOut = QStringLiteral("启动 dhclient 失败");
        return started;
    }

    if (errOut) *errOut = QStringLiteral("系统未安装 udhcpc 或 dhclient");
    return false;
}


/**
 * @brief 清除接口旧地址和路由，为 DHCP 或静态配置重新应用做准备。
 * @param iface 目标有线网卡名称。
 * @param[out] errOut 可选输出，接收命令或网络操作错误。
 * @return 地址及路由清理成功时为 true。
 */
static bool clearIfaceNetworkConfig(const QString& iface, QString* errOut = nullptr)
{
    QString err;

    // 停 DHCP，避免又把地址拉回来
    stopDhcpClient(iface, nullptr);

    // 清理 IP 和路由
    if (!runCmd("ip", {"addr", "flush", "dev", iface}, &err)) {
        if (errOut) *errOut = "清理IP失败: " + err;
        return false;
    }

    if (!runCmd("ip", {"route", "flush", "dev", iface}, &err)) {
        if (errOut) *errOut = "清理路由失败: " + err;
        return false;
    }

    return true;
}

/**
 * @brief 仅在网络工作线程调用；断线轮询不启动网关查询进程。
 * @param iface 目标有线网卡名称。
 * @param requireCarrier 为 true 且网线断开时直接返回，跳过网关等进程查询。
 * @return 系统实际网络状态；要求载波且断线时仅返回载波状态。
 */
static NetworkRuntimeState queryNetworkRuntimeState(const QString &iface, bool requireCarrier)
{
    NetworkRuntimeState state;
    state.carrierUp = isIfaceCarrierUp(iface);
    if (requireCarrier && !state.carrierUp) return state;
    getIfaceIPv4AndMask(iface, &state.ip, &state.mask);
    state.gateway = getDefaultGateway(iface);
    state.dns = getDnsFromResolvConf();
    state.dhcpRunning = isDhcpRunning(iface);
    return state;
}

/**
 * @brief 后台串行运行系统调用，结果和界面操作回到 GUI 线程执行。
 * @param work 在专用网络线程串行执行的工作，不能直接刷新控件。
 * @param completed 结果回到 GUI 线程后的完成回调。
 * @note 同一时刻只允许一个后台网络任务；配置代次变化时丢弃过期回调，完成后再处理延期操作。
 */
void NetworkPage::runNetworkTask(std::function<void()> work,
                                std::function<void()> completed)
{
    Q_ASSERT(!networkTaskPending_);
    networkTaskPending_ = true;
    const quint64 generation = networkGeneration_->load();
    const bool queued = QMetaObject::invokeMethod(networkWorker_,
            [this, work, completed, generation]() {
        if (!QThread::currentThread()->isInterruptionRequested() &&
            generation == networkGeneration_->load()) {
            work();
        }
        QMetaObject::invokeMethod(this, [this, completed, generation]() {
            networkTaskPending_ = false;
            dhcpAutoApplying_ = false;
            if (generation == networkGeneration_->load()) completed();
            else if (!webNetworkToken_.isEmpty()) {
                const auto token = webNetworkToken_;
                webNetworkToken_.clear();
                emit webNetworkFinished(token, {}, 409,
                                        QStringLiteral("网络设置已被设备端新操作取代，请刷新核对"));
            }

            // 手动应用/离线清理优先，防止与尚在执行的续租命令交叉。
            if (!networkTaskPending_ && deferredNetworkAction_) {
                auto action = std::move(deferredNetworkAction_);
                deferredNetworkAction_ = {};
                action();
            }
            if (!networkTaskPending_ && networkRefreshPending_) {
                networkRefreshPending_ = false;
                refreshUiFromSystem(ifaceName_.trimmed().isEmpty()
                                    ? QStringLiteral("eth0") : ifaceName_);
            }
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
    if (!queued) {
        networkTaskPending_ = false;
        dhcpAutoApplying_ = false;
        appendLog(QStringLiteral("网络后台任务投递失败"));
        if (!webNetworkToken_.isEmpty()) {
            const auto token = webNetworkToken_;
            webNetworkToken_.clear();
            emit webNetworkFinished(token, {}, 503, QStringLiteral("网络后台任务投递失败"));
        }
    }
}

/**
 * @brief 合并重复回显请求，后台采集后仅应用当前配置代次的结果。
 * @param iface 目标有线网卡名称。
 * @param completed 结果回到 GUI 线程后的完成回调。
 */
void NetworkPage::refreshUiFromSystem(const QString &iface, std::function<void()> completed)
{
    if (networkTaskPending_) {
        if (completed) {
            const quint64 generation = networkGeneration_->load();
            deferredNetworkAction_ = [this, iface, completed, generation]() {
                if (generation == networkGeneration_->load()) refreshUiFromSystem(iface, completed);
            };
        } else {
            networkRefreshPending_ = true;
        }
        return;
    }
    auto state = std::make_shared<NetworkRuntimeState>();
    runNetworkTask([state, iface]() {
        *state = queryNetworkRuntimeState(iface, false);
    }, [this, state, completed]() {
        ui->ipAddressEdit->setText(state->ip.isEmpty() ? "-" : state->ip);
        ui->subnetMaskEdit->setText(state->mask.isEmpty() ? "-" : state->mask);
        ui->gatewayEdit->setText(state->gateway.isEmpty() ? "-" : state->gateway);
        ui->dnsEdit->setText(state->dns.isEmpty() ? "-" : state->dns);
        if (completed) completed();
    });
}

/**
 * @brief 创建网络配置 UI 和 DHCP 状态同步定时器。
 * @param parent Qt 父对象；对象销毁由父子关系管理，空指针表示无父对象。
 */
NetworkPage::NetworkPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::NetworkPage)
{
    ui->setupUi(this);

    networkThread_ = new QThread(this);
    networkThread_->setObjectName(QStringLiteral("DhcpNetworkThread"));
    networkWorker_ = new QObject;
    networkWorker_->moveToThread(networkThread_);
    connect(networkThread_, &QThread::finished, networkWorker_, &QObject::deleteLater);
    networkThread_->start();

    // cancel -> signal
    connect(ui->cancelButton, &QPushButton::clicked,
            this, &NetworkPage::cancelRequested);

    // 使用不符合 connectSlotsByName 规则的槽名，只保留这一条显式连接，
    // 避免 .ui 自动连接与手工连接叠加后一次点击执行两遍。
    connect(ui->applyButton, &QPushButton::clicked,
            this, &NetworkPage::handleApplyClicked);

    // ✅ 这些输入框都改成“只读”，点击时弹键盘
    QLineEdit* edits[] = {
        ui->ipAddressEdit,
        ui->subnetMaskEdit,
        ui->gatewayEdit,
        ui->dnsEdit
    };

    for (auto *e : edits) {
        e->setReadOnly(true);
        e->setFocusPolicy(Qt::StrongFocus);
        e->setProperty("ycestCustomKeyboard", true);
        e->installEventFilter(this);
    }

    connect(ui->ipModeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &NetworkPage::handleIpModeChanged);

    connect(ui->connectionTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &NetworkPage::handleConnectionTypeChanged);

    // IP初始状态
    handleIpModeChanged(ui->ipModeComboBox->currentIndex());

    // 网络初始化显示
    handleConnectionTypeChanged(ui->connectionTypeCombo->currentIndex());

    //状态回显
    refreshUiFromSystem("eth0");

    //DHCP 轮询定时器
    dhcpSyncTimer_ = new QTimer(this);
    dhcpSyncTimer_->setInterval(2000); // 2 秒轮询一次
    connect(dhcpSyncTimer_, &QTimer::timeout,
            this, &NetworkPage::syncDhcpRuntimeStateToConfig);
    dhcpSyncTimer_->start();
}


/**
 * @brief 停止轮询，使旧任务失效并等待网络线程退出，再释放页面。
 */
NetworkPage::~NetworkPage()
{
    dhcpSyncTimer_->stop();
    networkGeneration_->fetch_add(1);
    networkThread_->requestInterruption();
    networkThread_->quit();
    networkThread_->wait();
    delete ui;
}


/**
 * @brief 有线连接模式切换时刷新可用控件。
 * @param index 界面选项索引。
 */
void NetworkPage::handleConnectionTypeChanged(int index)
{
    networkGeneration_->fetch_add(1);
    networkRefreshPending_ = false;
    // 0=有线 1=无线
    if (index == 0) {
        // 有线页面
        ui->networkStack->setCurrentIndex(0);

        // 有线状态回显
        refreshUiFromSystem("eth0");
    } else {
        // 无线页面
        ui->networkStack->setCurrentIndex(1);
    }
}

/**
 * @brief DHCP/静态模式切换时更新输入框状态。
 * @param index 界面选项索引。
 */
void NetworkPage::handleIpModeChanged(int index)
{
    networkGeneration_->fetch_add(1);
    networkRefreshPending_ = false;
    const bool isStatic = (index == 0);     // isStatic:0=静态
    setIpFieldsEnabled(!isStatic);

    if (isStatic) {

    }

    // 静态模式切换时清空缓存
    if (!isStatic) { // DHCP
        lastIp_.clear();
        lastMask_.clear();
        lastGateway_.clear();
        lastDns_.clear();
    }
}

//是否使能IP编辑框
/**
 * @brief 统一设置静态地址输入控件是否可编辑。
 * @param enabled 是否启用相关界面或功能。
 */
void NetworkPage::setIpFieldsEnabled(bool enabled)
{
    ui->ipAddressEdit->setEnabled(enabled);
    ui->subnetMaskEdit->setEnabled(enabled);
    ui->gatewayEdit->setEnabled(enabled);
    ui->dnsEdit->setEnabled(enabled);
}

// ✅ 事件过滤：点击输入框就弹键盘
/**
 * @brief 处理当前页面输入事件并将其余事件交给 Qt。
 * @param watched 收到事件的对象。
 * @param event Qt 事件。
 * @return 事件已被本对象处理时为 true，否则沿用 Qt 事件分派。
 */
bool NetworkPage::eventFilter(QObject *watched, QEvent *event)
{
    // 只处理鼠标点击（触摸屏通常也会转成鼠标事件）
    if (event->type() == QEvent::MouseButtonPress) {
        auto *edit = qobject_cast<QLineEdit *>(watched);
        if (edit && edit->isEnabled()) {
            if (edit == ui->ipAddressEdit) {
                openKeyboardFor(edit, "请输入IP地址");
                return true; // 拦截，避免默认处理
            }
            if (edit == ui->subnetMaskEdit) {
                openKeyboardFor(edit, "请输入子网掩码");
                return true;
            }
            if (edit == ui->gatewayEdit) {
                openKeyboardFor(edit, "请输入网关");
                return true;
            }
            if (edit == ui->dnsEdit) {
                openKeyboardFor(edit, "请输入DNS");
                return true;
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}


/**
 * @brief 使用项目屏幕键盘编辑指定输入框。
 * @param edit 接收键盘输入的文本框。
 * @param title 键盘窗口标题。
 */
void NetworkPage::openKeyboardFor(QLineEdit *edit, const QString &title)
{
    networkGeneration_->fetch_add(1);
    networkRefreshPending_ = false;
    bool ok = false;

    // 这里 maxLen 给 15（IPv4 最大 15 字符：255.255.255.255）
    const QString text = KeyboardDialog::getText(
        this,
        title,
        edit->text(),
        QLineEdit::Normal,
        15,
        &ok
    );

    if (ok) {
        edit->setText(text.trimmed());
    }
}


/**
 * @brief 保存并应用当前网络设置。
 */
void NetworkPage::handleApplyClicked()
{
    applyNetworkConfig(true);
}

/**
 * @brief 执行 DHCP 或静态网络命令，并按需显示结果。
 * @param showMessage 是否向设备界面显示操作结果。
 * @return 本次设置被校验并受理时为 true；后台结果另行回报。
 */
bool NetworkPage::applyNetworkConfig(bool showMessage)
{
    const quint64 generation = networkGeneration_->fetch_add(1) + 1;
    networkRefreshPending_ = false;
    if (networkTaskPending_) {
        deferredNetworkAction_ = [this, showMessage, generation]() {
            if (generation == networkGeneration_->load()) applyNetworkConfig(showMessage);
        };
        return true; // 已受理；等旧后台任务结束后再应用，避免并发修改网卡。
    }
    // online_v1-v2启用功能
    if (!isNetworkFeatureEnabled()) {
        const QString iface = ifaceName_.trimmed().isEmpty() ? QString("eth0") : ifaceName_;

        // 当勾选框为 offline_v1 时，清空配置文件IP，并清掉系统当前IP
        if (featureSettings_.offline_v1) {
            QString err;
            if (!clearIfaceNetworkConfig(iface, &err)) {
                appendLog(QString("offline_v1开启时清理系统网络失败：%1").arg(err));
            } else {
                appendLog(QString("offline_v1开启时已清理系统网络：%1").arg(iface));
            }

            if (!clearNetworkConfigFileIp(configDir_)) {
                appendLog("offline_v1开启时清理配置文件IP失败");
            }

            refreshUiFromSystem(iface);
        }

        if (showMessage) {
            UiMessageBox::warning(this, "权限不足", "未启用网络功能");
        }
        appendLog("未启用网络功能");
        return false;
    }

    const QString iface = ifaceName_.trimmed().isEmpty() ? QString("eth0") : ifaceName_;
    const int mode = ui->ipModeComboBox->currentIndex(); // 0=DHCP 1=静态

    QString err;

    // 0=有线 1=无线
    if (ui->connectionTypeCombo->currentIndex() == 1) {
        if (showMessage) {
            UiMessageBox::information(this, "提示", "无线网络功能开发中，暂不可用。");
        }
        appendLog("无线网络功能开发中，暂不可用");
        return false;
    }

    if (mode == 0) {
        applyDhcpNetworkConfig(iface, showMessage);
        return true;
    }

    if (!runCmd("ip", {"link", "set", iface, "up"}, &err)) {
        if (showMessage) {
            UiMessageBox::critical(this, "失败", "拉起网卡失败: " + err);
        }
        appendLog(QString("拉起网卡失败: %1").arg(err));
        return false;
    }

    // 静态 IP
    stopDhcpClient(iface);

    const QString ip   = ui->ipAddressEdit->text().trimmed();
    const QString mask = ui->subnetMaskEdit->text().trimmed();
    const QString gw   = ui->gatewayEdit->text().trimmed();
    const QString dns  = ui->dnsEdit->text().trimmed();

    if (!isIPv4(ip)) {
        if (showMessage) {
            UiMessageBox::warning(this, "输入错误", "IP 地址不合法");
        }
        appendLog("IP 地址不合法");
        return false;
    }

    int prefix = netmaskToPrefix(mask);
    if (prefix < 0) {
        if (showMessage) {
            UiMessageBox::warning(this, "输入错误", "子网掩码不合法（例如 255.255.255.0）");
        }
        appendLog("子网掩码不合法");
        return false;
    }

    if (!gw.isEmpty() && !isIPv4(gw)) {
        if (showMessage) {
            UiMessageBox::warning(this, "输入错误", "网关不合法");
        }
        appendLog("网关不合法");
        return false;
    }

    if (!dns.isEmpty() && !isIPv4(dns)) {
        if (showMessage) {
            UiMessageBox::warning(this, "输入错误", "DNS 不合法");
        }
        appendLog("DNS 不合法");
        return false;
    }

    if (!runCmd("ip", {"addr", "flush", "dev", iface}, &err)) {
        if (showMessage) {
            UiMessageBox::critical(this, "失败", "清理旧地址失败: " + err);
        }
        appendLog(QString("清理旧地址失败: %1").arg(err));
        return false;
    }

    const QString ipWithPrefix = QString("%1/%2").arg(ip).arg(prefix);
    if (!runCmd("ip", {"addr", "add", ipWithPrefix, "dev", iface}, &err)) {
        if (showMessage) {
            UiMessageBox::critical(this, "失败", "设置IP失败: " + err);
        }
        appendLog(QString("设置IP失败: %1").arg(err));
        return false;
    }

    if (!gw.isEmpty()) {
        if (!runCmd("ip", {"route", "replace", "default", "via", gw, "dev", iface}, &err)) {
            if (showMessage) {
                UiMessageBox::critical(this, "失败", "设置默认网关失败: " + err);
            }
            appendLog(QString("设置默认网关失败: %1").arg(err));
            return false;
        }
    }

    if (!dns.isEmpty()) {
        QFile f("/etc/resolv.conf");
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            if (showMessage) {
                UiMessageBox::warning(this, "提示", "IP已设置，但写入 /etc/resolv.conf 失败（DNS可能未更新）");
            }
            appendLog("IP已设置，但写入 /etc/resolv.conf 失败（DNS可能未更新）");
        } else {
            QTextStream ts(&f);
            ts << "nameserver " << dns << "\n";
            f.close();
        }
    }

    refreshUiFromSystem(iface, [this, iface, showMessage]() {
        saveInputsToConfigFile(configDir_);
        if (showMessage) {
            UiMessageBox::information(this, "成功", QString("静态 IP 已应用（%1）").arg(iface));
        }
        appendLog(QString("静态 IP 已应用（%1）").arg(iface));
        emit networkAppliedSuccessfully();
    });
    return true;
}

/**
 * @brief Web 使用原网卡命令与 DHCP 客户端，在共享网络工作线程执行并保存配置。
 * @param token 贯穿请求与异步结果的任务标识。
 * @param input Web 提交的网络设置，进入异步网络操作前统一校验。
 * @note GUI 线程受理并排队到专用网络线程；完成后通过 token 回报 Web，禁止在界面线程等待网关查询或续租命令。
 */
void NetworkPage::applyWebNetworkConfig(const QString &token, const QJsonObject &input)
{
    if (networkTaskPending_ || !webNetworkToken_.isEmpty()) {
        emit webNetworkFinished(token, {}, 409, QStringLiteral("网络正在配置，请稍后重试"));
        return;
    }
    const QString cfgPath = QDir(configDir_).filePath(configFileName_);
    QString error;
    const auto current = NetworkConfig::read(cfgPath, &error);
    QJsonObject config;
    if (!error.isEmpty() || !NetworkConfig::normalize(input, current, &config, &error) ||
        input.value("iface") != config.value("iface")) {
        emit webNetworkFinished(token,
                                {},
                                400,
                                error.isEmpty() ? QStringLiteral("网卡配置已变化，请刷新后重试")
                                                : error);
        return;
    }
    const auto iface = config.value("iface").toString();
    const auto device = QNetworkInterface::interfaceFromName(iface);
    // 只操作有线接口；热点、无线、回环和点对点接口不参与设备 IP 设置。
    bool portalAddress = false;
    for (const auto &entry : device.addressEntries())
        portalAddress = portalAddress || entry.ip().toString() == "192.168.10.1";
    if (!device.isValid() || device.type() == QNetworkInterface::Wifi ||
        device.flags().testFlag(QNetworkInterface::IsLoopBack) ||
        device.flags().testFlag(QNetworkInterface::IsPointToPoint) || portalAddress ||
        QFileInfo::exists("/sys/class/net/" + iface + "/wireless") ||
        QFileInfo::exists("/sys/class/net/" + iface + "/phy80211")) {
        emit webNetworkFinished(token,
                                {},
                                400,
                                QStringLiteral("配置目标必须是可用的有线网卡，不能修改热点接口"));
        return;
    }

    struct Result {
        NetworkRuntimeState state;
        QJsonObject config;
        QString error;
        bool saved = false;
        bool applied = false;
    };

    auto result = std::make_shared<Result>();
    result->config = config;
    webNetworkToken_ = token;
    networkGeneration_->fetch_add(1);
    networkRefreshPending_ = false;
    const auto generation = networkGeneration_;
    const auto expected = generation->load();
    runNetworkTask(
        [result, iface, config, cfgPath, generation, expected]() {
            const auto valid = [generation, expected]() {
                return generation->load() == expected &&
                       !QThread::currentThread()->isInterruptionRequested();
            };
            // 参数已全部校验，系统命令和地址等待均不占用 Qt 主线程。
            auto &error = result->error;
            if (!valid() || !runCmd("ip", {"link", "set", iface, "up"}, &error))
                return;
            if (config.value("dhcp").toBool() && !isIfaceCarrierUp(iface)) {
                error = QStringLiteral("网线未连接，无法获取 DHCP 地址");
                return;
            }
            if (!valid() || !stopDhcpClient(iface, &error))
                return;
            if (!valid() || !runCmd("ip", {"addr", "flush", "dev", iface}, &error))
                return;
            if (!valid() || !runCmd("ip", {"route", "flush", "dev", iface}, &error))
                return;
            if (config.value("dhcp").toBool()) {
                if (!valid() || !startDhcpDaemon(iface, &error))
                    return;
                for (int n = 0; n < 30 && valid(); ++n) {
                    getIfaceIPv4AndMask(iface, &result->state.ip, &result->state.mask);
                    if (!result->state.ip.isEmpty() && result->state.ip != "0.0.0.0" &&
                        !result->state.mask.isEmpty())
                        break;
                    QThread::msleep(100);
                }
            } else {
                const QString cidr =
                    config.value("ip").toString() + "/" +
                    QString::number(netmaskToPrefix(config.value("netmask").toString()));
                if (!valid() || !runCmd("ip", {"addr", "add", cidr, "dev", iface}, &error))
                    return;
                const auto gateway = config.value("gateway").toString();
                if (!gateway.isEmpty() &&
                    (!valid() ||
                     !runCmd("ip",
                             {"route", "replace", "default", "via", gateway, "dev", iface},
                             &error)))
                    return;
                const auto dns = config.value("dns").toString();
                if (!dns.isEmpty()) {
                    QFile file("/etc/resolv.conf");
                    QByteArray bytes;
                    for (const auto &server : dns.split(' ', QString::SkipEmptyParts))
                        bytes += "nameserver " + server.toUtf8() + '\n';
                    if (!valid() || !file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                        file.write(bytes) != bytes.size() || !file.flush()) {
                        error = QStringLiteral("IP 已应用，但 DNS 配置写入失败");
                        return;
                    }
                }
            }
            if (!valid())
                return;
            result->state = queryNetworkRuntimeState(iface, false);
            const auto &state = result->state;
            result->applied = (!config.value("dhcp").toBool() || state.carrierUp) &&
                              !state.ip.isEmpty() && state.ip != "0.0.0.0" && !state.mask.isEmpty();
            if (config.value("dhcp").toBool()) {
                // DHCP 启动成功但尚未取得租约时保存模式，后续沿用原 2 秒同步定时器。
                result->config["ip"] = result->applied ? state.ip : QString();
                result->config["netmask"] = result->applied ? state.mask : QString();
                result->config["gateway"] = result->applied ? state.gateway : QString();
                result->config["dns"] = result->applied ? state.dns : QString();
            } else if (state.ip != config.value("ip").toString() ||
                       state.mask != config.value("netmask").toString() ||
                       (!config.value("gateway").toString().isEmpty() &&
                        state.gateway != config.value("gateway").toString())) {
                result->applied = false;
                error = QStringLiteral("系统实际地址或网关未达到请求配置，请刷新核对");
                return;
            }
            if (valid())
                result->saved = NetworkConfig::save(cfgPath, result->config, &error);
        },
        [this, token, iface, result]() {
            webNetworkToken_.clear();
            if (!result->saved) {
                emit webNetworkFinished(token,
                                        {{"saved", false}, {"applied", result->applied}},
                                        503,
                                        result->error.isEmpty()
                                            ? QStringLiteral("网络配置未完成，请刷新核对实际状态")
                                            : result->error);
                return;
            }
            // 同步后台页面和 DHCP 缓存，防止旧 UI 下一次轮询覆盖刚保存的模式。
            webNetworkEnabled_ = true;
            ifaceName_ = iface;
            const QSignalBlocker blockMode(ui->ipModeComboBox);
            const QSignalBlocker blockType(ui->connectionTypeCombo);
            ui->connectionTypeCombo->setCurrentIndex(0);
            ui->ipModeComboBox->setCurrentIndex(result->config.value("dhcp").toBool() ? 0 : 1);
            setIpFieldsEnabled(!result->config.value("dhcp").toBool());
            lastIp_ = result->config.value("ip").toString();
            lastMask_ = result->config.value("netmask").toString();
            lastGateway_ = result->config.value("gateway").toString();
            lastDns_ = result->config.value("dns").toString();
            lastCarrierUp_ = result->state.carrierUp;
            dhcpHadValidIp_ = result->applied;
            lastDhcpAutoTryMs_ = QDateTime::currentMSecsSinceEpoch();
            ui->ipAddressEdit->setText(lastIp_);
            ui->subnetMaskEdit->setText(lastMask_);
            ui->gatewayEdit->setText(lastGateway_);
            ui->dnsEdit->setText(lastDns_);
            auto reply = result->config;
            reply.insert("saved", true);
            reply.insert("applied", result->applied);
            reply.insert("waitingForDhcp",
                         result->config.value("dhcp").toBool() && !result->applied);
            emit webNetworkFinished(token, reply, 200, {});
            if (result->applied)
                emit networkAppliedSuccessfully();
        });
}

/**
 * @brief 向页面日志区追加文本。
 * @param text 待显示的日志文本。
 */
void NetworkPage::appendLog(const QString &text)
{
    const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    qDebug().noquote() << QString("[NetworkPage][%1] %2").arg(ts, text);
}

/**
 * @brief 从指定目录的 INI 文件回显网络输入。
 * @param cfgDir 网络配置所在目录。
 * @return 配置已成功读取并回显时为 true。
 */
bool NetworkPage::loadInputsFromConfigFile(const QString& cfgDir)
{
    const QString cfgPath = QDir(cfgDir).filePath(configFileName_);

    if (!QFileInfo::exists(cfgPath)) {
        appendLog(QString("网络配置读取失败：文件不存在 %1").arg(cfgPath));
        return false;
    }

    QSettings ini(cfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    const bool useCfg = ini.value("network/use_config", false).toBool();
    if (!useCfg) {
        appendLog("配置文件存在，但 network/use_config=false，不覆盖网络输入框");
        return true;
    }

    networkGeneration_->fetch_add(1);
    networkRefreshPending_ = false;

    const QString iface = ini.value("network/iface", ifaceName_).toString().trimmed();
    const bool autoConnect = ini.value("network/auto_connect", false).toBool();
    const bool dhcp = ini.value("network/dhcp", true).toBool();

    const QString ip   = ini.value("network/ip", "").toString().trimmed();
    const QString mask = ini.value("network/netmask", "").toString().trimmed();
    const QString gw   = ini.value("network/gateway", "").toString().trimmed();
    const QString dns  = ini.value("network/dns", "").toString().trimmed();

    if (!iface.isEmpty()) {
        ifaceName_ = iface;
    }

    // 这里默认当前只支持有线，所以 connectionTypeCombo 设为有线
    if (ui->connectionTypeCombo) {
        ui->connectionTypeCombo->setCurrentIndex(0);
    }

    // 0 = DHCP, 1 = 静态
    if (ui->ipModeComboBox) {
        ui->ipModeComboBox->setCurrentIndex(dhcp ? 0 : 1);
    }

    if (dhcp) {
        if (!ui->ipAddressEdit->hasFocus())  ui->ipAddressEdit->setText("-");
        if (!ui->subnetMaskEdit->hasFocus()) ui->subnetMaskEdit->setText("-");
        if (!ui->gatewayEdit->hasFocus())    ui->gatewayEdit->setText("-");
        if (!ui->dnsEdit->hasFocus())        ui->dnsEdit->setText("-");
    } else {
        if (!ui->ipAddressEdit->hasFocus())  ui->ipAddressEdit->setText(ip);
        if (!ui->subnetMaskEdit->hasFocus()) ui->subnetMaskEdit->setText(mask);
        if (!ui->gatewayEdit->hasFocus())    ui->gatewayEdit->setText(gw);
        if (!ui->dnsEdit->hasFocus())        ui->dnsEdit->setText(dns);
    }

    appendLog(QString("网络配置填充成功：%1").arg(cfgPath));
    appendLog(QString("iface=%1 auto_connect=%2 dhcp=%3 ip=%4 mask=%5 gw=%6 dns=%7")
              .arg(ifaceName_)
              .arg(autoConnect ? "true" : "false")
              .arg(dhcp ? "true" : "false")
              .arg(ip, mask, gw, dns));

    return true;
}

/**
 * @brief 根据已保存配置执行一次自动联网。
 */
void NetworkPage::autoConnectFromSavedConfig()
{
    // online_v1-v2启用功能
    if (!isNetworkFeatureEnabled()) {
        appendLog("未启用网络功能，跳过自动网络连接");
        qDebug() << "未启用网络功能，跳过自动网络连接";
        return;
    }

    appendLog("自动模式：开始读取网络配置");

    const QString cfgPath = QDir(configDir_).filePath(configFileName_);
    if (!QFileInfo::exists(cfgPath)) {
        appendLog(QString("自动连接失败：配置文件不存在 %1").arg(cfgPath));
        return;
    }

    QSettings ini(cfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    const bool useCfg = ini.value("network/use_config", false).toBool();
    const bool autoConnect = ini.value("network/auto_connect", false).toBool();

    if (!useCfg) {
        appendLog("自动模式：network/use_config=false，跳过网络自动连接");
        return;
    }

    if (!autoConnect) {
        appendLog("自动模式：network/auto_connect=false，跳过网络自动连接");
        return;
    }

    if (webNetworkEnabled_) {
        // 显式 Web 配置重启后仍走共享后台线程，避免静态设置占用启动主线程。
        if (networkTaskPending_) {
            deferredNetworkAction_ = [this]() { autoConnectFromSavedConfig(); };
            return;
        }
        QString error;
        const auto config = NetworkConfig::read(cfgPath, &error);
        if (!error.isEmpty()) {
            appendLog(error);
            return;
        }
        applyWebNetworkConfig(QStringLiteral("network-autoconnect"), config);
        return;
    }

    const bool dhcp = ini.value("network/dhcp", true).toBool();
    if (dhcp) {
        if (!clearNetworkConfigFileIp(configDir_)) {
            appendLog("自动模式：清空 DHCP 缓存 IP 失败");
        } else {
            appendLog("自动模式：已清空 DHCP 缓存 IP，等待重新获取");
        }
    }

    // 先把配置填到 UI
    loadInputsFromConfigFile(configDir_);

    appendLog("自动模式：开始应用网络配置");
    applyNetworkConfig(false);
}


/**
 * @brief 将当前输入保存到网络配置文件。
 * @param cfgDir 网络配置所在目录。
 * @return 网络输入成功持久化时为 true。
 */
bool NetworkPage::saveInputsToConfigFile(const QString& cfgDir)
{
    const QString cfgPath = QDir(cfgDir).filePath(configFileName_);

    QSettings ini(cfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    const bool dhcp = (ui->ipModeComboBox->currentIndex() == 0);

    ini.setValue("network/use_config", true);
    ini.setValue("network/iface", ifaceName_);
    ini.setValue("network/auto_connect", true);
    ini.setValue("network/dhcp", dhcp);
    ini.setValue("network/ip", normalizeNetValue(ui->ipAddressEdit->text()));
    ini.setValue("network/netmask", normalizeNetValue(ui->subnetMaskEdit->text()));
    ini.setValue("network/gateway", normalizeNetValue(ui->gatewayEdit->text()));
    ini.setValue("network/dns", normalizeNetValue(ui->dnsEdit->text()));

    ini.sync();

    if (ini.status() != QSettings::NoError) {
        appendLog(QString("保存网络配置失败：%1").arg(cfgPath));
        return false;
    }

    appendLog(QString("网络配置已保存：%1").arg(cfgPath));
    return true;
}


/**
 * @brief 清除配置文件中的静态 IP 字段。
 * @param cfgDir 网络配置所在目录。
 * @return 原配置中的网络地址清理成功时为 true。
 */
bool NetworkPage::clearNetworkConfigFileIp(const QString& cfgDir)
{
    const QString cfgPath = QDir(cfgDir).filePath(configFileName_);

    QSettings ini(cfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    // 保持原有其它配置，只清空 IP 相关项
    ini.setValue("network/ip", "");
    ini.setValue("network/netmask", "");
    ini.setValue("network/gateway", "");
    ini.setValue("network/dns", "");

    ini.sync();

    if (ini.status() != QSettings::NoError) {
        appendLog(QString("清空配置文件IP失败：%1").arg(cfgPath));
        return false;
    }

    appendLog(QString("配置文件IP相关项已清空：%1").arg(cfgPath));
    return true;
}

/**
 * @brief 更新功能开关并决定网络功能是否可用。
 * @param settings 设备功能开关快照。
 */
void NetworkPage::setFeatureSettings(const FeatureSettings &settings)
{
    // 只有显式 Web 配置才保留独立有线联网意图，未使用 Web 的设备沿用原模式规则。
    QSettings networkIni(QDir(configDir_).filePath(configFileName_), QSettings::IniFormat);
    webNetworkEnabled_ = networkIni.value("network/web_managed", false).toBool();
    const quint64 generation = networkGeneration_->fetch_add(1) + 1;
    networkRefreshPending_ = false;
    featureSettings_ = settings;

    qDebug() << "[D] setFeatureSettings:"
             << "offline_v1=" << featureSettings_.offline_v1
             << "online_v1=" << featureSettings_.online_v1
             << "online_v2=" << featureSettings_.online_v2;

    // offline_v1 启用，且网络功能未启用时，立即清理
    if (featureSettings_.offline_v1 && !isNetworkFeatureEnabled()) {
        if (networkTaskPending_) {
            deferredNetworkAction_ = [this, settings, generation]() {
                if (generation == networkGeneration_->load()) setFeatureSettings(settings);
            };
            return;
        }
        const QString iface = ifaceName_.trimmed().isEmpty() ? QString("eth0") : ifaceName_;
        QString err;

        if (!clearIfaceNetworkConfig(iface, &err)) {
            appendLog(QString("setFeatureSettings 清理系统网络失败：%1").arg(err));
        } else {
            appendLog(QString("setFeatureSettings 已清理系统网络：%1").arg(iface));
        }

        if (!clearNetworkConfigFileIp(configDir_)) {
            appendLog("setFeatureSettings 清理配置文件IP失败");
        }

        refreshUiFromSystem(iface);
    }
}

/**
 * @brief 合并功能配置与显式 Web 设置判断是否允许联网。
 * @return 在线功能或显式 Web 网络配置要求联网时返回 true。
 */
bool NetworkPage::isNetworkFeatureEnabled() const
{
    return featureSettings_.online_v1 || featureSettings_.online_v2 || webNetworkEnabled_;
}


// DHCP 运行时同步
/**
 * @brief 按连接状态触发后台网络查询并同步 DHCP 运行参数。
 * @note 轮询仅提交后台采样请求；已有网络任务时合并或延期，界面线程不执行阻塞网关命令。
 */
void NetworkPage::syncDhcpRuntimeStateToConfig()
{
    if (!ui || networkTaskPending_ || dhcpAutoApplying_) return;

    if (!isNetworkFeatureEnabled()) {
        return;
    }

    // 只处理有线
    if (ui->connectionTypeCombo->currentIndex() != 0)
        return;

    // 只处理 DHCP 模式：0 = DHCP，1 = 静态
    if (ui->ipModeComboBox->currentIndex() != 0)
        return;

    const QString iface = ifaceName_.trimmed().isEmpty() ? QString("eth0") : ifaceName_;

    auto state = std::make_shared<NetworkRuntimeState>();
    runNetworkTask([state, iface]() {
        *state = queryNetworkRuntimeState(iface, true);
    }, [this, iface, state]() {
        applyDhcpRuntimeState(iface, *state);
    });
}

/**
 * @brief GUI 线程沿用原状态转换，仅消费后台采集值。
 * @param iface 目标有线网卡名称。
 * @param state 工作线程采集的当前网卡状态。
 */
void NetworkPage::applyDhcpRuntimeState(const QString &iface,
                                      const NetworkRuntimeState &state)
{

    // 先看物理链路是否存在
    const bool carrierUp = state.carrierUp;
    const bool linkBecameUp = carrierUp && !lastCarrierUp_;
    const bool linkBecameDown = !carrierUp && lastCarrierUp_;

    lastCarrierUp_ = carrierUp;

    if (linkBecameUp) {
        appendLog(QString("检测到网线插入：%1 carrier=1").arg(iface));

        lastDhcpAutoTryMs_ = 0;
        requestAutoDhcpRenew("网线插入后自动获取DHCP", false);
        if (networkTaskPending_) return; // 续租后重新采样，不能应用续租前的地址。
    }

    if (linkBecameDown) {
        appendLog(QString("检测到网线断开：%1 carrier=0").arg(iface));

        lastIp_.clear();
        lastMask_.clear();
        lastGateway_.clear();
        lastDns_.clear();
        dhcpHadValidIp_ = false;

        ui->ipAddressEdit->setText("-");
        ui->subnetMaskEdit->setText("-");
        ui->gatewayEdit->setText("-");
        ui->dnsEdit->setText("-");

        clearNetworkConfigFileIp(configDir_);

        /*
         * 注意：
         * 这里不要通知播放器切本地轮播。
         * LIVE断流后的回退由 MultimediaDemo 的直播 watchdog 负责。
         */
        return;
    }

    // 如果链路断开，就认为当前 DHCP 网络无效，即使系统里还残留旧 IP
    if (!carrierUp) {
        return;
    }

    const QString ip = state.ip;
    const QString mask = state.mask;
    const QString gw = state.gateway;
    const QString dns = state.dns;

    const bool hasValidIp = (!ip.isEmpty() && ip != "0.0.0.0" && !mask.isEmpty());

    // 链路存在但还没拿到有效 IP，也清空缓存
    if (!hasValidIp) {
        const bool dhcpRunning = state.dhcpRunning;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();

        if (!dhcpRunning) {
            requestAutoDhcpRenew("网线已连接但DHCP客户端未运行，自动获取DHCP", false);
            return;
        }

        if (lastDhcpAutoTryMs_ > 0 &&
            now - lastDhcpAutoTryMs_ >= kDhcpForceRetryIntervalMs) {
            requestAutoDhcpRenew("DHCP客户端已运行但长时间无有效IP，强制重新获取", true);
            return;
        }

        return;
    }

    const bool changed =
        (ip != lastIp_) ||
        (mask != lastMask_) ||
        (gw != lastGateway_) ||
        (dns != lastDns_);

    const bool firstValidIp = !dhcpHadValidIp_;

    if (!changed && !firstValidIp) {
        return;
    }

    lastIp_ = ip;
    lastMask_ = mask;
    lastGateway_ = gw;
    lastDns_ = dns;
    dhcpHadValidIp_ = true;

    // 刷新 UI
    ui->ipAddressEdit->setText(ip);
    ui->subnetMaskEdit->setText(mask);
    ui->gatewayEdit->setText(gw.isEmpty() ? "-" : gw);
    ui->dnsEdit->setText(dns.isEmpty() ? "-" : dns);

    saveInputsToConfigFile(configDir_);

    appendLog(QString("检测到 DHCP 网络参数有效，已同步配置文件：ip=%1 mask=%2 gw=%3 dns=%4")
              .arg(ip, mask, gw, dns));

    emit networkAppliedSuccessfully();
}


/**
 * @brief 按重试节流请求 DHCP 续租或强制重启客户端。
 * @param reason 自动续租的触发原因，用于诊断日志。
 * @param forceRestart 是否强制重启 DHCP 客户端。
 */
void NetworkPage::requestAutoDhcpRenew(const QString &reason, bool forceRestart)
{
    if (!ui || networkTaskPending_ || dhcpAutoApplying_ || !isNetworkFeatureEnabled() ||
        ui->connectionTypeCombo->currentIndex() != 0 ||
        ui->ipModeComboBox->currentIndex() != 0) {
        return;
    }
    const QString iface = ifaceName_.trimmed().isEmpty() ? QString("eth0") : ifaceName_;
    if (!lastCarrierUp_) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!forceRestart && lastDhcpAutoTryMs_ > 0 &&
        now - lastDhcpAutoTryMs_ < kDhcpAutoRetryIntervalMs) {
        appendLog(QString("自动DHCP重试间隔过短，暂不重复执行：%1").arg(reason));
        return;
    }

    dhcpAutoApplying_ = true;
    lastDhcpAutoTryMs_ = now;
    appendLog(QString("自动DHCP开始：iface=%1 forceRestart=%2 reason=%3")
              .arg(iface).arg(forceRestart ? "true" : "false").arg(reason));
    struct Result {
        NetworkRuntimeState state;
        QString error;
        bool carrierUp = false;
        bool reset = false;
        bool started = false;
    };
    auto result = std::make_shared<Result>();
    const auto generation = networkGeneration_;
    const quint64 expected = generation->load();
    runNetworkTask([iface, forceRestart, result, generation, expected]() {
        const auto current = [generation, expected]() {
            return generation->load() == expected &&
                    !QThread::currentThread()->isInterruptionRequested();
        };
        if (!current()) return;
        result->carrierUp = isIfaceCarrierUp(iface);
        if (!result->carrierUp) return;
        if (forceRestart || !isDhcpRunning(iface)) {
            stopDhcpClient(iface, nullptr);
            if (!current()) return;
            runCmd("ip", {"link", "set", iface, "up"}, &result->error, 3000);
            if (!current()) return;
            runCmd("ip", {"addr", "flush", "dev", iface}, nullptr, 3000);
            if (!current()) return;
            runCmd("ip", {"route", "flush", "dev", iface}, nullptr, 3000);
            if (!current()) return;
            result->reset = true;
            result->started = startDhcpDaemon(iface, &result->error);
        }
        if (current()) result->state = queryNetworkRuntimeState(iface, false);
    }, [this, iface, reason, result]() {
        if (!result->carrierUp) {
            appendLog(QString("自动DHCP取消：%1 未检测到网线，reason=%2").arg(iface, reason));
            return;
        }
        if (result->reset) {
            clearNetworkConfigFileIp(configDir_);
            lastIp_.clear();
            lastMask_.clear();
            lastGateway_.clear();
            lastDns_.clear();
            dhcpHadValidIp_ = false;
            if (!result->started) {
                appendLog(QString("自动DHCP启动失败：%1").arg(result->error));
            } else {
                appendLog(QString("自动DHCP已启动：%1，等待定时器同步IP/网关/DNS").arg(iface));
            }
        } else {
            appendLog(QString("自动DHCP：%1 udhcpc 已运行，等待获取IP").arg(iface));
        }
        const auto &state = result->state;
        ui->ipAddressEdit->setText(state.ip.isEmpty() ? "-" : state.ip);
        ui->subnetMaskEdit->setText(state.mask.isEmpty() ? "-" : state.mask);
        ui->gatewayEdit->setText(state.gateway.isEmpty() ? "-" : state.gateway);
        ui->dnsEdit->setText(state.dns.isEmpty() ? "-" : state.dns);
    });
}

/**
 * @brief 手动或启动 DHCP 的命令及地址等待全部在同一工作线程执行。
 * @param iface 目标有线网卡名称。
 * @param showMessage 是否向设备界面显示操作结果。
 */
void NetworkPage::applyDhcpNetworkConfig(const QString &iface, bool showMessage)
{
    struct Result {
        NetworkRuntimeState state;
        QString error;
        enum Failure { None, LinkUp, NoCarrier, Start } failure = None;
    };
    auto result = std::make_shared<Result>();
    const auto generation = networkGeneration_;
    const quint64 expected = generation->load();
    dhcpAutoApplying_ = true;
    runNetworkTask([iface, result, generation, expected]() {
        const auto current = [generation, expected]() {
            return generation->load() == expected &&
                    !QThread::currentThread()->isInterruptionRequested();
        };
        if (!current()) return;
        if (!runCmd("ip", {"link", "set", iface, "up"}, &result->error)) {
            result->failure = Result::LinkUp;
            return;
        }
        if (!current()) return;
        if (!isIfaceCarrierUp(iface)) {
            result->failure = Result::NoCarrier;
            result->state = queryNetworkRuntimeState(iface, false);
            return;
        }
        stopDhcpClient(iface);
        if (!current()) return;
        runCmd("ip", {"addr", "flush", "dev", iface});
        if (!current()) return;
        runCmd("ip", {"route", "flush", "dev", iface});
        if (!current()) return;
        if (!startDhcpDaemon(iface, &result->error)) {
            result->failure = Result::Start;
        } else {
            QString ip, mask;
            for (int i = 0; i < 30 && current(); ++i) {
                getIfaceIPv4AndMask(iface, &ip, &mask);
                if (!ip.isEmpty() && ip != "0.0.0.0" && !mask.isEmpty()) break;
                QThread::msleep(100);
            }
        }
        if (current()) result->state = queryNetworkRuntimeState(iface, false);
    }, [this, iface, showMessage, result]() {
        if (result->failure == Result::LinkUp) {
            if (showMessage) UiMessageBox::critical(this, "失败", "拉起网卡失败: " + result->error);
            appendLog(QString("拉起网卡失败: %1").arg(result->error));
            return;
        }
        const auto &state = result->state;
        ui->ipAddressEdit->setText(state.ip.isEmpty() ? "-" : state.ip);
        ui->subnetMaskEdit->setText(state.mask.isEmpty() ? "-" : state.mask);
        ui->gatewayEdit->setText(state.gateway.isEmpty() ? "-" : state.gateway);
        ui->dnsEdit->setText(state.dns.isEmpty() ? "-" : state.dns);
        if (result->failure == Result::NoCarrier) {
            if (showMessage) UiMessageBox::warning(this, "网线未连接",
                    QString("未检测到 %1 网线连接，请插入网线后再获取DHCP。").arg(iface));
            appendLog(QString("DHCP取消：%1 未检测到网线连接，carrier=0").arg(iface));
            return;
        }
        if (result->failure == Result::Start) {
            if (showMessage) UiMessageBox::warning(this, "DHCP失败",
                    "启动DHCP客户端失败（检查网线/上级设备DHCP）\n" + result->error);
            appendLog(QString("启动DHCP客户端失败: %1").arg(result->error));
            return;
        }
        if (!state.ip.isEmpty() && state.ip != "0.0.0.0" && !state.mask.isEmpty()) {
            saveInputsToConfigFile(configDir_);
            lastIp_ = state.ip;
            lastMask_ = state.mask;
            lastGateway_ = state.gateway;
            lastDns_ = state.dns;
            if (showMessage) UiMessageBox::information(this, "设置成功", "DHCP已启动并获取到网络参数");
            appendLog(QString("DHCP已启动并获取到网络参数：ip=%1 mask=%2 gw=%3 dns=%4")
                      .arg(lastIp_, lastMask_, lastGateway_, lastDns_));
        } else {
            if (showMessage) UiMessageBox::information(this, "提示", "DHCP已启动，等待获取网络参数");
            appendLog("DHCP已启动，但尚未获取到有效IP，等待定时器同步");
        }
        emit networkAppliedSuccessfully();
    });
}
