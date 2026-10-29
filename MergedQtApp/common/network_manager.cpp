/**
 * @file network_manager.cpp
 * @brief NM 有线连接命令、状态采样和旧版独立 DHCP 的安全迁移实现。
 * @author Du lin
 * @date 2026-10-26
 * @note 最近修改：2026-10-27；修改人：Du lin；增加千兆全双工与默认百兆模式，启动比较 NM 保存参数和实际速率，匹配时复用连接、不匹配时按 INI 请求重新协商，保留失败回读与恢复。
 */
#include "network_manager.h"
#include "network_config.h"
#include "platform/rk3566_platform.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QThread>
#ifdef Q_OS_LINUX
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace {
/** @brief 传播明确失败说明；error 可空，始终返回 false。 */
bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

/** @brief 只接受内核接口名，拒绝回环及命令/路径字符。 */
bool validIface(const QString &iface)
{
    return iface != "lo" && QRegularExpression("^[A-Za-z0-9_.-]{1,15}$").match(iface).hasMatch();
}

/** @brief 解码 nmcli 的反斜线转义，按未转义冒号分隔字段。 */
QStringList fields(const QString &line)
{
    QStringList result;
    QString part;
    bool escaped = false;
    for (const QChar ch : line) {
        if (escaped) {
            part += ch;
            escaped = false;
        } else if (ch == '\\')
            escaped = true;
        else if (ch == ':') {
            result << part;
            part.clear();
        } else
            part += ch;
    }
    if (escaped)
        part += '\\';
    result << part;
    return result;
}

/** @brief 解析 nmcli 属性键值；保留带序号地址/DNS字段，空占位 -- 归一为空文本。 */
QMap<QString, QString> properties(const QString &output)
{
    QMap<QString, QString> result;
    for (const QString &line : output.split('\n', QString::SkipEmptyParts)) {
        const auto parts = fields(line.trimmed());
        if (parts.size() >= 2)
            result[parts.first()] =
                parts.mid(1).join(':') == "--" ? QString() : parts.mid(1).join(':');
    }
    return result;
}

/** @brief 从 nmcli 命令结果生成带操作上下文的失败说明。 */
QString commandError(const ManagedNetwork::CommandResult &result, const QString &action)
{
    const QString detail = result.error.isEmpty() ? result.output : result.error;
    return action + (detail.isEmpty() ? QStringLiteral("失败，退出码 %1").arg(result.code)
                                      : QStringLiteral("失败：") + detail.trimmed());
}

/** @brief 比较布尔 NM 属性；nmcli 使用 yes/no 或 true/false。 */
bool yes(const QString &value)
{
    return value == "yes" || value == "true" || value == "1" || value == "on";
}

/** @brief 将 NM 的逗号分隔 IPv4/DNS 属性归一为空格列表，用于配置比较。 */
QString listValue(const QString &value)
{
    return value.split(QRegularExpression("[, ]+"), QString::SkipEmptyParts).join(' ');
}
} // namespace

bool ManagedNetwork::State::hasAddress() const
{
    return !ip.isEmpty() && ip != "0.0.0.0" && !mask.isEmpty();
}

bool ManagedNetwork::State::activating() const
{
    return deviceState >= 40 && deviceState < 100;
}

ManagedNetwork::Backend::Backend(Runner runner, Current current)
    : runner_(std::move(runner)), current_(std::move(current))
{
    budget_.start();
}

ManagedNetwork::CommandResult
ManagedNetwork::Backend::run(const QString &tool, const QStringList &args, int timeoutMs)
{
    if ((current_ && !current_()) || QThread::currentThread()->isInterruptionRequested())
        return {-1, {}, QStringLiteral("网络任务已取消")};
    const qint64 remaining = 50000 - budget_.elapsed();
    if (remaining <= 0)
        return {-1, {}, QStringLiteral("网络任务超过 50 秒总预算")};
    timeoutMs = qMin(timeoutMs, static_cast<int>(remaining));
    if (runner_)
        return runner_(tool, args, timeoutMs);
    const QString executable = Rk3566Platform::findExecutable({tool});
    if (executable.isEmpty())
        return {-1, {}, QStringLiteral("命令不存在：") + tool};
    QProcess process;
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("LC_ALL", "C");
    process.setProcessEnvironment(env);
    process.start(executable, args);
    if (!process.waitForStarted(1000))
        return {-1, {}, QStringLiteral("命令启动失败：") + tool};
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(500);
        return {-1, {}, QStringLiteral("命令超时：") + tool};
    }
    return {process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1,
            QString::fromUtf8(process.readAllStandardOutput()).trimmed(),
            QString::fromUtf8(process.readAllStandardError()).trimmed()};
}

