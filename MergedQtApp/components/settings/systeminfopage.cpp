/**
 * @file systeminfopage.cpp
 * @brief 展示 RK3566 硬件、软件、网络和运行时长的系统信息页面的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "systeminfopage.h"

#include <algorithm>

#include <QAbstractSocket>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHideEvent>
#include <QHostAddress>
#include <QHBoxLayout>
#include <QLabel>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QShowEvent>
#include <QStorageInfo>
#include <QSysInfo>
#include <QTimer>
#include <QVBoxLayout>

namespace {

/** @brief 读取并裁剪系统文本节点，读取失败时返回空字符串。 */
QString readTextFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }

    QByteArray data = file.readAll();
    data.replace('\0', ' ');
    return QString::fromUtf8(data).simplified();
}

/** @brief 按候选路径顺序读取第一个存在且非空的系统信息节点。 */
QString readFirstExistingText(const QStringList &paths)
{
    for (const QString &path : paths) {
        const QString value = readTextFile(path);
        if (!value.isEmpty()) {
            return value;
        }
    }
    return QString();
}

/** @brief 从 /proc/meminfo 读取指定字段并换算为字节。 */
quint64 memInfoBytes(const QString &key)
{
    QFile file(QStringLiteral("/proc/meminfo"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 0;
    }

    while (!file.atEnd()) {
        const QString line = QString::fromLatin1(file.readLine());
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0
                || line.left(colon).trimmed().compare(key, Qt::CaseInsensitive) != 0) {
            continue;
        }

        const QStringList fields = line.mid(colon + 1).trimmed().split(
                    QRegularExpression(QStringLiteral("\\s+")), QString::SkipEmptyParts);
        if (fields.isEmpty()) {
            return 0;
        }

        bool ok = false;
        quint64 value = fields.first().toULongLong(&ok);
        if (!ok) {
            return 0;
        }

        const QString unit = fields.value(1).toLower();
        if (unit == QStringLiteral("kb")) {
            value *= 1024ULL;
        } else if (unit == QStringLiteral("mb")) {
            value *= 1024ULL * 1024ULL;
        } else if (unit == QStringLiteral("gb")) {
            value *= 1024ULL * 1024ULL * 1024ULL;
        }
        return value;
    }

    return 0;
}

/** @brief 执行 free 命令并返回输出，供内存信息回退解析。 */
bool runFreeCommand(const QString &program, const QStringList &arguments,
                    quint64 unitMultiplier,
                    quint64 *totalBytes, quint64 *usedBytes)
{
    QProcess process;
    process.start(program, arguments, QIODevice::ReadOnly);
    if (!process.waitForStarted(1000) || !process.waitForFinished(1500)) {
        process.kill();
        process.waitForFinished(200);
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return false;
    }

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QStringList lines = output.split(QLatin1Char('\n'), QString::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList fields = line.simplified().split(
                    QLatin1Char(' '), QString::SkipEmptyParts);
        QList<quint64> numbers;
        for (const QString &field : fields) {
            bool ok = false;
            const quint64 number = field.toULongLong(&ok);
            if (ok) {
                numbers.append(number);
            }
        }

        // free 的第一条数值行是物理内存：total、used、free……
        if (numbers.size() >= 3 && numbers.at(0) > 0) {
            *totalBytes = numbers.at(0) * unitMultiplier;
            *usedBytes = qMin(numbers.at(1), numbers.at(0)) * unitMultiplier;
            return true;
        }
    }

    return false;
}

/** @brief 从 free 输出解析内存总量和可用量。 */
bool memoryUsageFromFree(quint64 *totalBytes, quint64 *usedBytes)
{
    // GUI 进程的 PATH 可能与终端不同，先尝试设备上的常见绝对路径。
    const QStringList programs = {
        QStringLiteral("/usr/bin/free"),
        QStringLiteral("/bin/free"),
        QStringLiteral("free")
    };
    for (const QString &program : programs) {
        // procps-ng 支持 -b；部分 BusyBox 只支持 -k，因此依次回退。
        if (runFreeCommand(program, {QStringLiteral("-b")},
                           1ULL, totalBytes, usedBytes)) {
            return true;
        }
        if (runFreeCommand(program, {QStringLiteral("-k")},
                           1024ULL, totalBytes, usedBytes)) {
            return true;
        }
        if (runFreeCommand(program, QStringList(),
                           1024ULL, totalBytes, usedBytes)) {
            return true;
        }
    }
    return false;
}

