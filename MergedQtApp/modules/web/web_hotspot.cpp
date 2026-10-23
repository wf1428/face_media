/**
 * @file web_hotspot.cpp
 * @brief 发布 Qt 身份并通过固定辅助脚本读取或修改 NetworkManager 热点。
 * @author Du lin
 * @date 2026-10-16
 */

#include "web_hotspot.h"
#include "common/sql/web/web_sql.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QDebug>

namespace {

/**
 * @brief 从设备配置读取 Qt 已更新的 client_id，未就绪时返回业务错误。
 * @param path 待访问的文件或配置路径。
 * @return 配置中的设备标识。
 */
QString clientId(const QString &path)
{
    if (!QFileInfo(path).isFile())
        throw WebError(503, QStringLiteral("设备配置尚未就绪"));
    QSettings ini(path, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");
    const auto id = ini.value(QStringLiteral("mqtt/client_id")).toString().trimmed();
    if (ini.status() != QSettings::NoError || id.isEmpty())
        throw WebError(503, QStringLiteral("等待 Qt 更新设备号"));
    return id;
}

/**
 * @brief 按 UTF-8 字节数、首尾空白和控制字符校验热点名称。
 * @param ssid 热点名称，按 UTF-8 长度及字符规则校验。
 */
void validateSsid(const QString &ssid)
{
    if (ssid.isEmpty() || ssid != ssid.trimmed() || ssid.toUtf8().size() > 32)
        throw WebError(400, QStringLiteral("SSID 需为 1–32 字节，首尾不能有空白"));
    for (const QChar c : ssid)
        if (c.unicode() < 32 || c.unicode() == 127 || c.category() == QChar::Other_Control)
            throw WebError(400, QStringLiteral("SSID 不能包含换行或控制字符"));
}

/**
 * @brief 按 YC_设备号后五位_0000 生成并校验默认热点名称。
 * @param id 目标记录或任务的标识。
 * @return 设备默认热点名称。
 */
QString defaultSsid(const QString &id)
{
    const auto result = QStringLiteral("YC_%1_0000").arg(id.right(5));
    validateSsid(result);
    return result;
}

/**
 * @brief 限时启动固定的热点辅助脚本，避免网络命令无限等待。
 * @param process 受调用方管理的异步进程对象。
 * @param helper 固定的热点管理辅助程序路径。
 * @param arguments 传给固定辅助程序的独立命令参数。
 * @param seconds 辅助程序执行超时，单位为秒。
 */
void startHelper(QProcess *process,
                 const QString &helper,
                 const QStringList &arguments,
                 int seconds)
{
#ifdef Q_OS_LINUX
    // GNU timeout owns a process group, so an unresponsive nmcli child is also
    // terminated. A helper must never leave Qt's Web worker waiting indefinitely.
    process->start(QStringLiteral("timeout"),
                   QStringList{QStringLiteral("--kill-after=2"), QString::number(seconds), helper} +
                       arguments);
#else
    Q_UNUSED(seconds);
    process->start(helper, arguments);
#endif
}
} // namespace

/**
 * @brief 绑定热点配置和就绪文件，定时发布设备身份而不在启动时配置 WiFi。
 * @param networkConfig net_cfg.ini 路径。
 * @param helper 固定的热点管理辅助程序路径。
 * @param readyFile Qt Web 就绪标记文件路径。
 * @param parent Qt 父对象；对象销毁由父子关系管理，空指针表示无父对象。
 */
WebHotspot::WebHotspot(const QString &networkConfig,
                       const QString &helper,
                       const QString &readyFile,
                       QObject *parent)
    : QObject(parent), networkConfig_(networkConfig), helper_(helper), readyFile_(readyFile)
{
    auto *timer = new QTimer(this);
    const auto attempt = [this]() {
        try {
            synchronize();
            setProperty("waiting", false);
        } catch (const WebError &error) {
            if (!readyFile_.isEmpty())
                QFile::remove(readyFile_);
            if (!property("waiting").toBool())
                qWarning() << "[WEB] device identity waiting:" << error.message;
            setProperty("waiting", true);
        }
    };
    connect(timer, &QTimer::timeout, this, attempt);
    timer->start(1000);
    QTimer::singleShot(0, this, attempt);
}

/**
 * @brief 销毁身份发布对象时移除本进程的 Web 就绪标记。
 */
WebHotspot::~WebHotspot()
{
    if (!readyFile_.isEmpty())
        QFile::remove(readyFile_);
}

/**
 * @brief 将当前 Qt PID、设备号和默认 SSID 原子写入就绪文件。
 */
void WebHotspot::synchronize()
{
    const auto id = clientId(networkConfig_);
    const auto ready = QByteArray::number(QCoreApplication::applicationPid()) + '\n' + id.toUtf8() +
                       '\n' + defaultSsid(id).toUtf8() + '\n';
    if (readyFile_.isEmpty())
        return;
    QFile existing(readyFile_);
    const bool unchanged = existing.open(QIODevice::ReadOnly) && existing.readAll() == ready;
    existing.close();
    if (unchanged)
        return;
    QSaveFile file(readyFile_);
    if (!file.open(QIODevice::WriteOnly) || file.write(ready) != ready.size() || !file.commit())
        throw WebError(503, QStringLiteral("Web 就绪标记保存失败"));
}

/**
 * @brief 执行受控热点命令，限时等待并按需取得标准输出。
 * @param arguments 传给固定辅助程序的独立命令参数。
 * @param[out] output 可选输出，接收辅助程序标准输出。
 * @param input 通过标准输入提供的数据，避免敏感内容出现在命令参数中。
 * @return 固定辅助程序成功结束时为 true；失败或超时为 false。
 */
bool WebHotspot::command(const QStringList &arguments,
                         QString *output,
                         const QByteArray &input) const
{
    if (!QFileInfo(helper_).isExecutable())
        return false;
    QProcess process;
    startHelper(&process, helper_, arguments, 8);
    if (!process.waitForStarted(1000))
        return false;
    // 密码通过标准输入交给固定辅助程序，不放入 Qt 启动参数或日志。
    if (!input.isEmpty() && process.write(input) != input.size()) {
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    process.closeWriteChannel();
    if (!process.waitForFinished(11000)) {
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    if (output)
        *output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

/**
 * @brief 返回实际设备号、热点名称和待重启/更新错误状态。
 * @return 实际热点配置及辅助程序报告的运行状态。
 */
QJsonObject WebHotspot::info() const
{
    const auto id = clientId(networkConfig_);
    QString ssid;
    if (!command({QStringLiteral("ssid")}, &ssid) || ssid.isEmpty())
        ssid = defaultSsid(id);
    return {{"deviceId", id},
            {"deviceName", ssid},
            {"wifiName", ssid},
            {"restartPending", reloadPending_},
            {"wifiUpdateError", updateError_}};
}

/**
 * @brief 保存 SSID 和可选新密码，沿用延迟重新激活及错误回报流程。
 * @param ssid 热点名称，按 UTF-8 长度及字符规则校验。
 * @param password 新热点密码；空值表示保留原密码。
 * @return 保存后的热点状态；空密码保留原密码，重载异步执行。
 */
QJsonObject WebHotspot::save(const QString &ssid, const QString &password)
{
    validateSsid(ssid);
    // WPA-PSK 使用 8–63 位可打印 ASCII，或 64 位十六进制密钥。
    if (!password.isEmpty()) {
        bool ascii = password.size() >= 8 && password.size() <= 63;
        bool hex = password.size() == 64;
        for (const auto c : password) {
            ascii = ascii && c.unicode() >= 32 && c.unicode() <= 126;
            hex =
                hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
        }
        if (!ascii && !hex)
            throw WebError(
                400,
                QStringLiteral("热点密码需为8至63位英文、数字或符号，或64位十六进制密钥"));
    }
    clientId(networkConfig_);
    if (reloadPending_)
        throw WebError(409, QStringLiteral("热点正在更新，请重新连接后再修改"));
    QStringList arguments{QStringLiteral("save"), ssid};
    if (!password.isEmpty())
        arguments.append(QStringLiteral("--password-stdin"));
    if (!command(arguments, nullptr, password.isEmpty() ? QByteArray{} : password.toUtf8() + '\n'))
        throw WebError(503, QStringLiteral("NetworkManager 热点未就绪或配置保存失败"));
    updateError_.clear();
    reloadPending_ = true;

    // 先返回 SSID 保存结果，延迟重新激活连接，为手机接收响应留出时间。
    QTimer::singleShot(3000, this, [this]() {
        auto *process = new QProcess(this);
        connect(process,
                &QProcess::errorOccurred,
                this,
                [this, process](QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart)
                        return;
                    reloadPending_ = false;
                    updateError_ = QStringLiteral("热点更新程序无法启动，热点设置已保存");
                    qWarning() << "[WEB]" << updateError_;
                    process->deleteLater();
                });
        connect(process,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this,
                [this, process](int code, QProcess::ExitStatus status) {
                    reloadPending_ = false;
                    if (status != QProcess::NormalExit || code != 0) {
                        updateError_ =
                            QStringLiteral("热点重新激活失败，热点设置已保存，请检查服务日志");
                        qWarning() << "[WEB]" << updateError_ << process->readAllStandardError();
                    } else {
                        qInfo() << "[WEB] NetworkManager hotspot reactivated";
                    }
                    process->deleteLater();
                });
        startHelper(process, helper_, {QStringLiteral("request-refresh")}, 40);
        QTimer::singleShot(45000, process, [process]() {
            if (process->state() != QProcess::NotRunning)
                process->kill();
        });
    });
    return {{"deviceId", clientId(networkConfig_)},
            {"deviceName", ssid},
            {"wifiName", ssid},
            {"restartPending", true}};
}