ManagedNetwork::CommandResult ManagedNetwork::Backend::nm(const QStringList &args, int waitSeconds)
{
    return run(
        "nmcli",
        QStringList{"--colors", "no", "--escape", "yes", "--wait", QString::number(waitSeconds)} +
            args,
        qMax(4000, (waitSeconds + 1) * 1000));
}

bool ManagedNetwork::Backend::ensureService(QString *error)
{
    if (error)
        error->clear();
    const auto status = nm({"-t", "-f", "RUNNING", "general"});
    if (status.code == 0 && status.output == "running")
        return true;
    const auto started = run("systemctl", {"start", "NetworkManager.service"}, 20000);
    if (started.code != 0)
        return fail(error, commandError(started, QStringLiteral("启动 NetworkManager")));
    const auto ready = nm({"-t", "-f", "RUNNING", "general"});
    return (ready.code == 0 && ready.output == "running") ||
           fail(error, commandError(ready, QStringLiteral("等待 NetworkManager 就绪")));
}

ManagedNetwork::State ManagedNetwork::Backend::query(const QString &iface)
{
    State state;
    if (!validIface(iface)) {
        state.error = QStringLiteral("有线网卡名称无效");
        return state;
    }
    const auto result = nm({"-t",
                            "-f",
                            "GENERAL.TYPE,GENERAL.STATE,GENERAL.CON-UUID,WIRED-PROPERTIES.CARRIER,"
                            "IP4.ADDRESS,IP4.GATEWAY,IP4.DNS",
                            "device",
                            "show",
                            iface});
    if (result.code != 0) {
        state.error = commandError(result, QStringLiteral("读取 NM 接口状态"));
        return state;
    }
    const auto values = properties(result.output);
    bool ok = false;
    state.deviceState = values.value("GENERAL.STATE").section(' ', 0, 0).toInt(&ok);
    state.type = values.value("GENERAL.TYPE");
    if (!ok || state.type.isEmpty()) {
        state.error = QStringLiteral("NM 返回的接口状态不完整");
        return state;
    }
    state.available = true;
    state.carrierUp = yes(values.value("WIRED-PROPERTIES.CARRIER"));
    state.uuid = values.value("GENERAL.CON-UUID");
    state.gateway = values.value("IP4.GATEWAY");
    QStringList dns;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (it.key().startsWith("IP4.ADDRESS") && state.ip.isEmpty()) {
            const auto cidr = QHostAddress::parseSubnet(it.value());
            if (cidr.first.protocol() == QAbstractSocket::IPv4Protocol && cidr.second >= 0 &&
                !cidr.first.isNull() && !cidr.first.isLinkLocal()) {
                state.ip = it.value().section('/', 0, 0);
                const quint32 mask = cidr.second == 0 ? 0 : 0xffffffffU << (32 - cidr.second);
                state.mask = QHostAddress(mask).toString();
            }
        }
        if (it.key().startsWith("IP4.DNS")) {
            const QHostAddress address(it.value());
            if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isNull())
                dns << it.value();
        }
    }
    dns.removeDuplicates();
    state.dns = dns.join(' ');
    return state;
}