/** @brief 将字节数格式化为适合显示的容量单位。 */
QString formatBytes(quint64 bytes)
{
    static const quint64 kKiB = 1024ULL;
    static const quint64 kMiB = kKiB * 1024ULL;
    static const quint64 kGiB = kMiB * 1024ULL;

    if (bytes >= kGiB) {
        return QStringLiteral("%1 GiB").arg(bytes / static_cast<double>(kGiB), 0, 'f', 2);
    }
    if (bytes >= kMiB) {
        return QStringLiteral("%1 MiB").arg(bytes / static_cast<double>(kMiB), 0, 'f', 1);
    }
    if (bytes >= kKiB) {
        return QStringLiteral("%1 KiB").arg(bytes / static_cast<double>(kKiB), 0, 'f', 1);
    }
    return QStringLiteral("%1 B").arg(bytes);
}

/** @brief 汇总应用数据所在文件系统的容量、已用量和使用率。 */
QString storageSummary()
{
    QStorageInfo storage(QStringLiteral("/"));
    storage.refresh();
    if (!storage.isValid() || !storage.isReady() || storage.bytesTotal() <= 0) {
        return QStringLiteral("无法读取");
    }

    const quint64 total = static_cast<quint64>(storage.bytesTotal());
    const quint64 available = static_cast<quint64>(storage.bytesAvailable());
    const quint64 used = total >= available ? total - available : 0;
    return QStringLiteral("已用 %1 / 总计 %2")
            .arg(formatBytes(used), formatBytes(total));
}

/** @brief 从 os-release 读取系统发行版显示名称。 */
QString osPrettyName()
{
    QFile file(QStringLiteral("/etc/os-release"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!file.atEnd()) {
            QString line = QString::fromUtf8(file.readLine()).trimmed();
            if (!line.startsWith(QStringLiteral("PRETTY_NAME="))) {
                continue;
            }

            QString value = line.mid(QStringLiteral("PRETTY_NAME=").size()).trimmed();
            if (value.size() >= 2
                    && ((value.startsWith(QLatin1Char('"'))
                         && value.endsWith(QLatin1Char('"')))
                        || (value.startsWith(QLatin1Char('\''))
                            && value.endsWith(QLatin1Char('\''))))) {
                value = value.mid(1, value.size() - 2);
            }
            if (!value.isEmpty()) {
                return value;
            }
        }
    }

    const QString product = QSysInfo::prettyProductName().trimmed();
    return product.isEmpty() ? QStringLiteral("Linux") : product;
}

/** @brief 读取系统运行秒数并格式化为天、时、分。 */
QString systemUptimeText()
{
    QFile file(QStringLiteral("/proc/uptime"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QStringLiteral("无法读取");
    }

    const QByteArray secondsField = file.readLine().simplified().split(' ').value(0);
    bool ok = false;
    qint64 totalSeconds = static_cast<qint64>(secondsField.toDouble(&ok));
    if (!ok || totalSeconds < 0) {
        return QStringLiteral("无法读取");
    }

    static const qint64 kSecondsPerDay = 24 * 60 * 60;
    qint64 days = totalSeconds / kSecondsPerDay;
    const qint64 years = days / 365;
    days %= 365;
    totalSeconds %= kSecondsPerDay;

    const qint64 hours = totalSeconds / (60 * 60);
    totalSeconds %= 60 * 60;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;

    const QString dayAndTime = QStringLiteral("%1天%2时%3分%4秒")
            .arg(days).arg(hours).arg(minutes).arg(seconds);
    return years > 0
            ? QStringLiteral("%1年%2").arg(years).arg(dayAndTime)
            : dayAndTime;
}

/** @brief 为网络接口分配显示优先级，优先展示有线和无线主接口。 */
int interfacePriority(const QNetworkInterface &iface)
{
    const QString name = iface.name().toLower();
    if (name == QStringLiteral("eth0")) {
        return 0;
    }
    if (name.startsWith(QStringLiteral("eth"))
            || name.startsWith(QStringLiteral("en"))) {
        return 1;
    }
    if (name.startsWith(QStringLiteral("wlan"))
            || name.startsWith(QStringLiteral("wl"))) {
        return 2;
    }
    return 3;
}

/** @brief 过滤回环和无效接口，并按业务优先级返回可展示网卡。 */
QList<QNetworkInterface> usableInterfaces()
{
    QList<QNetworkInterface> result;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (flags.testFlag(QNetworkInterface::IsLoopBack)
                || !flags.testFlag(QNetworkInterface::IsUp)) {
            continue;
        }

        const QString mac = iface.hardwareAddress().trimmed();
        bool hasIpv4 = false;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol
                    && !entry.ip().isLoopback()) {
                hasIpv4 = true;
                break;
            }
        }

        if (!mac.isEmpty() || hasIpv4) {
            result.append(iface);
        }
    }

    std::sort(result.begin(), result.end(),
              [](const QNetworkInterface &left, const QNetworkInterface &right) {
        return interfacePriority(left) < interfacePriority(right);
    });
    return result;
}

