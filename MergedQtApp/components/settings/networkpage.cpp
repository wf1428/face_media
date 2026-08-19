/**
 * @file networkpage.cpp
 * @brief 有线网络 DHCP/静态地址配置与自动恢复页面的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "networkpage.h"
#include "ui_networkpage.h"

#include <QMessageBox>
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

#include <signal.h>
#include <unistd.h>

#include "platform/rk3566_platform.h"


// 判断字符串是否是合法 IPv4
static bool isIPv4(const QString &s)
{
    QHostAddress addr;
    return addr.setAddress(s) && addr.protocol() == QAbstractSocket::IPv4Protocol;
}

// 过滤 -
static QString normalizeNetValue(const QString& s)
{
    const QString v = s.trimmed();
    return (v.isEmpty() || v == "-") ? QString() : v;
}

//判断链路状态
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


/** @brief 读取接口当前地址、网关和 DNS 并回显到页面。 */
static void refreshUiFromSystem(Ui::NetworkPage* ui, const QString& ifname)
{
    QString ip, mask;
    getIfaceIPv4AndMask(ifname, &ip, &mask);
    const QString gw  = getDefaultGateway(ifname);
    const QString dns = getDnsFromResolvConf();

    ui->ipAddressEdit->setText(ip.isEmpty() ? "-" : ip);
    ui->subnetMaskEdit->setText(mask.isEmpty() ? "-" : mask);
    ui->gatewayEdit->setText(gw.isEmpty() ? "-" : gw);
    ui->dnsEdit->setText(dns.isEmpty() ? "-" : dns);
}


/** @return 指定网卡 udhcpc 进程的 PID 文件路径。 */
static QString dhcpPidFile(const QString& iface)
{
    const QString runDir = QStringLiteral("/run/qt_ycest");
    if (QDir().mkpath(runDir)) {
        return QDir(runDir).filePath(QStringLiteral("dhcp.%1.pid").arg(iface));
    }
    return QDir(QDir::tempPath()).filePath(QStringLiteral("qt_ycest.dhcp.%1.pid").arg(iface));
}


/** @return PID 文件对应的 DHCP 客户端仍存活时返回 true。 */
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


/** @brief 终止指定网卡的 DHCP 客户端并清理 PID 文件。 */
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

/** @return 系统中第一个可用的 udhcpc 事件脚本。 */
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

/** @brief 以后台守护方式为指定网卡启动 udhcpc。 */
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


/** @brief 清除接口旧地址和路由，为 DHCP 或静态配置重新应用做准备。 */
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

/** @brief 创建网络配置 UI 和 DHCP 状态同步定时器。 */
NetworkPage::NetworkPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::NetworkPage)
{
    ui->setupUi(this);

    // cancel -> signal
    connect(ui->cancelButton, &QPushButton::clicked,
            this, &NetworkPage::cancelRequested);

    // apply（显式连接，避免自动连接失败）
    connect(ui->applyButton, &QPushButton::clicked,
            this, &NetworkPage::on_applyButton_clicked);

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
        e->installEventFilter(this);
    }

    connect(ui->ipModeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &NetworkPage::on_ipModeComboBox_currentIndexChanged);

    connect(ui->connectionTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &NetworkPage::on_connectionTypeCombo_currentIndexChanged);

    // IP初始状态
    on_ipModeComboBox_currentIndexChanged(ui->ipModeComboBox->currentIndex());

    // 网络初始化显示
    on_connectionTypeCombo_currentIndexChanged(ui->connectionTypeCombo->currentIndex());

    //状态回显
    refreshUiFromSystem(ui, "eth0");

    //DHCP 轮询定时器
    dhcpSyncTimer_ = new QTimer(this);
    dhcpSyncTimer_->setInterval(2000); // 2 秒轮询一次
    connect(dhcpSyncTimer_, &QTimer::timeout,
            this, &NetworkPage::syncDhcpRuntimeStateToConfig);
    dhcpSyncTimer_->start();
}


/** @brief 停止定时器并释放 Qt Designer UI。 */
NetworkPage::~NetworkPage()
{
    delete ui;
}