bool ManagedNetwork::Backend::retireLegacyDhcp(const QString &iface, QString *error)
{
#ifdef Q_OS_LINUX
    // 只迁移旧版本 Qt 创建的两个已知 PID 文件，不按进程名杀系统或 NM 的 DHCP。
    const QStringList paths{QString("/run/qt_ycest/dhcp.%1.pid").arg(iface),
                            QDir::temp().filePath(QString("qt_ycest.dhcp.%1.pid").arg(iface))};
    for (const QString &path : paths) {
        QFile pidFile(path);
        if (!pidFile.open(QIODevice::ReadOnly))
            continue;
        bool ok = false;
        const qint64 number = pidFile.readAll().trimmed().toLongLong(&ok);
        pidFile.close();
        if (!ok || number <= 1 || number > 2147483647)
            return fail(error, QStringLiteral("旧 DHCP PID 文件无效：") + path);
        const pid_t pid = static_cast<pid_t>(number);
        QFile cmdline(QString("/proc/%1/cmdline").arg(number));
        if (!cmdline.open(QIODevice::ReadOnly)) {
            if (::kill(pid, 0) == -1 && errno == ESRCH) {
                QFile::remove(path);
                continue;
            }
            return fail(error, QStringLiteral("无法核实旧 DHCP 进程：") + path);
        }
        const auto args = cmdline.readAll().split('\0');
        const QString program = QFileInfo(QString::fromLocal8Bit(args.value(0))).fileName();
        const int pidOption =
            args.indexOf(program == "udhcpc" ? QByteArray("-p") : QByteArray("-pf"));
        const int ifaceOption = args.indexOf("-i");
        const bool sameIface =
            program == "udhcpc"
                ? ifaceOption >= 0 && args.value(ifaceOption + 1) == iface.toLocal8Bit()
                : args.contains(iface.toLocal8Bit());
        if ((program != "udhcpc" && program != "dhclient") || !sameIface || pidOption < 0 ||
            args.value(pidOption + 1) != path.toLocal8Bit())
            return fail(error, QStringLiteral("旧 PID 已被其他进程使用，未终止：") + path);
        if (::kill(pid, SIGTERM) != 0 && errno != ESRCH)
            return fail(error, QStringLiteral("停止旧 DHCP 失败"));
        QElapsedTimer timer;
        timer.start();
        while (::kill(pid, 0) == 0 && timer.elapsed() < 2000)
            QThread::msleep(50);
        if (::kill(pid, 0) == 0)
            return fail(error, QStringLiteral("旧 DHCP 尚未退出，暂不启用 NM，避免双重管理"));
        QFile::remove(path);
    }
#else
    Q_UNUSED(iface);
    Q_UNUSED(error);
#endif
    return true;
}

bool ManagedNetwork::Backend::claim(const QString &iface, QString *error)
{
    const State state = query(iface);
    if (!state.available)
        return fail(error, state.error);
    if (state.type != "ethernet")
        return fail(error, QStringLiteral("仅允许配置有线接口，不能修改热点或无线接口"));
    if (!retireLegacyDhcp(iface, error))
        return false;
    const auto result = nm({"device", "set", iface, "autoconnect", "no", "managed", "yes"});
    return result.code == 0 ||
           fail(error, commandError(result, QStringLiteral("取得有线接口管理权")));
}

QString ManagedNetwork::Backend::profile(const QString &iface, QString *error)
{
    const QString name = "yc-wired-" + iface;
    const auto list = nm({"-t", "-f", "UUID,NAME,TYPE", "connection", "show"});
    if (list.code != 0) {
        fail(error, commandError(list, QStringLiteral("读取 NM 连接列表")));
        return {};
    }
    QString selected;
    for (const QString &line : list.output.split('\n', QString::SkipEmptyParts)) {
        const auto row = fields(line);
        if (row.size() != 3)
            continue;
        if (row[2] != "802-3-ethernet" && row[2] != "ethernet") {
            if (row[1] == name) {
                fail(error, QStringLiteral("项目连接名称已被非有线配置占用"));
                return {};
            }
            continue;
        }
        const auto detail = nm({"-g",
                                "connection.interface-name,connection.autoconnect",
                                "connection",
                                "show",
                                "uuid",
                                row[0]});
        if (detail.code != 0) {
            fail(error, commandError(detail, QStringLiteral("检查 NM 有线连接")));
            return {};
        }
        const auto details = detail.output.split('\n');
        if (details.value(0).trimmed() != iface) {
            if (row[1] == name) {
                fail(error, QStringLiteral("项目连接名称已绑定其他接口，未修改该连接"));
                return {};
            }
            continue;
        }
        if (row[1] == name && selected.isEmpty())
            selected = row[0];
        // 只有自动激活尚未关闭时才写配置，避免启动时对相同连接反复落盘。
        if (yes(details.value(1).trimmed())) {
            const auto disabled =
                nm({"connection", "modify", "uuid", row[0], "connection.autoconnect", "no"});
            if (disabled.code != 0) {
                fail(error, commandError(disabled, QStringLiteral("关闭重复有线连接自动激活")));
                return {};
            }
        }
    }
    if (!selected.isEmpty())
        return selected;
    const auto added = nm({"connection",
                           "add",
                           "type",
                           "ethernet",
                           "ifname",
                           iface,
                           "con-name",
                           name,
                           "connection.autoconnect",
                           "no",
                           "ipv4.method",
                           "disabled",
                           "ipv6.method",
                           "ignore"});
    if (added.code != 0) {
        fail(error, commandError(added, QStringLiteral("创建 NM 有线配置")));
        return {};
    }
    const auto uuid = nm({"-g", "connection.uuid", "connection", "show", "id", name});
    if (uuid.code != 0 || uuid.output.isEmpty() || uuid.output.contains('\n')) {
        fail(error, commandError(uuid, QStringLiteral("读取项目有线连接 UUID")));
        return {};
    }
    return uuid.output;
}