/** @brief 创建系统信息页面统一样式的字段标签。 */
QLabel *createFieldLabel(const QString &text, QWidget *parent)
{
    QLabel *label = new QLabel(QStringLiteral("%1：").arg(text), parent);
    label->setMinimumWidth(124);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setStyleSheet(QStringLiteral(
        "QLabel {"
        " color: #409eff;"
        " font-weight: 600;"
        " background-color: #ecf5ff;"
        " border-left: 4px solid #409eff;"
        " border-radius: 4px;"
        " padding: 5px 8px;"
        "}"));
    return label;
}

/** @brief 为系统信息分组框应用统一标题和边框样式。 */
void styleGroupBox(QGroupBox *groupBox)
{
    groupBox->setStyleSheet(QStringLiteral(
        "QGroupBox {"
        " color: #303133;"
        " font-weight: 600;"
        " background-color: #ffffff;"
        " border: 1px solid #dcdfe6;"
        " border-radius: 8px;"
        " margin-top: 12px;"
        " padding-top: 12px;"
        "}"
        "QGroupBox::title {"
        " subcontrol-origin: margin;"
        " left: 14px;"
        " padding: 0 6px;"
        " color: #409eff;"
        "}"));
}

/** @brief 设置系统信息值标签的换行、选择和最小高度属性。 */
void prepareValueLabel(QLabel *label)
{
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setStyleSheet(QStringLiteral("color: #606266; padding: 3px 0;"));
}

} // namespace

/** @brief 创建信息分组和运行时长定时器。 */
SystemInfoPage::SystemInfoPage(QWidget *parent)
    : QWidget(parent)
{
    setupUI();

    uptimeTimer = new QTimer(this);
    uptimeTimer->setInterval(1000);
    connect(uptimeTimer, &QTimer::timeout, this, &SystemInfoPage::updateUptime);

    updateSystemInfo();
}

/** @brief 停止并释放定时器。 */
SystemInfoPage::~SystemInfoPage()
{}