/** @brief 有线连接模式切换时刷新可用控件。 */
void NetworkPage::on_connectionTypeCombo_currentIndexChanged(int index)
{
    // 0=有线 1=无线
    if (index == 0) {
        // 有线页面
        ui->networkStack->setCurrentIndex(0);

        // 有线状态回显
        refreshUiFromSystem(ui, "eth0");
    } else {
        // 无线页面
        ui->networkStack->setCurrentIndex(1);
    }
}

/** @brief DHCP/静态模式切换时更新输入框状态。 */
void NetworkPage::on_ipModeComboBox_currentIndexChanged(int index)
{
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
void NetworkPage::setIpFieldsEnabled(bool enabled)
{
    ui->ipAddressEdit->setEnabled(enabled);
    ui->subnetMaskEdit->setEnabled(enabled);
    ui->gatewayEdit->setEnabled(enabled);
    ui->dnsEdit->setEnabled(enabled);
}

// ✅ 事件过滤：点击输入框就弹键盘
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


/** @brief 使用项目屏幕键盘编辑指定输入框。 */
void NetworkPage::openKeyboardFor(QLineEdit *edit, const QString &title)
{
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


/** @brief 保存并应用当前网络设置。 */
void NetworkPage::on_applyButton_clicked()
{
    applyNetworkConfig(true);
}

/** @brief 执行 DHCP 或静态网络命令，并按需显示结果。 */
bool NetworkPage::applyNetworkConfig(bool showMessage)
{
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

            refreshUiFromSystem(ui, iface);
        }

        if (showMessage) {
            QMessageBox::warning(this, "权限不足", "未启用网络功能");
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
            QMessageBox::information(this, "提示", "无线网络功能开发中，暂不可用。");
        }
        appendLog("无线网络功能开发中，暂不可用");
        return false;
    }

    if (!runCmd("ip", {"link", "set", iface, "up"}, &err)) {
        if (showMessage) {
            QMessageBox::critical(this, "失败", "拉起网卡失败: " + err);
        }
        appendLog(QString("拉起网卡失败: %1").arg(err));
        return false;
    }

    if (mode == 0) {
        // DHCP 前先判断网线是否连接
        if (!isIfaceCarrierUp(iface)) {
            if (showMessage) {
                QMessageBox::warning(this,
                                     "网线未连接",
                                     QString("未检测到 %1 网线连接，请插入网线后再获取DHCP。").arg(iface));
            }

            appendLog(QString("DHCP取消：%1 未检测到网线连接，carrier=0").arg(iface));
            refreshUiFromSystem(ui, iface);
            return false;
        }

        stopDhcpClient(iface);
        runCmd("ip", {"addr", "flush", "dev", iface});
        runCmd("ip", {"route", "flush", "dev", iface});

        if (!startDhcpDaemon(iface, &err)) {
            if (showMessage) {
                QMessageBox::warning(this, "DHCP失败",
                                     "启动DHCP客户端失败（检查网线/上级设备DHCP）\n" + err);
            }
            appendLog(QString("启动DHCP客户端失败: %1").arg(err));
            refreshUiFromSystem(ui, iface);
            return false;
        }

        QString ipNow, maskNow;
        for (int i = 0; i < 30; ++i) {
            getIfaceIPv4AndMask(iface, &ipNow, &maskNow);
            if (!ipNow.isEmpty() && ipNow != "0.0.0.0" && !maskNow.isEmpty())
                break;
            runCmd("sleep", {"0.1"});
        }

        refreshUiFromSystem(ui, iface);

        if (!ipNow.isEmpty() && ipNow != "0.0.0.0" && !maskNow.isEmpty()) {
            saveInputsToConfigFile(configDir_);

            lastIp_ = ipNow;
            lastMask_ = maskNow;
            lastGateway_ = getDefaultGateway(iface);
            lastDns_ = getDnsFromResolvConf();

            if (showMessage) {
                QMessageBox::information(this, "设置成功", "DHCP已启动并获取到网络参数");
            }
            appendLog(QString("DHCP已启动并获取到网络参数：ip=%1 mask=%2 gw=%3 dns=%4")
                      .arg(lastIp_, lastMask_, lastGateway_, lastDns_));
        } else {
            if (showMessage) {
                QMessageBox::information(this, "提示", "DHCP已启动，等待获取网络参数");
            }
            appendLog("DHCP已启动，但尚未获取到有效IP，等待定时器同步");
        }

        emit networkAppliedSuccessfully();
        return true;
    }

    // 静态 IP
    stopDhcpClient(iface);

    const QString ip   = ui->ipAddressEdit->text().trimmed();
    const QString mask = ui->subnetMaskEdit->text().trimmed();
    const QString gw   = ui->gatewayEdit->text().trimmed();
    const QString dns  = ui->dnsEdit->text().trimmed();

    if (!isIPv4(ip)) {
        if (showMessage) {
            QMessageBox::warning(this, "输入错误", "IP 地址不合法");
        }
        appendLog("IP 地址不合法");
        return false;
    }

    int prefix = netmaskToPrefix(mask);
    if (prefix < 0) {
        if (showMessage) {
            QMessageBox::warning(this, "输入错误", "子网掩码不合法（例如 255.255.255.0）");
        }
        appendLog("子网掩码不合法");
        return false;
    }

    if (!gw.isEmpty() && !isIPv4(gw)) {
        if (showMessage) {
            QMessageBox::warning(this, "输入错误", "网关不合法");
        }
        appendLog("网关不合法");
        return false;
    }

    if (!dns.isEmpty() && !isIPv4(dns)) {
        if (showMessage) {
            QMessageBox::warning(this, "输入错误", "DNS 不合法");
        }
        appendLog("DNS 不合法");
        return false;
    }

    if (!runCmd("ip", {"addr", "flush", "dev", iface}, &err)) {
        if (showMessage) {
            QMessageBox::critical(this, "失败", "清理旧地址失败: " + err);
        }
        appendLog(QString("清理旧地址失败: %1").arg(err));
        return false;
    }

    const QString ipWithPrefix = QString("%1/%2").arg(ip).arg(prefix);
    if (!runCmd("ip", {"addr", "add", ipWithPrefix, "dev", iface}, &err)) {
        if (showMessage) {
            QMessageBox::critical(this, "失败", "设置IP失败: " + err);
        }
        appendLog(QString("设置IP失败: %1").arg(err));
        return false;
    }

    if (!gw.isEmpty()) {
        if (!runCmd("ip", {"route", "replace", "default", "via", gw, "dev", iface}, &err)) {
            if (showMessage) {
                QMessageBox::critical(this, "失败", "设置默认网关失败: " + err);
            }
            appendLog(QString("设置默认网关失败: %1").arg(err));
            return false;
        }
    }

    if (!dns.isEmpty()) {
        QFile f("/etc/resolv.conf");
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            if (showMessage) {
                QMessageBox::warning(this, "提示", "IP已设置，但写入 /etc/resolv.conf 失败（DNS可能未更新）");
            }
            appendLog("IP已设置，但写入 /etc/resolv.conf 失败（DNS可能未更新）");
        } else {
            QTextStream ts(&f);
            ts << "nameserver " << dns << "\n";
            f.close();
        }
    }

    refreshUiFromSystem(ui, iface);
    saveInputsToConfigFile(configDir_);

    if (showMessage) {
        QMessageBox::information(this, "成功", QString("静态 IP 已应用（%1）").arg(iface));
    }
    appendLog(QString("静态 IP 已应用（%1）").arg(iface));

    emit networkAppliedSuccessfully();
    return true;
}