/**
 * @brief 检查 ethtool 回读是否达到指定全双工协商速率。
 * @param output C 语言环境下的 ethtool 标准输出。
 * @param speed 目标速率文本，单位 Mb/s，为 100 或 1000。
 * @return 速率、Full 双工和实际载波均匹配时为 true。
 */
static bool reportedLinkMatches(const QString &output, const QString &speed)
{
    return QRegularExpression("Speed:\\s*" + speed + "Mb/s").match(output).hasMatch() &&
           QRegularExpression("Duplex:\\s*Full").match(output).hasMatch() &&
           QRegularExpression("Link detected:\\s*yes").match(output).hasMatch();
}

bool ManagedNetwork::Backend::verifyLinkMode(const QString &iface,
                                             const QString &mode,
                                             QString *error)
{
    if (mode == "auto")
        return true;
    const QString speed = mode == "1000-full" ? "1000" : "100";
    QElapsedTimer timer;
    timer.start();
    do {
        const auto result = run("ethtool", {iface}, 2500);
        if (result.code != 0)
            return fail(error, commandError(result, QStringLiteral("回读网卡速率")));
        if (reportedLinkMatches(result.output, speed))
            return true;
        QThread::msleep(200);
    } while (timer.elapsed() < 8000 && (!current_ || current_()));
    return fail(error,
                QStringLiteral("已提交 %1Mb/s 配置，但未确认全双工链路，请检查网线、驱动和交换机")
                    .arg(speed));
}