/** @brief 创建硬件、软件和网络信息 UI。 */
void SystemInfoPage::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 18, 24, 18);
    mainLayout->setSpacing(10);

    titleLabel = new QLabel(tr("系统信息"), this);
    titleLabel->setStyleSheet(QStringLiteral(
        "font-size: 22px; font-weight: bold; color: #303133; margin-bottom: 6px;"));
    mainLayout->addWidget(titleLabel);

    hardwareGroupBox = new QGroupBox(tr("硬件信息"), this);
    styleGroupBox(hardwareGroupBox);
    QFormLayout *hardwareLayout = new QFormLayout(hardwareGroupBox);
    hardwareLayout->setHorizontalSpacing(14);
    hardwareLayout->setVerticalSpacing(7);

    cpuInfoLabel = new QLabel(hardwareGroupBox);
    gpuInfoLabel = new QLabel(hardwareGroupBox);
    temperatureLabel = new QLabel(hardwareGroupBox);
    serialNumberLabel = new QLabel(hardwareGroupBox);
    memoryInfoLabel = new QLabel(hardwareGroupBox);
    storageInfoLabel = new QLabel(hardwareGroupBox);

    hardwareLayout->addRow(createFieldLabel(tr("CPU"), hardwareGroupBox), cpuInfoLabel);
    hardwareLayout->addRow(createFieldLabel(tr("GPU"), hardwareGroupBox), gpuInfoLabel);
    hardwareLayout->addRow(createFieldLabel(tr("温度"), hardwareGroupBox), temperatureLabel);
    hardwareLayout->addRow(createFieldLabel(tr("序列号"), hardwareGroupBox), serialNumberLabel);
    hardwareLayout->addRow(createFieldLabel(tr("运行内存"), hardwareGroupBox), memoryInfoLabel);
    hardwareLayout->addRow(createFieldLabel(tr("存储"), hardwareGroupBox), storageInfoLabel);
    mainLayout->addWidget(hardwareGroupBox);

    softwareGroupBox = new QGroupBox(tr("软件信息"), this);
    styleGroupBox(softwareGroupBox);
    QFormLayout *softwareLayout = new QFormLayout(softwareGroupBox);
    softwareLayout->setHorizontalSpacing(14);
    softwareLayout->setVerticalSpacing(7);

    appVersionLabel = new QLabel(softwareGroupBox);
    systemUptimeLabel = new QLabel(softwareGroupBox);
    osVersionLabel = new QLabel(softwareGroupBox);
    kernelVersionLabel = new QLabel(softwareGroupBox);

    softwareLayout->addRow(createFieldLabel(tr("应用版本"), softwareGroupBox), appVersionLabel);
    softwareLayout->addRow(createFieldLabel(tr("系统运行"), softwareGroupBox),
                           systemUptimeLabel);
    softwareLayout->addRow(createFieldLabel(tr("操作系统"), softwareGroupBox), osVersionLabel);
    softwareLayout->addRow(createFieldLabel(tr("内核版本"), softwareGroupBox),
                           kernelVersionLabel);
    mainLayout->addWidget(softwareGroupBox);

    networkGroupBox = new QGroupBox(tr("网络信息"), this);
    styleGroupBox(networkGroupBox);
    QFormLayout *networkLayout = new QFormLayout(networkGroupBox);
    networkLayout->setHorizontalSpacing(14);
    networkLayout->setVerticalSpacing(7);

    macAddressLabel = new QLabel(networkGroupBox);
    ipAddressLabel = new QLabel(networkGroupBox);
    networkLayout->addRow(createFieldLabel(tr("MAC地址"), networkGroupBox), macAddressLabel);
    networkLayout->addRow(createFieldLabel(tr("IP地址"), networkGroupBox), ipAddressLabel);
    mainLayout->addWidget(networkGroupBox);

    for (QLabel *label : {cpuInfoLabel, gpuInfoLabel, temperatureLabel,
                          serialNumberLabel, memoryInfoLabel, storageInfoLabel,
                          appVersionLabel, systemUptimeLabel, osVersionLabel,
                          kernelVersionLabel, macAddressLabel, ipAddressLabel}) {
        prepareValueLabel(label);
    }

    refreshButton = new QPushButton(tr("刷新信息"), this);
    refreshButton->setMinimumWidth(120);
    refreshButton->setStyleSheet(QStringLiteral(
        "QPushButton {"
        " background-color: #409eff;"
        " color: white;"
        " border: none;"
        " border-radius: 4px;"
        " padding: 8px 16px;"
        "}"
        "QPushButton:hover { background-color: #66b1ff; }"
        "QPushButton:pressed { background-color: #3a8ee6; }"));

    QHBoxLayout *buttonLayout = new QHBoxLayout;
    buttonLayout->addStretch();
    buttonLayout->addWidget(refreshButton);
    mainLayout->addLayout(buttonLayout);

    connect(refreshButton, &QPushButton::clicked,
            this, &SystemInfoPage::onRefreshButtonClicked);
}