/** @brief 向页面日志区追加文本。 */
void NetworkPage::appendLog(const QString &text)
{
    const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    qDebug().noquote() << QString("[NetworkPage][%1] %2").arg(ts, text);
}

/** @brief 从指定目录的 INI 文件回显网络输入。 */
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

/** @brief 根据已保存配置执行一次自动联网。 */
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


/** @brief 将当前输入保存到网络配置文件。 */
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


/** @brief 清除配置文件中的静态 IP 字段。 */
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

/** @brief 更新功能开关并决定网络功能是否可用。 */
void NetworkPage::setFeatureSettings(const FeatureSettings &settings)
{
    featureSettings_ = settings;

    qDebug() << "[D] setFeatureSettings:"
             << "offline_v1=" << featureSettings_.offline_v1
             << "online_v1=" << featureSettings_.online_v1
             << "online_v2=" << featureSettings_.online_v2;

    // offline_v1 启用，且网络功能未启用时，立即清理
    if (featureSettings_.offline_v1 && !isNetworkFeatureEnabled()) {
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

        refreshUiFromSystem(ui, iface);
    }
}

/** @return 任一在线功能开关要求网络时返回 true。 */
bool NetworkPage::isNetworkFeatureEnabled() const
{
    return featureSettings_.online_v1 || featureSettings_.online_v2;
}