bool ManagedNetwork::Backend::apply(const QJsonObject &config, State *state, QString *error)
{
    if (error)
        error->clear();
    const QString iface = config.value("iface").toString();
    const QString mode = config.value("linkMode").toString("100-full");
    if (!state || !validIface(iface) || !NetworkConfig::isLinkModeValid(mode))
        return fail(error, QStringLiteral("网卡或链路速率配置无效"));
    if (!ensureService(error) || !claim(iface, error))
        return false;
    *state = query(iface);
    const bool dhcp = config.value("dhcp").toBool();
    if (!state->available)
        return fail(error, state->error);
    if (dhcp && !state->carrierUp)
        return fail(error, QStringLiteral("网线未连接，无法获取 DHCP 地址"));
    const QString uuid = profile(iface, error);
    if (uuid.isEmpty())
        return false;
    const auto current =
        nm({"-t",
            "-f",
            "ipv4.method,ipv4.addresses,ipv4.gateway,ipv4.dns,ipv4.ignore-auto-dns,802-3-ethernet."
            "auto-negotiate,802-3-ethernet.speed,802-3-ethernet.duplex",
            "connection",
            "show",
            "uuid",
            uuid});
    if (current.code != 0)
        return fail(error, commandError(current, QStringLiteral("读取当前有线参数")));
    const auto saved = properties(current.output);
    const QString method = dhcp ? "auto" : "manual";
    const auto subnet = QHostAddress::parseSubnet(config.value("ip").toString() + "/" +
                                                  config.value("netmask").toString());
    const QString address =
        dhcp ? QString() : config.value("ip").toString() + "/" + QString::number(subnet.second);
    if (!dhcp && subnet.second < 0)
        return fail(error, QStringLiteral("静态 IP 或子网掩码无效"));
    const QString gateway = dhcp ? QString() : config.value("gateway").toString();
    const QString dns = dhcp ? QString() : config.value("dns").toString();
    const bool forced = mode != "auto";
    const QString speed = mode == "1000-full" ? "1000" : (forced ? "100" : "0");
    const bool same = saved.value("ipv4.method") == method &&
                      listValue(saved.value("ipv4.addresses")) == address &&
                      saved.value("ipv4.gateway") == gateway &&
                      listValue(saved.value("ipv4.dns")) == dns &&
                      yes(saved.value("ipv4.ignore-auto-dns")) == !dhcp &&
                      yes(saved.value("802-3-ethernet.auto-negotiate")) &&
                      saved.value("802-3-ethernet.speed") == speed &&
                      saved.value("802-3-ethernet.duplex") == (forced ? "full" : "");
    if (!same) {
        const auto changed = nm({"connection",
                                 "modify",
                                 "uuid",
                                 uuid,
                                 "ipv4.method",
                                 method,
                                 "ipv4.addresses",
                                 address,
                                 "ipv4.gateway",
                                 gateway,
                                 "ipv4.dns",
                                 dns,
                                 "ipv4.ignore-auto-dns",
                                 dhcp ? "no" : "yes",
                                 "ipv4.may-fail",
                                 "no",
                                 "connection.autoconnect",
                                 "no",
                                 "802-3-ethernet.auto-negotiate",
                                 "yes",
                                 "802-3-ethernet.speed",
                                 speed,
                                 "802-3-ethernet.duplex",
                                 forced ? "full" : ""});
        if (changed.code != 0)
            return fail(error, commandError(changed, QStringLiteral("保存 NM 有线配置")));
    }
    // 相同配置的租约及申请过程直接接管，Qt 重启/重复 apply 不产生断链。
    bool reuse = same && state->uuid == uuid && (state->hasAddress() || state->activating());
    if (reuse && forced && state->deviceState == 100 && state->carrierUp) {
        // 即使 NM 文件相同，也核对实际链路，修正外部 ethtool 改动造成的运行态偏差。
        const auto actual = run("ethtool", {iface}, 2500);
        if (actual.code != 0)
            return fail(error, commandError(actual, QStringLiteral("读取当前协商速率")));
        reuse = reportedLinkMatches(actual.output, speed);
    }
    if (!reuse) {
        const auto activated = nm({"connection", "up", "uuid", uuid, "ifname", iface}, 0);
        if (activated.code != 0)
            return fail(error, commandError(activated, QStringLiteral("激活 NM 有线连接")));
    }
    QElapsedTimer timer;
    timer.start();
    do {
        *state = query(iface);
        if (!state->available)
            return fail(error, state->error);
        if (state->uuid == uuid && state->hasAddress() && state->deviceState == 100)
            break;
        if (state->deviceState == 120)
            return fail(error,
                        QStringLiteral("NetworkManager 激活失败，请检查地址冲突和 DHCP 服务"));
        QThread::msleep(100);
    } while (timer.elapsed() < 3000 && (!current_ || current_()));
    if (!dhcp && (state->uuid != uuid || state->ip != config.value("ip").toString() ||
                  state->mask != config.value("netmask").toString() || state->gateway != gateway))
        return fail(error, QStringLiteral("NM 实际静态地址或网关未达到请求配置"));
    if (state->uuid != uuid) {
        state->ip.clear();
        state->mask.clear();
        state->gateway.clear();
        state->dns.clear();
        return fail(error, QStringLiteral("项目连接尚未成为目标接口的活动连接，请刷新状态"));
    }
    if (forced && state->carrierUp && !verifyLinkMode(iface, mode, error)) {
        const QString reason = error ? *error : QStringLiteral("链路速率回读失败");
        // 只有本次改变了速率时才恢复旧模式；任务取消后 run() 会阻止回滚命令。
        const bool speedChanged = saved.value("802-3-ethernet.speed") != speed ||
                                  saved.value("802-3-ethernet.duplex") != "full";
        if (!same && speedChanged) {
            const auto restored =
                nm({"connection",
                    "modify",
                    "uuid",
                    uuid,
                    "802-3-ethernet.auto-negotiate",
                    yes(saved.value("802-3-ethernet.auto-negotiate")) ? "yes" : "no",
                    "802-3-ethernet.speed",
                    saved.value("802-3-ethernet.speed", "0"),
                    "802-3-ethernet.duplex",
                    saved.value("802-3-ethernet.duplex")});
            const auto reapplied = restored.code == 0
                                       ? nm({"connection", "up", "uuid", uuid, "ifname", iface}, 0)
                                       : restored;
            return fail(error,
                        reason + (reapplied.code == 0
                                      ? QStringLiteral("；已提交恢复原速率模式，请回读实际状态")
                                      : QStringLiteral("；恢复原速率模式失败：") +
                                            commandError(reapplied, QStringLiteral("恢复连接"))));
        }
        return false;
    }
    return true;
}

bool ManagedNetwork::Backend::disconnect(const QString &iface, QString *error)
{
    if (error)
        error->clear();
    if (!ensureService(error) || !claim(iface, error))
        return false;
    const State state = query(iface);
    if (!state.available)
        return fail(error, state.error);
    const QString uuid = profile(iface, error);
    if (uuid.isEmpty())
        return false;
    if (state.uuid.isEmpty() && state.deviceState <= 30)
        return true;
    const auto result = nm({"device", "disconnect", iface}, 5);
    return result.code == 0 || fail(error, commandError(result, QStringLiteral("断开有线连接")));
}