/** @brief 读取 CPU、温度、内存、存储、版本和网络信息。 */
void SystemInfoPage::updateSystemInfo()
{
    cpuInfoLabel->setText(QStringLiteral("Quad-Core ARM Cortex-A55 1.8GHz"));
    gpuInfoLabel->setText(QStringLiteral("ARM Mali-G52 2EE"));

    const QString temperatureText = readFirstExistingText({
        QStringLiteral("/sys/class/thermal/thermal_zone0/temp"),
        QStringLiteral("/sys/class/thermal/thermal_zone1/temp")
    });
    bool temperatureOk = false;
    double temperature = temperatureText.toDouble(&temperatureOk);
    if (temperatureOk) {
        if (temperature > 1000.0) {
            temperature /= 1000.0;
        }
        if (temperature >= -40.0 && temperature <= 150.0) {
            temperatureLabel->setText(
                        QStringLiteral("%1 °C").arg(temperature, 0, 'f', 1));
        } else {
            temperatureOk = false;
        }
    }
    if (!temperatureOk) {
        temperatureLabel->setText(QStringLiteral("无法读取"));
    }

    QString serialNumber = readFirstExistingText({
        QStringLiteral("/sys/firmware/devicetree/base/serial-number"),
        QStringLiteral("/proc/device-tree/serial-number"),
        QStringLiteral("/etc/machine-id")
    });
    if (serialNumber.isEmpty()) {
        serialNumber = QStringLiteral("无法读取");
    }
    serialNumberLabel->setText(serialNumber);

    quint64 memoryTotal = 0;
    quint64 memoryUsed = 0;
    if (!memoryUsageFromFree(&memoryTotal, &memoryUsed)) {
        memoryTotal = memInfoBytes(QStringLiteral("MemTotal"));
        quint64 memoryAvailable = memInfoBytes(QStringLiteral("MemAvailable"));
        if (memoryAvailable == 0) {
            memoryAvailable = memInfoBytes(QStringLiteral("MemFree"))
                    + memInfoBytes(QStringLiteral("Buffers"))
                    + memInfoBytes(QStringLiteral("Cached"))
                    + memInfoBytes(QStringLiteral("SReclaimable"));
            const quint64 sharedMemory = memInfoBytes(QStringLiteral("Shmem"));
            if (memoryAvailable >= sharedMemory) {
                memoryAvailable -= sharedMemory;
            }
        }
        if (memoryTotal > 0) {
            memoryAvailable = qMin(memoryAvailable, memoryTotal);
            memoryUsed = memoryTotal - memoryAvailable;
        }
    }

    if (memoryTotal > 0) {
        memoryInfoLabel->setText(QStringLiteral("%1 / %2")
                                 .arg(formatBytes(memoryUsed), formatBytes(memoryTotal)));
    } else {
        memoryInfoLabel->setText(QStringLiteral("无法读取运行内存信息"));
    }

    storageInfoLabel->setText(storageSummary());

    appVersionLabel->setText(QStringLiteral("YCEE_V1.0.0"));
    updateUptime();
    osVersionLabel->setText(osPrettyName());
    kernelVersionLabel->setText(
                QStringLiteral("Linux %1").arg(QSysInfo::kernelVersion()));

    const QList<QNetworkInterface> interfaces = usableInterfaces();
    QStringList macLines;
    QStringList ipLines;
    for (const QNetworkInterface &iface : interfaces) {
        const QString mac = iface.hardwareAddress().trimmed();
        if (!mac.isEmpty() && mac != QStringLiteral("00:00:00:00:00:00")) {
            macLines << QStringLiteral("%1：%2").arg(iface.name(), mac);
        }

        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol || ip.isLoopback()) {
                continue;
            }
            ipLines << QStringLiteral("%1：%2").arg(iface.name(), ip.toString());
        }
    }

    macAddressLabel->setText(
                macLines.isEmpty() ? QStringLiteral("未检测到已启用的网络接口")
                                   : macLines.join(QStringLiteral("\n")));
    ipAddressLabel->setText(
                ipLines.isEmpty() ? QStringLiteral("未获取到 IPv4 地址")
                                  : ipLines.join(QStringLiteral("\n")));
}

/** @brief 用户点击后重新读取全部系统信息。 */
void SystemInfoPage::onRefreshButtonClicked()
{
    updateSystemInfo();
}

/** @brief 更新系统启动时长文本。 */
void SystemInfoPage::updateUptime()
{
    systemUptimeLabel->setText(systemUptimeText());
}

/** @brief 页面显示时刷新完整信息并启动运行时长更新。 */
void SystemInfoPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    updateSystemInfo();
    uptimeTimer->start();
}

/** @brief 页面隐藏时停止周期更新。 */
void SystemInfoPage::hideEvent(QHideEvent *event)
{
    uptimeTimer->stop();
    QWidget::hideEvent(event);
}