// DHCP 运行时同步
void NetworkPage::syncDhcpRuntimeStateToConfig()
{
    if (!ui) return;

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

    // 先看物理链路是否存在
    const bool carrierUp = isIfaceCarrierUp(iface);
    const bool linkBecameUp = carrierUp && !lastCarrierUp_;
    const bool linkBecameDown = !carrierUp && lastCarrierUp_;

    lastCarrierUp_ = carrierUp;

    if (linkBecameUp) {
        appendLog(QString("检测到网线插入：%1 carrier=1").arg(iface));

        lastDhcpAutoTryMs_ = 0;
        requestAutoDhcpRenew("网线插入后自动获取DHCP", false);
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

    QString ip, mask;
    getIfaceIPv4AndMask(iface, &ip, &mask);

    const QString gw  = getDefaultGateway(iface);
    const QString dns = getDnsFromResolvConf();

    const bool hasValidIp = (!ip.isEmpty() && ip != "0.0.0.0" && !mask.isEmpty());

    // 链路存在但还没拿到有效 IP，也清空缓存
    if (!hasValidIp) {
        const bool dhcpRunning = isDhcpRunning(iface);
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


/** @brief 按重试节流请求 DHCP 续租或强制重启客户端。 */
void NetworkPage::requestAutoDhcpRenew(const QString &reason, bool forceRestart)
{
    if (!ui) {
        return;
    }

    if (dhcpAutoApplying_) {
        appendLog(QString("自动DHCP正在执行中，忽略重复请求：%1").arg(reason));
        return;
    }

    if (!isNetworkFeatureEnabled()) {
        return;
    }

    if (ui->connectionTypeCombo->currentIndex() != 0) {
        return;
    }

    if (ui->ipModeComboBox->currentIndex() != 0) {
        return;
    }

    const QString iface = ifaceName_.trimmed().isEmpty() ? QString("eth0") : ifaceName_;

    if (!isIfaceCarrierUp(iface)) {
        appendLog(QString("自动DHCP取消：%1 未检测到网线，reason=%2")
                  .arg(iface, reason));
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (!forceRestart &&
        lastDhcpAutoTryMs_ > 0 &&
        now - lastDhcpAutoTryMs_ < kDhcpAutoRetryIntervalMs) {
        appendLog(QString("自动DHCP重试间隔过短，暂不重复执行：%1").arg(reason));
        return;
    }

    dhcpAutoApplying_ = true;
    lastDhcpAutoTryMs_ = now;

    appendLog(QString("自动DHCP开始：iface=%1 forceRestart=%2 reason=%3")
              .arg(iface)
              .arg(forceRestart ? "true" : "false")
              .arg(reason));

    QString err;

    if (forceRestart || !isDhcpRunning(iface)) {
        stopDhcpClient(iface, nullptr);

        runCmd("ip", {"link", "set", iface, "up"}, &err, 3000);
        runCmd("ip", {"addr", "flush", "dev", iface}, nullptr, 3000);
        runCmd("ip", {"route", "flush", "dev", iface}, nullptr, 3000);

        clearNetworkConfigFileIp(configDir_);

        ui->ipAddressEdit->setText("-");
        ui->subnetMaskEdit->setText("-");
        ui->gatewayEdit->setText("-");
        ui->dnsEdit->setText("-");

        lastIp_.clear();
        lastMask_.clear();
        lastGateway_.clear();
        lastDns_.clear();
        dhcpHadValidIp_ = false;

        if (!startDhcpDaemon(iface, &err)) {
            appendLog(QString("自动DHCP启动失败：%1").arg(err));
            refreshUiFromSystem(ui, iface);
            dhcpAutoApplying_ = false;
            return;
        }

        appendLog(QString("自动DHCP已启动：%1，等待定时器同步IP/网关/DNS").arg(iface));
    } else {
        appendLog(QString("自动DHCP：%1 udhcpc 已运行，等待获取IP").arg(iface));
    }

    refreshUiFromSystem(ui, iface);

    dhcpAutoApplying_ = false;
}
