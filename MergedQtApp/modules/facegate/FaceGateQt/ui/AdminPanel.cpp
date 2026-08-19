/**
 * @file AdminPanel.cpp
 * @brief 人脸门禁管理员综合面板的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "AdminPanel.h"
#include "FaceEnrollWidget.h"
#include "AppMessageDialog.h"
#include "AppPasswordDialog.h"

#include <QtQml/QQmlError>
#include <QAbstractItemView>
#include <QAbstractSocket>
#include <QApplication>
#include <QColor>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFrame>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QNetworkAddressEntry>
#include <QPalette>
#include <QNetworkInterface>
#include <QPushButton>
#include <QProcess>
#include <QPixmap>
#include <QQuickWidget>
#include <QGuiApplication>
#include <QIcon>
#include <QInputMethod>
#include <QRadioButton>
#include <QRegExp>
#include <QScrollArea>
#include <QSlider>
#include <QSize>
#include <QSizePolicy>
#include <QStorageInfo>
#include <QUrl>
#include <QStackedWidget>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

namespace {
/** @brief 创建管理员表格使用的只读单元格。 */
QTableWidgetItem *tableItem(const QString &text, qint64 id = 0)
{
    auto *item = new QTableWidgetItem(text);
    item->setToolTip(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    if (id > 0) {
        item->setData(Qt::UserRole, id);
    }
    return item;
}

/** @brief 读取并裁剪单行系统文件内容。 */
QString trimmedFileText(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromLocal8Bit(file.readAll()).trimmed();
}

/** @brief 按候选系统节点顺序返回第一个有效文本。 */
QString firstAvailableText(const QStringList &paths)
{
    for (const QString &path : paths) {
        const QString text = trimmedFileText(path);
        if (!text.isEmpty()) {
            return text;
        }
    }
    return QString();
}

/** @brief 将字节数格式化为适合显示的容量单位。 */
QString formatBytes(qint64 bytes)
{
    double value = static_cast<double>(bytes);
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < 4) {
        value /= 1024.0;
        ++unitIndex;
    }
    return QString("%1%2").arg(value, 0, unitIndex == 0 ? 'f' : 'f', unitIndex == 0 ? 0 : 1).arg(units[unitIndex]);
}

/** @brief 定位应用数据目录所在且已经就绪的文件系统。 */
QStorageInfo storageForApplicationData()
{
    // root() 在部分嵌入式系统上会命中只读 /rom 或 overlay 上层，
    // 容易显示成“已用=总容量”。按应用所在目录取挂载点，
    // 才能反映人脸库、抓拍和日志实际占用的存储分区。
    QStorageInfo storage(QCoreApplication::applicationDirPath());
    storage.refresh();
    if (!storage.isValid() || !storage.isReady() || storage.bytesTotal() <= 0) {
        storage = QStorageInfo::root();
        storage.refresh();
    }
    return storage;
}

/** @brief 返回首个可用非回环 IPv4 地址。 */
QString firstIpv4Address()
{
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            !(iface.flags() & QNetworkInterface::IsRunning) ||
            (iface.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                return entry.ip().toString();
            }
        }
    }
    return "未连接";
}

/** @brief 返回首个有效非回环网卡的 MAC 地址。 */
QString firstMacAddress()
{
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            (iface.flags() & QNetworkInterface::IsLoopBack) ||
            iface.hardwareAddress().isEmpty()) {
            continue;
        }
        return iface.hardwareAddress();
    }
    return "未知";
}

/** @brief 管理页面一次读取到的网络运行状态快照。 */
struct NetworkSnapshot {
    QString ip;        /**< IPv4 地址。 */
    QString netmask;   /**< IPv4 子网掩码。 */
    QString gateway;   /**< 默认网关。 */
    QString dns;       /**< 首选 DNS 服务器。 */
    QString ifaceName; /**< 提供该状态的网卡名称。 */
};

/** @brief 将 /proc/net/route 中的小端十六进制网关转换为 IPv4 文本。 */
QString gatewayFromRouteHex(const QString &hex)
{
    bool ok = false;
    const quint32 value = hex.toUInt(&ok, 16);
    if (!ok || value == 0) {
        return QString();
    }
    return QString("%1.%2.%3.%4")
        .arg(value & 0xff)
        .arg((value >> 8) & 0xff)
        .arg((value >> 16) & 0xff)
        .arg((value >> 24) & 0xff);
}

/** @brief 从内核路由表读取指定接口的默认网关。 */
QString defaultGatewayForInterface(const QString &ifaceName)
{
    QFile file("/proc/net/route");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning().noquote() << "读取 /proc/net/route 失败：" << file.errorString();
        return QString();
    }

    while (!file.atEnd()) {
        const QString line = QString::fromLocal8Bit(file.readLine()).simplified();
        const QStringList fields = line.split(' ');
        if (fields.size() < 3 || fields.value(0) == "Iface") {
            continue;
        }
        if (!ifaceName.isEmpty() && fields.value(0) != ifaceName) {
            continue;
        }
        if (fields.value(1) == "00000000") {
            return gatewayFromRouteHex(fields.value(2));
        }
    }
    return QString();
}

/** @brief 从 resolv.conf 返回首个有效 DNS 服务器。 */
QString firstDnsServer()
{
    QFile file("/etc/resolv.conf");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning().noquote() << "读取 /etc/resolv.conf 失败：" << file.errorString();
        return QString();
    }

    while (!file.atEnd()) {
        const QString line = QString::fromLocal8Bit(file.readLine()).trimmed();
        if (line.startsWith('#')) {
            continue;
        }
        const QStringList parts = line.split(QRegExp("\\s+"), QString::SkipEmptyParts);
        if (parts.size() >= 2 && parts.value(0) == "nameserver") {
            return parts.value(1);
        }
    }
    return QString();
}

/** @brief 采集当前接口、IP、掩码、网关和 DNS 的只读快照。 */
NetworkSnapshot currentNetworkSnapshot()
{
    NetworkSnapshot snapshot;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp) ||
            !(iface.flags() & QNetworkInterface::IsRunning) ||
            (iface.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            snapshot.ifaceName = iface.name();
            snapshot.ip = entry.ip().toString();
            snapshot.netmask = entry.netmask().toString();
            snapshot.gateway = defaultGatewayForInterface(snapshot.ifaceName);
            snapshot.dns = firstDnsServer();
            return snapshot;
        }
    }
    snapshot.gateway = defaultGatewayForInterface(QString());
    snapshot.dns = firstDnsServer();
    return snapshot;
}

/** @brief 按平台候选节点读取设备序列号。 */
QString deviceSerialNumber()
{
    const QString serial = firstAvailableText(QStringList()
        << "/proc/device-tree/serial-number"
        << "/sys/class/dmi/id/product_serial"
        << "/sys/class/dmi/id/board_serial");
    if (!serial.isEmpty()) {
        return serial;
    }

    QFile file("/proc/cpuinfo");
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!file.atEnd()) {
            const QString line = QString::fromLocal8Bit(file.readLine()).trimmed();
            if (line.startsWith("Serial", Qt::CaseInsensitive)) {
                const int colon = line.indexOf(':');
                return colon >= 0 ? line.mid(colon + 1).trimmed() : line;
            }
        }
    }
    return "未知";
}

/** @brief 读取热区温度并转换为摄氏度显示文本。 */
QString currentTemperature()
{
    QDir dir("/sys/class/thermal");
    const QStringList zones = dir.entryList(QStringList() << "thermal_zone*", QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &zone : zones) {
        const QString raw = trimmedFileText(dir.absoluteFilePath(zone + "/temp"));
        bool ok = false;
        double value = raw.toDouble(&ok);
        if (!ok) {
            continue;
        }
        if (value > 1000.0) {
            value /= 1000.0;
        }
        return QString("%1 C").arg(value, 0, 'f', 1);
    }
    return "--";
}

/** @brief 读取 NPU 负载节点并生成百分比显示文本。 */
QString npuUsage()
{
    const QString raw = firstAvailableText(QStringList()
        << "/sys/kernel/debug/rknpu/load"
        << "/sys/class/devfreq/fdab0000.npu/load"
        << "/sys/class/devfreq/fde40000.npu/load");
    return raw.isEmpty() ? "--" : raw;
}

/** @brief 创建设备信息页面的名称和值双列行。 */
QWidget *metricRow(const QString &name, QLabel **valueLabel, QWidget *parent, const QString &iconPath = QString())
{
    auto *row = new QWidget(parent);
    row->setObjectName("metricRow");
    row->setMinimumHeight(58);
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(10);

    if (!iconPath.isEmpty()) {
        auto *iconLabel = new QLabel(row);
        iconLabel->setObjectName("metricIcon");
        iconLabel->setFixedSize(34, 34);
        iconLabel->setAlignment(Qt::AlignCenter);
        iconLabel->setPixmap(QIcon(iconPath).pixmap(18, 18));
        layout->addWidget(iconLabel, 0, Qt::AlignTop);
    }

    auto *textBox = new QVBoxLayout();
    textBox->setContentsMargins(0, 0, 0, 0);
    textBox->setSpacing(4);

    auto *nameLabel = new QLabel(name, row);
    nameLabel->setObjectName("metricName");
    nameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    *valueLabel = new QLabel("--", row);
    (*valueLabel)->setObjectName("metricValue");
    (*valueLabel)->setWordWrap(true);
    (*valueLabel)->setMinimumHeight(22);
    (*valueLabel)->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    textBox->addWidget(nameLabel);
    textBox->addWidget(*valueLabel);
    layout->addLayout(textBox, 1);
    return row;
}

/** @brief 加载图标并按按钮尺寸设置显示大小。 */
void setButtonIcon(QPushButton *button, const QString &iconPath)
{
    if (!button || iconPath.isEmpty()) {
        return;
    }
    button->setIcon(QIcon(iconPath));
    button->setIconSize(QSize(18, 18));
}

/** @brief 把网络快照格式化为设备信息页面的多行文本。 */
QString networkDisplayText(const QString &text)
{
    const QString trimmed = text.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("--") : trimmed;
}

/** @brief 从网络快照提取可写入编辑框的指定字段。 */
QString networkEditValue(QLineEdit *edit)
{
    if (!edit) {
        return QString();
    }
    const QString text = edit->text().trimmed();
    return text == QStringLiteral("--") ? QString() : text;
}

/** @brief 创建带标签和输入框的网络设置行。 */
QWidget *networkEditorRow(const QString &name, QLineEdit *edit, QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto *nameLabel = new QLabel(name, row);
    nameLabel->setObjectName("metricName");
    nameLabel->setFixedWidth(110);
    layout->addWidget(nameLabel);
    layout->addWidget(edit, 1);
    return row;
}

/** @brief 设置网络输入框文本并保持光标位置稳定。 */
void setNetworkEditorText(QLineEdit *edit, const QString &text)
{
    if (edit) {
        edit->setText(networkDisplayText(text));
    }
}

/** @brief 把运行秒数格式化为便于运维查看的持续时间。 */
QString formatRunDuration(qint64 totalSeconds)
{
    totalSeconds = qMax<qint64>(0, totalSeconds);
    const qint64 secondsPerMinute = 60;
    const qint64 secondsPerHour = secondsPerMinute * 60;
    const qint64 secondsPerDay = secondsPerHour * 24;
    const qint64 secondsPerYear = secondsPerDay * 365;

    const qint64 years = totalSeconds / secondsPerYear;
    totalSeconds %= secondsPerYear;
    const qint64 days = totalSeconds / secondsPerDay;
    totalSeconds %= secondsPerDay;
    const qint64 hours = totalSeconds / secondsPerHour;
    totalSeconds %= secondsPerHour;
    const qint64 minutes = totalSeconds / secondsPerMinute;
    const qint64 seconds = totalSeconds % secondsPerMinute;

    if (years > 0) {
        return QString("%1年%2天%3时%4分%5秒").arg(years).arg(days).arg(hours).arg(minutes).arg(seconds);
    }
    return QString("%1天%2时%3分%4秒").arg(days).arg(hours).arg(minutes).arg(seconds);
}

/** @brief 从设置对象读取并限制滑块配置值。 */
float sliderValue(QSlider *slider)
{
    return slider ? static_cast<float>(slider->value()) / 1000.0f : 0.0f;
}

/** @brief 从 /proc/meminfo 读取指定字段的 KiB 数值。 */
qint64 meminfoValueKb(const QString &text, const QString &key)
{
    QRegExp rx(QString("^%1:\\s*(\\d+)\\s*kB").arg(QRegExp::escape(key)));
    rx.setMinimal(true);
    const QStringList lines = text.split('\n');
    for (const QString &line : lines) {
        if (rx.indexIn(line.trimmed()) >= 0) {
            return rx.cap(1).toLongLong();
        }
    }
    return 0;
}

const int kEmbeddedKeyboardHeight = 300;
const int kAdminSidebarWidth = 184;
const int kAdminOuterMargin = 16;
const int kAdminTabMargin = 12;
const int kAdminRowSpacing = 6;
const int kDeviceInfoPageIndex = 0;
const int kPersonManagementPageIndex = 1;
const int kFaceRecordPageIndex = 2;
const int kSyncPageIndex = 3;
const int kMaintenancePageIndex = 4;
const int kSettingsPageIndex = 5;
const int kNewPersonTabIndex = 0;
const int kViewPeopleTabIndex = 1;
const int kNetworkPeopleTabIndex = 2;
}

/** @brief 按当前配置创建导航、内容页和内嵌虚拟键盘。 */
AdminPanel::AdminPanel(const AppConfig &config, QWidget *parent)
    : QDialog(parent), config_(config)
{
    setWindowTitle("人脸闸机管理");
    setModal(false);
    resize(1024, 768);
    // 管理界面目标屏幕为 1024x768。第二阶段样式加入图标和较大 padding 后，
    // 部分子控件的 minimumSizeHint 会反向把顶层 QDialog 撑宽。固定目标尺寸，
    // 再让右侧页面在内部换行/滚动，避免窗口超出屏幕。
    setFixedSize(1024, 768);

    // 左侧为导航栏，右侧为当前管理页面。
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->setSizeConstraint(QLayout::SetNoConstraint);

    auto *sidebar = new QFrame(this);
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(kAdminSidebarWidth);
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(14, 18, 14, 18);
    sideLayout->setSpacing(10);

    auto *title = new QLabel("管理", sidebar);
    title->setObjectName("adminTitle");
    auto *subtitle = new QLabel("闸机控制台", sidebar);
    subtitle->setObjectName("adminSubtitle");

    // 管理页面导航顺序必须与 stack_ 页面添加顺序一致。
    navList_ = new QListWidget(sidebar);
    navList_->setObjectName("navList");
    navList_->setIconSize(QSize(22, 22));
    navList_->addItem(new QListWidgetItem(QIcon(QStringLiteral(":/icons/nav/device.svg")), QStringLiteral("本机信息")));
    navList_->addItem(new QListWidgetItem(QIcon(QStringLiteral(":/icons/nav/users.svg")), QStringLiteral("人员管理")));
    navList_->addItem(new QListWidgetItem(QIcon(QStringLiteral(":/icons/nav/logs.svg")), QStringLiteral("识别记录")));
    navList_->addItem(new QListWidgetItem(QIcon(QStringLiteral(":/icons/nav/sync.svg")), QStringLiteral("同步")));
    navList_->addItem(new QListWidgetItem(QIcon(QStringLiteral(":/icons/nav/tools.svg")), QStringLiteral("运维维护")));
    navList_->addItem(new QListWidgetItem(QIcon(QStringLiteral(":/icons/nav/settings.svg")), QStringLiteral("设置")));
    navList_->setCurrentRow(kDeviceInfoPageIndex);

    livenessCheck_ = new QCheckBox("活体检测", sidebar);
    livenessCheck_->setObjectName("livenessSwitch");
    livenessCheck_->setChecked(true);

    auto *backButton = new QPushButton("返回", sidebar);
    backButton->setObjectName("backButton");
    setButtonIcon(backButton, QStringLiteral(":/icons/action/x.svg"));

    sideLayout->addWidget(title);
    sideLayout->addWidget(subtitle);
    sideLayout->addSpacing(18);
    sideLayout->addWidget(navList_);
    sideLayout->addStretch();
    sideLayout->addWidget(livenessCheck_);
    sideLayout->addWidget(backButton);

    stack_ = new QStackedWidget(this);
    stack_->setObjectName("contentStack");
    // 右侧页面不能把管理窗口的最小宽度撑大；宽表格和工具栏在页面内部滚动或换行。
    stack_->setMinimumWidth(0);
    stack_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

    enrollWidget_ = new FaceEnrollWidget(this);
    enrollWidget_->setAutoEnrollThresholds(config_.faceQualityThreshold, config_.faceDetectThreshold, config_.livenessThreshold);

    stack_->addWidget(createDeviceInfoPage());
    stack_->addWidget(createPersonManagementPage());
    stack_->addWidget(createLogPage());
    stack_->addWidget(createSyncPage());
    stack_->addWidget(createMaintenancePage());
    stack_->addWidget(createSettingsPage());

    auto *contentShell = new QWidget(this);
    contentShell_ = contentShell;
    contentShell_->setObjectName("contentShell");
    contentShell_->setMinimumWidth(0);
    contentShell_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    contentShell_->setAttribute(Qt::WA_StyledBackground, true);
    contentShell_->installEventFilter(this);
    qApp->installEventFilter(this);

    auto *contentLayout = new QVBoxLayout(contentShell_);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    contentLayout->setSizeConstraint(QLayout::SetNoConstraint);
    contentLayout->addWidget(stack_, 1);

    // 内嵌 QML 键盘默认隐藏，输入框获得焦点时作为覆盖层展开。
    // 覆盖显示不再挤压 stack_，避免录入预览画面被键盘顶上去或缩小。
    keyboardWidget_ = new QQuickWidget(contentShell_);
    keyboardWidget_->setObjectName("embeddedKeyboard");
    // 键盘必须作为 contentShell_ 的子控件参与同一个窗口事件链。
    // 不使用独立顶层窗口，避免模态窗口/窗口管理器截断或吞掉键盘点击。
    keyboardWidget_->setAttribute(Qt::WA_AcceptTouchEvents, true);
    keyboardWidget_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    keyboardWidget_->setClearColor(QColor("#101820"));
    keyboardWidget_->setFocusPolicy(Qt::NoFocus);
    keyboardWidget_->setFixedHeight(kEmbeddedKeyboardHeight);
    keyboardWidget_->setSource(QUrl(QStringLiteral("qrc:/qml/EmbeddedVirtualKeyboard.qml")));
    qDebug().noquote() << "内嵌键盘 QML 状态：" << keyboardWidget_->status()
                       << "错误数：" << keyboardWidget_->errors().size();
    keyboardWidget_->hide();

    root->addWidget(sidebar);
    root->addWidget(contentShell_, 1);

    connect(enrollWidget_, &FaceEnrollWidget::enrollRequested, this, &AdminPanel::enrollRequested);
    connect(enrollWidget_, &FaceEnrollWidget::cameraRequirementChanged, this, &AdminPanel::updateCameraRequirement);
    connect(livenessCheck_, &QCheckBox::toggled, this, &AdminPanel::livenessEnabledChanged);
    connect(backButton, &QPushButton::clicked, this, &AdminPanel::close);
    connect(navList_, &QListWidget::currentRowChanged, this, [this](int row) {
        stack_->setCurrentIndex(row);
        if (row == kDeviceInfoPageIndex) {
            updateDeviceInfo();
            emit deviceInfoRefreshRequested();
        } else if (row == kPersonManagementPageIndex) {
            emit peopleRefreshRequested();
        } else if (row == kFaceRecordPageIndex && !passedLogsLoaded_) {
            QTimer::singleShot(0, this, [this]() { requestQueryVerifyLogs(); });
        } else if (row == kSyncPageIndex) {
            emit syncTasksRefreshRequested();
        } else if (row == kMaintenancePageIndex) {
            emit deviceInfoRefreshRequested();
        }
        updateCameraRequirement();
    });

    deviceStatusTimer_ = new QTimer(this);
    deviceStatusTimer_->setInterval(1000);
    connect(deviceStatusTimer_, &QTimer::timeout, this, &AdminPanel::updateDeviceInfo);
    deviceStatusTimer_->start();

    if (QGuiApplication::inputMethod()) {
        connect(QGuiApplication::inputMethod(), &QInputMethod::visibleChanged, this, [this]() {
            if (!QGuiApplication::inputMethod()->isVisible()) {
                if (keyboardWidget_) {
                    keyboardWidget_->hide();
                }
                return;
            }
            updateEmbeddedKeyboardVisibility();
        });
    }
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (now && isAncestorOf(now) && now->testAttribute(Qt::WA_InputMethodEnabled)) {
            if (QGuiApplication::inputMethod()) {
                QGuiApplication::inputMethod()->show();
            }
        }
        updateEmbeddedKeyboardVisibility();
    });

    QFile adminStyleFile(QStringLiteral(":/qss/admin_panel.qss"));
    if (adminStyleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setStyleSheet(QString::fromUtf8(adminStyleFile.readAll()));
    }
}

/** @return 人员录入和管理组合页面。 */
QWidget *AdminPanel::createPersonManagementPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(kAdminOuterMargin, 18, kAdminOuterMargin, 18);
    layout->setSpacing(10);

    auto *title = new QLabel("人员管理", page);
    title->setObjectName("pageTitle");
    auto *hint = new QLabel("新建人员、查看已录入人员、修改编号、权限或删除人员", page);
    hint->setObjectName("pageHint");
    hint->setWordWrap(true);

    personTabs_ = new QTabWidget(page);
    personTabs_->setObjectName("personTabs");
    personTabs_->addTab(enrollWidget_, "新建人员");
    personTabs_->addTab(createPeoplePage(), "查看人员");
    personTabs_->addTab(createNetworkPeoplePage(), "网络人员");
    connect(personTabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == kViewPeopleTabIndex) {
            emit peopleRefreshRequested();
        } else if (index == kNetworkPeopleTabIndex) {
            emit networkPeopleRefreshRequested();
        }
        updateCameraRequirement();
    });

    layout->addWidget(title);
    layout->addWidget(hint);
    layout->addWidget(personTabs_, 1);
    return page;
}

/** @return 人员列表页面。 */
QWidget *AdminPanel::createPeoplePage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(kAdminOuterMargin, 18, kAdminOuterMargin, 18);
    layout->setSpacing(10);

    // 原来所有筛选框和操作按钮都放在同一条横向布局里，
    // 在 1024x768 屏幕上会把右侧页面最小宽度撑到超过可视区域，
    // 进而导致右侧内容和内嵌键盘一起被裁剪。这里改为两行紧凑布局。
    auto *filterRow = new QHBoxLayout();
    filterRow->setSpacing(kAdminRowSpacing);
    verifyResultCombo_ = new QComboBox(page);
    verifyResultCombo_->addItem("全部", "all");
    verifyResultCombo_->addItem("通过", "passed");
    verifyResultCombo_->addItem("失败", "failed");
    verifyResultCombo_->addItem("活体失败", "liveness_failed");
    verifyResultCombo_->addItem("陌生人", "stranger");
    verifyResultCombo_->addItem("门禁拒绝", "access_denied");
    verifyResultCombo_->addItem("出门按钮", "exit_button");
    verifyResultCombo_->addItem("报警", "alarm");
    verifyResultCombo_->setFixedWidth(88);
    verifyPersonNoEdit_ = new QLineEdit(page);
    verifyPersonNoEdit_->setPlaceholderText("人员编号");
    verifyPersonNoEdit_->setFixedWidth(96);
    verifyNameEdit_ = new QLineEdit(page);
    verifyNameEdit_->setPlaceholderText("姓名");
    verifyNameEdit_->setFixedWidth(84);
    verifyStartEdit_ = new QDateTimeEdit(QDateTime::currentDateTime().addDays(-7), page);
    verifyStartEdit_->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
    verifyStartEdit_->setFixedWidth(146);
    verifyEndEdit_ = new QDateTimeEdit(QDateTime::currentDateTime(), page);
    verifyEndEdit_->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
    verifyEndEdit_->setFixedWidth(146);
    auto *refreshButton = new QPushButton("刷新", page);
    refreshButton->setObjectName("toolButton");
    setButtonIcon(refreshButton, QStringLiteral(":/icons/action/refresh.svg"));

    // 操作按钮使用网格换行，而不是单条 HBox。
    // 否则 stage2 的图标、padding 和字体会让该行最小宽度超过右侧可视区域。
    auto *buttonGrid = new QGridLayout();
    buttonGrid->setHorizontalSpacing(kAdminRowSpacing);
    buttonGrid->setVerticalSpacing(kAdminRowSpacing);
    buttonGrid->setContentsMargins(0, 0, 0, 0);
    auto *modifyButton = new QPushButton("修改编号", page);
    modifyButton->setObjectName("toolButton");
    setButtonIcon(modifyButton, QStringLiteral(":/icons/action/edit.svg"));
    auto *permissionButton = new QPushButton("切换权限", page);
    permissionButton->setObjectName("toolButton");
    setButtonIcon(permissionButton, QStringLiteral(":/icons/action/key.svg"));
    auto *reEnrollButton = new QPushButton("重新录入", page);
    reEnrollButton->setObjectName("toolButton");
    setButtonIcon(reEnrollButton, QStringLiteral(":/icons/info/face.svg"));
    importPeopleButton_ = new QPushButton("导入人员表", page);
    importPeopleButton_->setObjectName("toolButton");
    setButtonIcon(importPeopleButton_, QStringLiteral(":/icons/action/upload.svg"));
    auto *deleteButton = new QPushButton("删除", page);
    deleteButton->setObjectName("dangerButton");
    setButtonIcon(deleteButton, QStringLiteral(":/icons/action/trash.svg"));
    exportPeopleButton_ = new QPushButton("导出人员表", page);
    exportPeopleButton_->setObjectName("toolButton");
    setButtonIcon(exportPeopleButton_, QStringLiteral(":/icons/action/download.svg"));
    showDeletedPeopleCheck_ = new QCheckBox("显示已删除", page);

    filterRow->addWidget(verifyResultCombo_);
    filterRow->addWidget(verifyPersonNoEdit_);
    filterRow->addWidget(verifyNameEdit_);
    filterRow->addWidget(verifyStartEdit_);
    filterRow->addWidget(verifyEndEdit_);
    filterRow->addWidget(refreshButton);
    filterRow->addStretch();

    buttonGrid->addWidget(modifyButton, 0, 0);
    buttonGrid->addWidget(permissionButton, 0, 1);
    buttonGrid->addWidget(reEnrollButton, 0, 2);
    buttonGrid->addWidget(importPeopleButton_, 0, 3);
    buttonGrid->addWidget(deleteButton, 1, 0);
    buttonGrid->addWidget(exportPeopleButton_, 1, 1);
    buttonGrid->addWidget(showDeletedPeopleCheck_, 1, 2);
    buttonGrid->setColumnStretch(3, 1);

    peopleTable_ = new QTableWidget(page);
    peopleTable_->setMinimumWidth(0);
    peopleTable_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    peopleTable_->setColumnCount(8);
    peopleTable_->setHorizontalHeaderLabels(QStringList()
        << "录入ID" << "人员编号" << "姓名" << "权限" << "删除状态" << "录入时间" << "更新时间" << "特征数");
    peopleTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    peopleTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    peopleTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    peopleTable_->setAutoFillBackground(false);
    peopleTable_->viewport()->setAutoFillBackground(true);
    peopleTable_->viewport()->setAttribute(Qt::WA_StyledBackground, true);
    QPalette peoplePalette = peopleTable_->palette();
    peoplePalette.setColor(QPalette::Base, QColor("#111c24"));
    peoplePalette.setColor(QPalette::Window, QColor("#111c24"));
    peopleTable_->setPalette(peoplePalette);
    peopleTable_->viewport()->setPalette(peoplePalette);
    peopleTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    peopleTable_->setWordWrap(false);
    peopleTable_->verticalHeader()->setVisible(false);
    peopleTable_->verticalHeader()->setDefaultSectionSize(38);
    auto *peopleHeader = peopleTable_->horizontalHeader();
    peopleHeader->setStretchLastSection(false);
    peopleHeader->setSectionsMovable(false);
    peopleHeader->setSectionsClickable(true);
    peopleHeader->setMinimumSectionSize(60);
    peopleHeader->setSectionResizeMode(QHeaderView::Interactive);
    peopleHeader->resizeSection(0, 80);
    peopleHeader->resizeSection(1, 126);
    peopleHeader->resizeSection(2, 112);
    peopleHeader->resizeSection(3, 72);
    peopleHeader->resizeSection(4, 86);
    peopleHeader->resizeSection(5, 148);
    peopleHeader->resizeSection(6, 148);
    peopleHeader->resizeSection(7, 76);

    layout->addLayout(filterRow);
    layout->addLayout(buttonGrid);
    layout->addWidget(peopleTable_, 1);

    connect(refreshButton, &QPushButton::clicked, this, &AdminPanel::peopleRefreshRequested);
    connect(showDeletedPeopleCheck_, &QCheckBox::toggled, this, &AdminPanel::peopleRefreshRequested);
    connect(modifyButton, &QPushButton::clicked, this, &AdminPanel::requestModifySelectedPersonNo);
    connect(permissionButton, &QPushButton::clicked, this, &AdminPanel::requestToggleSelectedPersonEnabled);
    connect(reEnrollButton, &QPushButton::clicked, this, &AdminPanel::requestReEnrollSelectedPerson);
    connect(importPeopleButton_, &QPushButton::clicked,
            this, &AdminPanel::requestImportPeopleFromUsb);
    connect(deleteButton, &QPushButton::clicked, this, &AdminPanel::requestDeleteSelectedPerson);
    connect(exportPeopleButton_, &QPushButton::clicked,
            this, &AdminPanel::requestExportPeopleToUsb);

    return page;
}

/** @return 网络人员与人脸原图同步页面。 */
QWidget *AdminPanel::createNetworkPeoplePage()
{
    auto *page = new QWidget(this);
    networkPeoplePage_ = page;
    page->installEventFilter(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(kAdminOuterMargin, 18,
                               kAdminOuterMargin, 18);
    layout->setSpacing(10);

    auto *hint = new QLabel(
                "接收网络人员全量同步后自动拉取缺失的人脸原图；"
                "当前只保存图像，不提取人脸特征、不参与识别。",
                page);
    hint->setObjectName("pageHint");
    hint->setWordWrap(true);

    auto *buttonRow = new QHBoxLayout();
    auto *refreshButton = new QPushButton("刷新", page);
    refreshButton->setObjectName("toolButton");
    setButtonIcon(refreshButton, QStringLiteral(":/icons/action/refresh.svg"));
    networkActiveButton_ = new QPushButton(QStringLiteral("正常人员"), page);
    networkActiveButton_->setObjectName("toolButton");
    networkActiveButton_->setCheckable(true);
    networkDeletedButton_ = new QPushButton(QStringLiteral("已删除"), page);
    networkDeletedButton_->setObjectName("toolButton");
    networkDeletedButton_->setCheckable(true);
    buttonRow->addWidget(refreshButton);
    buttonRow->addStretch();
    buttonRow->addWidget(networkActiveButton_);
    buttonRow->addWidget(networkDeletedButton_);

    networkPeopleTable_ = new QTableWidget(page);
    networkPeopleTable_->setMinimumWidth(0);
    networkPeopleTable_->setSizePolicy(QSizePolicy::Ignored,
                                       QSizePolicy::Expanding);
    networkPeopleTable_->setColumnCount(7);
    networkPeopleTable_->setHorizontalHeaderLabels(
                QStringList()
                << QStringLiteral("图像")
                << QStringLiteral("员工号")
                << QStringLiteral("姓名")
                << QStringLiteral("状态")
                << QStringLiteral("人脸数")
                << QStringLiteral("人员ID")
                << QStringLiteral("更新时间"));
    networkPeopleTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    networkPeopleTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    networkPeopleTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    networkPeopleTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    networkPeopleTable_->setWordWrap(false);
    networkPeopleTable_->verticalHeader()->setVisible(false);
    networkPeopleTable_->setIconSize(QSize(56, 56));
    networkPeopleTable_->verticalHeader()->setDefaultSectionSize(64);
    auto *header = networkPeopleTable_->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionsMovable(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->resizeSection(0, 72);
    header->resizeSection(1, 110);
    header->resizeSection(2, 110);
    header->resizeSection(3, 90);
    header->resizeSection(4, 80);
    header->resizeSection(5, 280);
    header->setStretchLastSection(true);

    networkSyncToast_ = new QLabel(page);
    networkSyncToast_->setObjectName(QStringLiteral("networkSyncToast"));
    networkSyncToast_->setAlignment(Qt::AlignCenter);
    networkSyncToast_->setWordWrap(true);
    networkSyncToast_->setAttribute(Qt::WA_TransparentForMouseEvents);
    networkSyncToast_->hide();
    networkSyncToastTimer_ = new QTimer(page);
    networkSyncToastTimer_->setSingleShot(true);
    networkSyncToastTimer_->setInterval(3000);

    layout->addWidget(hint);
    layout->addLayout(buttonRow);
    layout->addWidget(networkPeopleTable_, 1);

    connect(refreshButton, &QPushButton::clicked,
            this, &AdminPanel::networkPeopleRefreshRequested);
    connect(networkActiveButton_, &QPushButton::clicked,
            this, [this]() { setNetworkDeletedView(false); });
    connect(networkDeletedButton_, &QPushButton::clicked,
            this, [this]() { setNetworkDeletedView(true); });
    connect(networkPeopleTable_, &QTableWidget::cellDoubleClicked,
            this, [this](int row, int column) {
        if (column == 4) {
            showNetworkPersonFaces(row);
        }
    });
    connect(networkSyncToastTimer_, &QTimer::timeout,
            networkSyncToast_, &QLabel::hide);
    setNetworkDeletedView(false);
    return page;
}

/** @return 验证日志查询页面。 */
QWidget *AdminPanel::createLogPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(kAdminOuterMargin, 18, kAdminOuterMargin, 18);
    layout->setSpacing(10);

    auto *title = new QLabel("识别记录", page);
    title->setObjectName("pageTitle");
    auto *hint = new QLabel("显示全部通行记录，包含通过、失败、活体失败、陌生人等事件", page);
    hint->setObjectName("pageHint");
    hint->setWordWrap(true);

    auto *buttonRow = new QHBoxLayout();
    auto *refreshButton = new QPushButton("刷新", page);
    refreshButton->setObjectName("toolButton");
    setButtonIcon(refreshButton, QStringLiteral(":/icons/action/refresh.svg"));
    auto *exportButton = new QPushButton("导出CSV", page);
    exportButton->setObjectName("toolButton");
    setButtonIcon(exportButton, QStringLiteral(":/icons/action/download.svg"));
    buttonRow->addWidget(refreshButton);
    buttonRow->addWidget(exportButton);
    buttonRow->addStretch();

    passedLogTable_ = new QTableWidget(page);
    passedLogTable_->setMinimumWidth(0);
    passedLogTable_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    passedLogTable_->setColumnCount(13);
    passedLogTable_->setHorizontalHeaderLabels(QStringList()
        << "日志ID" << "结果" << "人员编号" << "姓名" << "失败原因" << "余弦值" << "活体"
        << "抓拍路径" << "设备SN" << "事件类型" << "方向" << "人员ID" << "时间");
    passedLogTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    passedLogTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    passedLogTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    passedLogTable_->setAutoFillBackground(false);
    passedLogTable_->viewport()->setAutoFillBackground(true);
    passedLogTable_->viewport()->setAttribute(Qt::WA_StyledBackground, true);
    QPalette logPalette = passedLogTable_->palette();
    logPalette.setColor(QPalette::Base, QColor("#111c24"));
    logPalette.setColor(QPalette::Window, QColor("#111c24"));
    passedLogTable_->setPalette(logPalette);
    passedLogTable_->viewport()->setPalette(logPalette);
    passedLogTable_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    passedLogTable_->setWordWrap(false);
    passedLogTable_->verticalHeader()->setVisible(false);
    passedLogTable_->verticalHeader()->setDefaultSectionSize(38);
    passedLogTable_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    auto *logHeader = passedLogTable_->horizontalHeader();
    logHeader->setStretchLastSection(false);
    logHeader->setSectionsMovable(false);
    logHeader->setSectionsClickable(true);
    logHeader->setMinimumSectionSize(60);
    logHeader->setSectionResizeMode(QHeaderView::Interactive);
    logHeader->resizeSection(0, 82);
    logHeader->resizeSection(1, 82);
    logHeader->resizeSection(2, 126);
    logHeader->resizeSection(3, 112);
    logHeader->resizeSection(4, 86);
    logHeader->resizeSection(5, 78);
    logHeader->resizeSection(6, 78);
    logHeader->resizeSection(7, 180);
    logHeader->resizeSection(8, 120);
    logHeader->resizeSection(9, 110);
    logHeader->resizeSection(10, 70);
    logHeader->resizeSection(11, 82);
    logHeader->resizeSection(12, 178);

    layout->addWidget(title);
    layout->addWidget(hint);
    layout->addLayout(buttonRow);
    layout->addWidget(passedLogTable_, 1);

    connect(refreshButton, &QPushButton::clicked, this, &AdminPanel::requestQueryVerifyLogs);
    connect(exportButton, &QPushButton::clicked, this, [this]() {
        if (!passedLogTable_) {
            return;
        }
        const QString path = QDir(QCoreApplication::applicationDirPath())
            .absoluteFilePath(QString("通行记录_%1.csv").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            AppMessageDialog::warning(this, "导出CSV", "导出文件失败：" + file.errorString());
            return;
        }
        QTextStream out(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        out.setCodec("UTF-8");
#endif
        QStringList headers;
        for (int col = 0; col < passedLogTable_->columnCount(); ++col) {
            headers << passedLogTable_->horizontalHeaderItem(col)->text();
        }
        out << headers.join(',') << "\n";
        for (int row = 0; row < passedLogTable_->rowCount(); ++row) {
            QStringList fields;
            for (int col = 0; col < passedLogTable_->columnCount(); ++col) {
                const QTableWidgetItem *item = passedLogTable_->item(row, col);
                QString text = item ? item->text() : QString();
                text.replace("\"", "\"\"");
                fields << ("\"" + text + "\"");
            }
            out << fields.join(',') << "\n";
        }
        AppMessageDialog::success(this, "导出CSV", "通行记录已导出：" + path);
    });

    return page;
}

/** @return 同步任务与系统事件页面。 */
QWidget *AdminPanel::createSyncPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(kAdminOuterMargin, 18, kAdminOuterMargin, 18);
    layout->setSpacing(10);

    auto *title = new QLabel("远程同步", page);
    title->setObjectName("pageTitle");
    auto *hint = new QLabel("网络同步为预留功能，离线记录会继续保存在本地", page);
    hint->setObjectName("pageHint");
    hint->setWordWrap(true);
    auto *refreshButton = new QPushButton("刷新同步队列", page);
    refreshButton->setObjectName("toolButton");
    setButtonIcon(refreshButton, QStringLiteral(":/icons/action/refresh.svg"));
    syncTaskTable_ = new QTableWidget(page);
    syncTaskTable_->setMinimumWidth(0);
    syncTaskTable_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    syncTaskTable_->setColumnCount(7);
    syncTaskTable_->setHorizontalHeaderLabels(QStringList()
        << "任务ID" << "类型" << "状态" << "重试" << "错误" << "创建时间" << "负载JSON");
    syncTaskTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    syncTaskTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    syncTaskTable_->setAutoFillBackground(false);
    syncTaskTable_->viewport()->setAutoFillBackground(true);
    syncTaskTable_->viewport()->setAttribute(Qt::WA_StyledBackground, true);
    QPalette syncPalette = syncTaskTable_->palette();
    syncPalette.setColor(QPalette::Base, QColor("#111c2c"));
    syncPalette.setColor(QPalette::Window, QColor("#111c2c"));
    syncTaskTable_->setPalette(syncPalette);
    syncTaskTable_->viewport()->setPalette(syncPalette);
    syncTaskTable_->verticalHeader()->setVisible(false);
    syncTaskTable_->verticalHeader()->setDefaultSectionSize(38);
    syncTaskTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    syncTaskTable_->horizontalHeader()->resizeSection(6, 220);
    layout->addWidget(title);
    layout->addWidget(hint);
    layout->addWidget(refreshButton, 0, Qt::AlignLeft);
    layout->addWidget(syncTaskTable_, 1);
    connect(refreshButton, &QPushButton::clicked, this, &AdminPanel::syncTasksRefreshRequested);
    return page;
}

/** @return 备份和清理维护页面。 */
QWidget *AdminPanel::createMaintenancePage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(kAdminOuterMargin, 18, kAdminOuterMargin, 18);
    layout->setSpacing(10);

    auto *title = new QLabel("运维维护", page);
    title->setObjectName("pageTitle");
    auto *hint = new QLabel("执行数据库备份、日志和抓拍清理，并查看最近系统错误", page);
    hint->setObjectName("pageHint");
    hint->setWordWrap(true);

    auto *buttonGrid = new QGridLayout();
    buttonGrid->setHorizontalSpacing(kAdminRowSpacing);
    buttonGrid->setVerticalSpacing(kAdminRowSpacing);
    buttonGrid->setContentsMargins(0, 0, 0, 0);
    auto *backupButton = new QPushButton("备份SQLite数据库", page);
    backupButton->setObjectName("toolButton");
    setButtonIcon(backupButton, QStringLiteral(":/icons/action/save.svg"));
    auto *cleanup30Button = new QPushButton("清理30天前", page);
    cleanup30Button->setObjectName("toolButton");
    setButtonIcon(cleanup30Button, QStringLiteral(":/icons/action/trash.svg"));
    auto *cleanup90Button = new QPushButton("清理90天前", page);
    cleanup90Button->setObjectName("toolButton");
    setButtonIcon(cleanup90Button, QStringLiteral(":/icons/action/trash.svg"));
    auto *cleanupCustomButton = new QPushButton("自定义天数", page);
    cleanupCustomButton->setObjectName("toolButton");
    setButtonIcon(cleanupCustomButton, QStringLiteral(":/icons/action/edit.svg"));
    auto *refreshErrorsButton = new QPushButton("刷新错误", page);
    refreshErrorsButton->setObjectName("toolButton");
    setButtonIcon(refreshErrorsButton, QStringLiteral(":/icons/action/refresh.svg"));
    buttonGrid->addWidget(backupButton, 0, 0);
    buttonGrid->addWidget(cleanup30Button, 0, 1);
    buttonGrid->addWidget(cleanup90Button, 0, 2);
    buttonGrid->addWidget(cleanupCustomButton, 1, 0);
    buttonGrid->addWidget(refreshErrorsButton, 1, 1);
    buttonGrid->setColumnStretch(2, 1);

    systemEventTable_ = new QTableWidget(page);
    systemEventTable_->setMinimumWidth(0);
    systemEventTable_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    systemEventTable_->setColumnCount(6);
    systemEventTable_->setHorizontalHeaderLabels(QStringList()
        << "ID" << "类型" << "级别" << "消息" << "详情" << "时间");
    systemEventTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    systemEventTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    systemEventTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    systemEventTable_->horizontalHeader()->resizeSection(3, 180);
    systemEventTable_->horizontalHeader()->resizeSection(4, 220);

    layout->addWidget(title);
    layout->addWidget(hint);
    layout->addLayout(buttonGrid);
    layout->addWidget(systemEventTable_, 1);

    connect(backupButton, &QPushButton::clicked, this, &AdminPanel::maintenanceBackupRequested);
    connect(cleanup30Button, &QPushButton::clicked, this, [this]() { emit maintenanceCleanupRequested(30); });
    connect(cleanup90Button, &QPushButton::clicked, this, [this]() { emit maintenanceCleanupRequested(90); });
    connect(cleanupCustomButton, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const int days = QInputDialog::getInt(this, "自定义清理天数", "清理多少天前的数据：", 30, 1, 3650, 1, &ok);
        if (ok) {
            emit maintenanceCleanupRequested(days);
        }
    });
    connect(refreshErrorsButton, &QPushButton::clicked, this, &AdminPanel::deviceInfoRefreshRequested);
    return page;
}

/** @return 设备资源与运行状态页面。 */
QWidget *AdminPanel::createDeviceInfoPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(kAdminOuterMargin, 18, kAdminOuterMargin, 18);
    layout->setSpacing(10);

    auto *title = new QLabel("本机信息", page);
    title->setObjectName("pageTitle");
    auto *hint = new QLabel("查看设备标识、存储容量和运行状态", page);
    hint->setObjectName("pageHint");
    hint->setWordWrap(true);

    auto *scrollArea = new QScrollArea(page);
    scrollArea->setObjectName("deviceInfoScroll");
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto *scrollContent = new QWidget(scrollArea);
    scrollContent->setObjectName("deviceInfoScrollContent");
    auto *contentLayout = new QVBoxLayout(scrollContent);
    contentLayout->setContentsMargins(0, 4, 8, 8);
    contentLayout->setSpacing(12);

    auto *basicGroup = new QGroupBox("基本信息", scrollContent);
    auto *basicLayout = new QVBoxLayout(basicGroup);
    basicLayout->setContentsMargins(10, 18, 10, 10);
    basicLayout->setSpacing(8);
    basicLayout->addWidget(metricRow("本机序列号", &serialLabel_, basicGroup, QStringLiteral(":/icons/info/serial.svg")));
    basicLayout->addWidget(metricRow("IP 地址", &ipLabel_, basicGroup, QStringLiteral(":/icons/info/ip.svg")));
    basicLayout->addWidget(metricRow("MAC 地址", &macLabel_, basicGroup, QStringLiteral(":/icons/info/mac.svg")));
    basicLayout->addWidget(metricRow("应用版本", &versionLabel_, basicGroup, QStringLiteral(":/icons/info/version.svg")));
    basicLayout->addWidget(metricRow("运行天数", &uptimeLabel_, basicGroup, QStringLiteral(":/icons/info/uptime.svg")));

    auto *storageGroup = new QGroupBox("存储信息", scrollContent);
    auto *storageLayout = new QVBoxLayout(storageGroup);
    storageLayout->setContentsMargins(10, 18, 10, 10);
    storageLayout->setSpacing(8);
    storageLayout->addWidget(metricRow("人员数量", &peopleCapacityLabel_, storageGroup, QStringLiteral(":/icons/info/people.svg")));
    storageLayout->addWidget(metricRow("注册照数量", &faceCapacityLabel_, storageGroup, QStringLiteral(":/icons/info/face.svg")));
    storageLayout->addWidget(metricRow("识别记录数", &verifyLogCountLabel_, storageGroup, QStringLiteral(":/icons/info/logcount.svg")));
    storageLayout->addWidget(metricRow("已用/总容量", &diskCapacityLabel_, storageGroup, QStringLiteral(":/icons/info/storage.svg")));

    auto *statusGroup = new QGroupBox("运行状态", scrollContent);
    auto *statusLayout = new QVBoxLayout(statusGroup);
    statusLayout->setContentsMargins(10, 18, 10, 10);
    statusLayout->setSpacing(8);
    statusLayout->addWidget(metricRow("CPU 占用", &cpuUsageLabel_, statusGroup, QStringLiteral(":/icons/info/cpu.svg")));
    statusLayout->addWidget(metricRow("内存占用", &memoryUsageLabel_, statusGroup, QStringLiteral(":/icons/info/memory.svg")));
    statusLayout->addWidget(metricRow("当前温度", &temperatureLabel_, statusGroup, QStringLiteral(":/icons/info/temp.svg")));
    statusLayout->addWidget(metricRow("NPU 占用", &npuUsageLabel_, statusGroup, QStringLiteral(":/icons/info/npu.svg")));

    contentLayout->addWidget(basicGroup);
    contentLayout->addWidget(storageGroup);
    contentLayout->addWidget(statusGroup);
    contentLayout->addStretch();
    scrollArea->setWidget(scrollContent);

    layout->addWidget(title);
    layout->addWidget(hint);
    layout->addWidget(scrollArea, 1);
    updateDeviceInfo();
    return page;
}

/** @return 阈值、网络、音频和密码设置页面。 */
QWidget *AdminPanel::createSettingsPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(kAdminOuterMargin, 18, kAdminOuterMargin, 18);
    layout->setSpacing(10);

    auto *title = new QLabel("设置", page);
    title->setObjectName("pageTitle");
    auto *hint = new QLabel("配置网络参数、识别阈值和基础运行参数", page);
    hint->setObjectName("pageHint");
    hint->setWordWrap(true);

    auto *tabs = new QTabWidget(page);

    auto *networkPage = new QWidget(tabs);
    auto *networkLayout = new QVBoxLayout(networkPage);
    networkLayout->setContentsMargins(kAdminTabMargin, kAdminTabMargin, kAdminTabMargin, kAdminTabMargin);
    networkLayout->setSpacing(10);
    dhcpRadio_ = new QRadioButton("DHCP获取IP", networkPage);
    staticRadio_ = new QRadioButton("静态IP", networkPage);
    dhcpRadio_->setChecked(config_.networkDhcp);
    staticRadio_->setChecked(!config_.networkDhcp);
    ipEdit_ = new QLineEdit(networkDisplayText(config_.networkStaticIp), networkPage);
    netmaskEdit_ = new QLineEdit(networkDisplayText(config_.networkNetmask), networkPage);
    gatewayEdit_ = new QLineEdit(networkDisplayText(config_.networkGateway), networkPage);
    dnsEdit_ = new QLineEdit(networkDisplayText(config_.networkDns), networkPage);
    ipEdit_->setPlaceholderText("--");
    netmaskEdit_->setPlaceholderText("--");
    gatewayEdit_->setPlaceholderText("--");
    dnsEdit_->setPlaceholderText("--");
    ipEdit_->setAttribute(Qt::WA_InputMethodEnabled, true);
    netmaskEdit_->setAttribute(Qt::WA_InputMethodEnabled, true);
    gatewayEdit_->setAttribute(Qt::WA_InputMethodEnabled, true);
    dnsEdit_->setAttribute(Qt::WA_InputMethodEnabled, true);
    auto *networkSaveButton = new QPushButton("保存网络设置", networkPage);
    networkSaveButton->setObjectName("toolButton");
    setButtonIcon(networkSaveButton, QStringLiteral(":/icons/action/save.svg"));
    networkLayout->addWidget(dhcpRadio_);
    networkLayout->addWidget(staticRadio_);
    networkLayout->addWidget(networkEditorRow("IP 地址", ipEdit_, networkPage));
    networkLayout->addWidget(networkEditorRow("子网掩码", netmaskEdit_, networkPage));
    networkLayout->addWidget(networkEditorRow("网关", gatewayEdit_, networkPage));
    networkLayout->addWidget(networkEditorRow("DNS", dnsEdit_, networkPage));
    networkLayout->addWidget(networkSaveButton, 0, Qt::AlignLeft);
    networkLayout->addStretch();
    connect(dhcpRadio_, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) {
            setNetworkEditorText(ipEdit_, QString());
            setNetworkEditorText(netmaskEdit_, QString());
            setNetworkEditorText(gatewayEdit_, QString());
            setNetworkEditorText(dnsEdit_, QString());
            updateNetworkEditorsEnabled();
        }
    });
    connect(staticRadio_, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) {
            if (ipEdit_) { ipEdit_->clear(); }
            if (netmaskEdit_) { netmaskEdit_->clear(); }
            if (gatewayEdit_) { gatewayEdit_->clear(); }
            if (dnsEdit_) { dnsEdit_->clear(); }
            updateNetworkEditorsEnabled();
        }
    });
    connect(networkSaveButton, &QPushButton::clicked, this, &AdminPanel::requestSaveNetworkConfig);
    updateNetworkEditorsEnabled();

    auto *thresholdPage = new QWidget(tabs);
    auto *thresholdLayout = new QVBoxLayout(thresholdPage);
    thresholdLayout->setContentsMargins(kAdminTabMargin, kAdminTabMargin, kAdminTabMargin, kAdminTabMargin);
    thresholdLayout->setSpacing(14);
    const AppConfig defaultConfig = AppConfig::defaults();

    auto addSlider = [thresholdPage, thresholdLayout](const QString &name, float value, float recommended, QSlider **sliderOut, QLabel **valueOut) {
        auto *row = new QWidget(thresholdPage);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(12);
        auto *label = new QLabel(QString("%1（建议 %2）").arg(name).arg(recommended, 0, 'f', 2), row);
        label->setObjectName("metricName");
        auto *slider = new QSlider(Qt::Horizontal, row);
        slider->setRange(0, 1000);
        slider->setValue(qBound(0, static_cast<int>(value * 1000.0f), 1000));
        auto *valueLabel = new QLabel(QString::number(sliderValue(slider), 'f', 2), row);
        valueLabel->setObjectName("metricValue");
        QObject::connect(slider, &QSlider::valueChanged, valueLabel, [valueLabel](int v) {
            valueLabel->setText(QString::number(static_cast<double>(v) / 1000.0, 'f', 2));
        });
        rowLayout->addWidget(label);
        rowLayout->addWidget(slider, 1);
        rowLayout->addWidget(valueLabel);
        thresholdLayout->addWidget(row);
        *sliderOut = slider;
        *valueOut = valueLabel;
    };

    addSlider("识别相似度阈值", config_.faceCosineThreshold, defaultConfig.faceCosineThreshold, &faceCosineSlider_, &faceCosineValueLabel_);
    addSlider("重复录入阈值", config_.duplicateFaceCosineThreshold, defaultConfig.duplicateFaceCosineThreshold, &duplicateFaceSlider_, &duplicateFaceValueLabel_);
    addSlider("人脸质量阈值", config_.faceQualityThreshold, defaultConfig.faceQualityThreshold, &faceQualitySlider_, &faceQualityValueLabel_);
    addSlider("检测置信阈值", config_.faceDetectThreshold, defaultConfig.faceDetectThreshold, &faceDetectSlider_, &faceDetectValueLabel_);
    addSlider("活体通过阈值", config_.livenessThreshold, defaultConfig.livenessThreshold, &livenessSlider_, &livenessValueLabel_);
    auto *thresholdSaveButton = new QPushButton("保存阈值设置", thresholdPage);
    thresholdSaveButton->setObjectName("toolButton");
    setButtonIcon(thresholdSaveButton, QStringLiteral(":/icons/action/save.svg"));
    auto *thresholdResetButton = new QPushButton("恢复默认配置", thresholdPage);
    thresholdResetButton->setObjectName("toolButton");
    setButtonIcon(thresholdResetButton, QStringLiteral(":/icons/action/refresh.svg"));
    auto *thresholdButtonRow = new QHBoxLayout();
    thresholdButtonRow->addWidget(thresholdSaveButton);
    thresholdButtonRow->addWidget(thresholdResetButton);
    thresholdButtonRow->addStretch();
    thresholdLayout->addLayout(thresholdButtonRow);
    thresholdLayout->addStretch();
    connect(thresholdSaveButton, &QPushButton::clicked, this, &AdminPanel::requestSaveThresholdConfig);
    connect(thresholdResetButton, &QPushButton::clicked, this, &AdminPanel::resetThresholdDefaults);

    auto *audioScrollPage = new QScrollArea(tabs);
    audioScrollPage->setObjectName(QStringLiteral("audioSettingsScroll"));
    audioScrollPage->setWidgetResizable(true);
    audioScrollPage->setFrameShape(QFrame::NoFrame);
    audioScrollPage->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    audioScrollPage->viewport()->setAutoFillBackground(false);

    auto *audioPage = new QWidget(audioScrollPage);
    audioPage->setObjectName(QStringLiteral("audioSettingsScrollContent"));
    auto *audioLayout = new QVBoxLayout(audioPage);
    audioLayout->setContentsMargins(kAdminTabMargin, kAdminTabMargin, kAdminTabMargin, kAdminTabMargin + 8);
    audioLayout->setSpacing(16);

    auto *audioBasicGroup = new QGroupBox(QStringLiteral("音频播报"), audioPage);
    auto *audioBasicLayout = new QGridLayout(audioBasicGroup);
    audioBasicLayout->setContentsMargins(14, 22, 14, 16);
    audioBasicLayout->setHorizontalSpacing(14);
    audioBasicLayout->setVerticalSpacing(14);

    audioEnabledCheck_ = new QCheckBox(QStringLiteral("启用本地 WAV 音频播报"), audioBasicGroup);
    audioEnabledCheck_->setChecked(config_.audioEnabled);
    audioEnabledCheck_->setMinimumHeight(34);

    audioVolumeSlider_ = new QSlider(Qt::Horizontal, audioBasicGroup);
    audioVolumeSlider_->setRange(0, 100);
    audioVolumeSlider_->setValue(qBound(0, config_.audioVolume, 100));
    audioVolumeSlider_->setMinimumHeight(34);
    audioVolumeValueLabel_ = new QLabel(QString::number(audioVolumeSlider_->value()) + QStringLiteral("%"), audioBasicGroup);
    audioVolumeValueLabel_->setObjectName("metricValue");
    audioVolumeValueLabel_->setMinimumWidth(52);
    audioVolumeValueLabel_->setAlignment(Qt::AlignCenter);
    connect(audioVolumeSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (audioVolumeValueLabel_) {
            audioVolumeValueLabel_->setText(QString::number(value) + QStringLiteral("%"));
        }
    });

    auto *audioVolumeNameLabel = new QLabel(QStringLiteral("音量"), audioBasicGroup);
    audioVolumeNameLabel->setObjectName("metricName");
    audioVolumeNameLabel->setMinimumHeight(34);

    audioBasicLayout->addWidget(audioEnabledCheck_, 0, 0, 1, 3);
    audioBasicLayout->addWidget(audioVolumeNameLabel, 1, 0);
    audioBasicLayout->addWidget(audioVolumeSlider_, 1, 1);
    audioBasicLayout->addWidget(audioVolumeValueLabel_, 1, 2);
    audioBasicLayout->setColumnStretch(1, 1);

    auto *promptGroup = new QGroupBox(QStringLiteral("播报事件"), audioPage);
    auto *promptLayout = new QGridLayout(promptGroup);
    promptLayout->setContentsMargins(14, 22, 14, 16);
    promptLayout->setHorizontalSpacing(12);
    promptLayout->setVerticalSpacing(14);

    auto *eventHeader = new QLabel(QStringLiteral("事件"), promptGroup);
    auto *enabledHeader = new QLabel(QStringLiteral("启用"), promptGroup);
    auto *textHeader = new QLabel(QStringLiteral("播报文字"), promptGroup);
    auto *actionHeader = new QLabel(QStringLiteral("操作"), promptGroup);
    eventHeader->setObjectName("metricName");
    enabledHeader->setObjectName("metricName");
    textHeader->setObjectName("metricName");
    actionHeader->setObjectName("metricName");
    eventHeader->setMinimumHeight(30);
    enabledHeader->setMinimumHeight(30);
    textHeader->setMinimumHeight(30);
    actionHeader->setMinimumHeight(30);
    promptLayout->addWidget(eventHeader, 0, 0);
    promptLayout->addWidget(enabledHeader, 0, 1, Qt::AlignCenter);
    promptLayout->addWidget(textHeader, 0, 2);
    promptLayout->addWidget(actionHeader, 0, 3, Qt::AlignCenter);

    auto addPromptRow = [this, promptGroup, promptLayout](int row,
                                                           const QString &key,
                                                           const QString &title,
                                                           bool enabled,
                                                           const QString &text) {
        auto *titleLabel = new QLabel(title, promptGroup);
        titleLabel->setObjectName("metricName");
        titleLabel->setMinimumHeight(36);
        titleLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

        auto *enabledCheck = new QCheckBox(promptGroup);
        enabledCheck->setChecked(enabled);
        enabledCheck->setMinimumHeight(36);

        auto *textEdit = new QLineEdit(text, promptGroup);
        textEdit->setAttribute(Qt::WA_InputMethodEnabled, true);
        textEdit->setMinimumHeight(36);
        textEdit->setPlaceholderText(QStringLiteral("请输入播报文字"));

        auto *testButton = new QPushButton(QStringLiteral("测试"), promptGroup);
        testButton->setObjectName("toolButton");
        testButton->setMinimumHeight(34);
        testButton->setMinimumWidth(76);

        promptLayout->addWidget(titleLabel, row, 0);
        promptLayout->addWidget(enabledCheck, row, 1, Qt::AlignCenter);
        promptLayout->addWidget(textEdit, row, 2);
        promptLayout->addWidget(testButton, row, 3, Qt::AlignCenter);
        promptLayout->setRowMinimumHeight(row, 44);

        audioPromptEnabledChecks_.insert(key, enabledCheck);
        audioPromptTextEdits_.insert(key, textEdit);

        connect(testButton, &QPushButton::clicked, this, [this, key]() {
            requestTestAudioPrompt(key);
        });
    };

    addPromptRow(1, QStringLiteral("verify_passed"), QStringLiteral("验证通过"), config_.promptVerifyPassedEnabled, config_.promptVerifyPassedText);
    addPromptRow(2, QStringLiteral("verify_failed"), QStringLiteral("验证失败"), config_.promptVerifyFailedEnabled, config_.promptVerifyFailedText);
    addPromptRow(3, QStringLiteral("stranger"), QStringLiteral("未录入人员"), config_.promptStrangerEnabled, config_.promptStrangerText);
    addPromptRow(4, QStringLiteral("liveness_failed"), QStringLiteral("活体失败"), config_.promptLivenessFailedEnabled, config_.promptLivenessFailedText);
    addPromptRow(5, QStringLiteral("enroll_success"), QStringLiteral("录入成功"), config_.promptEnrollSuccessEnabled, config_.promptEnrollSuccessText);
    addPromptRow(6, QStringLiteral("enroll_failed"), QStringLiteral("录入失败"), config_.promptEnrollFailedEnabled, config_.promptEnrollFailedText);
    promptLayout->setColumnStretch(2, 1);

    auto *audioSaveButton = new QPushButton(QStringLiteral("保存音频设置"), audioPage);
    audioSaveButton->setObjectName("toolButton");
    audioSaveButton->setMinimumHeight(36);
    setButtonIcon(audioSaveButton, QStringLiteral(":/icons/action/save.svg"));

    audioLayout->addWidget(audioBasicGroup);
    audioLayout->addWidget(promptGroup);
    audioLayout->addWidget(audioSaveButton, 0, Qt::AlignLeft);
    audioLayout->addStretch();
    audioScrollPage->setWidget(audioPage);

    connect(audioSaveButton, &QPushButton::clicked, this, &AdminPanel::requestSaveAudioConfig);

    auto *basicPage = new QWidget(tabs);
    auto *basicLayout = new QVBoxLayout(basicPage);
    basicLayout->setContentsMargins(kAdminTabMargin, kAdminTabMargin, kAdminTabMargin, kAdminTabMargin);
    basicLayout->setSpacing(12);
    auto *basicText = new QLabel(QString("采集帧数：%1\n活体通过帧数：%2\n结果保持：%3 ms\n同人抑制：%4 ms")
        .arg(config_.collectFrameCount)
        .arg(config_.livenessPassRequired)
        .arg(config_.resultHoldMs)
        .arg(config_.sameFaceSuppressMs), basicPage);
    basicText->setObjectName("pageHint");
    basicText->setWordWrap(true);

    auto *dateTimeGroup = new QGroupBox("时间日期设置", basicPage);
    auto *dateTimeLayout = new QHBoxLayout(dateTimeGroup);
    dateTimeLayout->setContentsMargins(12, 16, 12, 12);
    dateTimeLayout->setSpacing(12);
    auto *dateTimeLabel = new QLabel("当前时间", dateTimeGroup);
    dateTimeLabel->setObjectName("metricName");
    dateTimeEdit_ = new QDateTimeEdit(QDateTime::currentDateTime(), dateTimeGroup);
    dateTimeEdit_->setLocale(QLocale(QLocale::Chinese, QLocale::China));
    dateTimeEdit_->setDisplayFormat("yyyy-MM-dd dddd HH:mm:ss");
    dateTimeEdit_->setCalendarPopup(false);
    dateTimeEdit_->setMinimumWidth(220);
    dateTimeEdit_->setAttribute(Qt::WA_InputMethodEnabled, true);
    auto *dateTimeButton = new QPushButton("设置时间日期", dateTimeGroup);
    dateTimeButton->setObjectName("toolButton");
    setButtonIcon(dateTimeButton, QStringLiteral(":/icons/action/clock.svg"));
    dateTimeLayout->addWidget(dateTimeLabel);
    dateTimeLayout->addWidget(dateTimeEdit_, 1);
    dateTimeLayout->addWidget(dateTimeButton);

    auto *adminPasswordGroup = new QGroupBox("管理员密码修改", basicPage);
    auto *adminPasswordLayout = new QHBoxLayout(adminPasswordGroup);
    adminPasswordLayout->setContentsMargins(12, 16, 12, 12);
    adminPasswordLayout->setSpacing(12);
    auto *adminPasswordHint = new QLabel("修改进入管理后台使用的管理员密码", adminPasswordGroup);
    adminPasswordHint->setObjectName("metricName");
    auto *adminPasswordButton = new QPushButton("修改管理员密码", adminPasswordGroup);
    adminPasswordButton->setObjectName("toolButton");
    setButtonIcon(adminPasswordButton, QStringLiteral(":/icons/action/lock.svg"));
    adminPasswordLayout->addWidget(adminPasswordHint, 1);
    adminPasswordLayout->addWidget(adminPasswordButton);

    basicLayout->addWidget(basicText);
    basicLayout->addWidget(dateTimeGroup);
    basicLayout->addWidget(adminPasswordGroup);
    basicLayout->addStretch();
    connect(dateTimeButton, &QPushButton::clicked, this, &AdminPanel::requestApplyDateTime);
    connect(adminPasswordButton, &QPushButton::clicked, this, &AdminPanel::requestAdminPasswordChange);

    tabs->addTab(networkPage, "网络设置");
    tabs->addTab(thresholdPage, "识别阈值");
    tabs->addTab(audioScrollPage, QStringLiteral("音频设置"));
    tabs->addTab(basicPage, "基础设置");

    layout->addWidget(title);
    layout->addWidget(hint);
    layout->addWidget(tabs, 1);
    return page;
}

/**
 * @brief 将最新摄像头预览帧转发给录入页面。
 */
void AdminPanel::setCurrentFrame(const QImage &image, const QImage &previewImage)
{
    if (enrollWidget_) {
        enrollWidget_->setCurrentFrame(image, previewImage);
    }
}

/** @brief 清除录入预览。 */
void AdminPanel::clearEnrollPreview()
{
    if (enrollWidget_) {
        enrollWidget_->clearCurrentFrame();
    }
}

/** @brief 更新录入页面的人脸分析结果。 */
void AdminPanel::setEnrollAnalysis(const QImage &image,
                                   const QImage &previewImage,
                                   const QVector<DetectedFace> &faces,
                                   const QString &message)
{
    if (enrollWidget_) {
        enrollWidget_->setAnalysisFrame(image, previewImage, faces, message);
    }
}

/** @brief 更新录入活体功能状态。 */
void AdminPanel::setEnrollLivenessStatus(bool enabled, const QString &message)
{
    if (enrollWidget_) {
        enrollWidget_->setLivenessStatus(enabled, message);
    }
}

/** @brief 更新录入活体结果。 */
void AdminPanel::setEnrollLivenessResult(bool enabled, const LivenessResult &result, const QString &message)
{
    if (enrollWidget_) {
        enrollWidget_->setLivenessResult(enabled, result, message);
    }
}

/**
 * @brief 兼容旧底库回调；人员行现在由 setPeople() 提供。
 */
void AdminPanel::setGallery(const QVector<FaceRecord> &records)
{
    Q_UNUSED(records)
}

/**
 * @brief 使用数据库人员行刷新管理员人员表。
 */
void AdminPanel::setPeople(const QVector<PersonAdminRecord> &records)
{
    if (!peopleTable_) {
        return;
    }

    peopleTable_->setRowCount(records.size());
    for (int row = 0; row < records.size(); ++row) {
        const PersonAdminRecord &record = records[row];
        peopleTable_->setItem(row, 0, tableItem(QString::number(record.id), record.id));
        peopleTable_->setItem(row, 1, tableItem(record.personNo));
        peopleTable_->setItem(row, 2, tableItem(record.name));
        QTableWidgetItem *permissionItem = tableItem(record.enabled ? "启用" : "禁用");
        permissionItem->setData(Qt::UserRole, record.enabled ? 1 : 0);
        peopleTable_->setItem(row, 3, permissionItem);
        peopleTable_->setItem(row, 4, tableItem(record.deleted ? "已删除" : "正常"));
        peopleTable_->setItem(row, 5, tableItem(record.createdAt.isEmpty() ? record.firstFeatureAt : record.createdAt));
        peopleTable_->setItem(row, 6, tableItem(record.updatedAt));
        peopleTable_->setItem(row, 7, tableItem(QString::number(record.featureCount)));
    }
    peopleTable_->resizeRowsToContents();
    peopleTable_->horizontalHeader()->updateGeometry();
    peopleTable_->viewport()->update();
}

/** @brief 刷新网络人员及其人脸原图对应关系。 */
void AdminPanel::setNetworkPeople(const QJsonArray &records)
{
    networkPeopleRecords_ = records;
    refreshNetworkPeopleTable();
}

void AdminPanel::refreshNetworkPeopleTable()
{
    if (!networkPeopleTable_) {
        return;
    }

    QJsonArray visibleRecords;
    int activeCount = 0;
    int deletedCount = 0;
    for (const QJsonValue &recordValue : networkPeopleRecords_) {
        const QJsonObject record = recordValue.toObject();
        const bool deleted = record.value(QStringLiteral("deleted")).toBool();
        if (deleted) {
            ++deletedCount;
        } else {
            ++activeCount;
        }
        if (deleted == networkDeletedView_) {
            visibleRecords.append(record);
        }
    }

    if (networkActiveButton_) {
        networkActiveButton_->setText(
                    QStringLiteral("正常人员 (%1)").arg(activeCount));
    }
    if (networkDeletedButton_) {
        networkDeletedButton_->setText(
                    QStringLiteral("已删除 (%1)").arg(deletedCount));
    }

    networkPeopleTable_->setUpdatesEnabled(false);
    networkPeopleTable_->clearContents();
    networkPeopleTable_->setRowCount(visibleRecords.size());
    for (int row = 0; row < visibleRecords.size(); ++row) {
        const QJsonObject record = visibleRecords.at(row).toObject();
        QStringList imagePaths;
        const QJsonArray faces =
                record.value(QStringLiteral("faces")).toArray();
        for (const QJsonValue &faceValue : faces) {
            const QJsonObject face = faceValue.toObject();
            const QString imagePath =
                    face.value(QStringLiteral("imagePath")).toString();
            if (!imagePath.isEmpty()) {
                imagePaths.append(imagePath);
            }
        }

        QString state;
        if (record.value(QStringLiteral("deleted")).toBool()) {
            state = QStringLiteral("已删除");
        } else if (!record.value(QStringLiteral("active")).toBool()) {
            state = QStringLiteral("停用");
        } else {
            state = QStringLiteral("正常");
        }
        auto *previewItem = tableItem(QString());
        if (!imagePaths.isEmpty()) {
            previewItem->setIcon(QIcon(imagePaths.first()));
            previewItem->setToolTip(imagePaths.first());
        }
        networkPeopleTable_->setItem(row, 0, previewItem);
        networkPeopleTable_->setItem(
                    row, 1,
                    tableItem(record.value(
                                  QStringLiteral("employeeNo")).toString()));
        networkPeopleTable_->setItem(
                    row, 2,
                    tableItem(record.value(QStringLiteral("name")).toString()));
        networkPeopleTable_->setItem(row, 3, tableItem(state));
        auto *faceCountItem = tableItem(
                    QString::number(record.value(
                                        QStringLiteral("faceCount")).toInt()));
        faceCountItem->setToolTip(QStringLiteral("双击查看该人员的全部人脸"));
        networkPeopleTable_->setItem(row, 4, faceCountItem);
        networkPeopleTable_->setItem(
                    row, 5,
                    tableItem(record.value(
                                  QStringLiteral("personId")).toString()));
        networkPeopleTable_->setItem(
                    row, 6,
                    tableItem(record.value(
                                  QStringLiteral("updatedAt")).toString()));
    }
    networkPeopleTable_->resizeRowsToContents();
    networkPeopleTable_->setUpdatesEnabled(true);
    networkPeopleTable_->viewport()->update();
}

void AdminPanel::setNetworkDeletedView(bool deleted)
{
    networkDeletedView_ = deleted;
    const QString selectedStyle = QStringLiteral(
                "background:#138a72;color:#ffffff;border:2px solid #55d6b8;");
    const QString idleStyle = QStringLiteral(
                "background:#2a3a52;color:#e8f1ff;border:1px solid #58708e;");
    if (networkActiveButton_) {
        networkActiveButton_->setChecked(!deleted);
        networkActiveButton_->setStyleSheet(
                    deleted ? idleStyle : selectedStyle);
    }
    if (networkDeletedButton_) {
        networkDeletedButton_->setChecked(deleted);
        networkDeletedButton_->setStyleSheet(
                    deleted ? selectedStyle : idleStyle);
    }
    refreshNetworkPeopleTable();
}

void AdminPanel::showNetworkPersonFaces(int row)
{
    if (!networkPeopleTable_ || row < 0 ||
        row >= networkPeopleTable_->rowCount()) {
        return;
    }

    const QTableWidgetItem *personIdItem = networkPeopleTable_->item(row, 5);
    if (!personIdItem) {
        return;
    }
    const QString personId = personIdItem->text().trimmed();
    if (personId.isEmpty()) {
        return;
    }

    QJsonObject personRecord;
    for (const QJsonValue &recordValue : networkPeopleRecords_) {
        const QJsonObject record = recordValue.toObject();
        if (record.value(QStringLiteral("personId")).toString() == personId) {
            personRecord = record;
            break;
        }
    }
    if (personRecord.isEmpty()) {
        return;
    }

    const QJsonArray faces =
            personRecord.value(QStringLiteral("faces")).toArray();
    const int faceCount = faces.size();
    const int columns = qMin(3, qMax(1, faceCount));
    const int visibleRows = qMin(2, qMax(1, (faceCount + columns - 1) / columns));
    const int dialogWidth = qBound(340, columns * 164 + 52, 560);
    const int dialogHeight = qBound(260, visibleRows * 184 + 116, 470);

    QDialog dialog(this);
    dialog.setModal(true);
    dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    dialog.setAttribute(Qt::WA_TranslucentBackground);
    dialog.setFixedSize(dialogWidth, dialogHeight);

    auto *outerLayout = new QVBoxLayout(&dialog);
    outerLayout->setContentsMargins(8, 8, 8, 8);

    auto *shell = new QFrame(&dialog);
    shell->setObjectName(QStringLiteral("networkFacesShell"));
    shell->setStyleSheet(QStringLiteral(
        "QFrame#networkFacesShell {"
        "background:#162236;"
        "border:none;"
        "border-radius:14px;"
        "}"
        "QLabel { color:#edf5ff; border:none; background:transparent; }"));
    outerLayout->addWidget(shell);

    auto *shellLayout = new QVBoxLayout(shell);
    shellLayout->setContentsMargins(18, 14, 18, 16);
    shellLayout->setSpacing(12);

    auto *titleRow = new QHBoxLayout();
    auto *title = new QLabel(
                QStringLiteral("%1 的全部人脸（%2）")
                .arg(personRecord.value(QStringLiteral("name")).toString(),
                     QString::number(faceCount)),
                shell);
    title->setStyleSheet(QStringLiteral(
        "font-size:18px;font-weight:700;color:#ffffff;"));
    title->setToolTip(personId);
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), shell);
    closeButton->setFixedSize(68, 36);
    closeButton->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "background:#2a3a52;color:#ffffff;border:none;border-radius:8px;"
        "font-size:15px;font-weight:600;"
        "}"
        "QPushButton:pressed { background:#203047; }"));
    titleRow->addWidget(title, 1);
    titleRow->addWidget(closeButton);
    shellLayout->addLayout(titleRow);

    auto *scrollArea = new QScrollArea(shell);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setStyleSheet(QStringLiteral(
        "QScrollArea { background:transparent;border:none; }"
        "QScrollArea > QWidget > QWidget { background:transparent; }"));

    auto *faceContainer = new QWidget(scrollArea);
    auto *faceGrid = new QGridLayout(faceContainer);
    faceGrid->setContentsMargins(0, 0, 0, 0);
    faceGrid->setHorizontalSpacing(12);
    faceGrid->setVerticalSpacing(12);
    faceGrid->setAlignment(Qt::AlignHCenter | Qt::AlignTop);

    if (faces.isEmpty()) {
        auto *emptyLabel = new QLabel(
                    QStringLiteral("当前人员没有可显示的人脸图像"),
                    faceContainer);
        emptyLabel->setAlignment(Qt::AlignCenter);
        emptyLabel->setStyleSheet(QStringLiteral(
            "color:#b9c8dc;font-size:16px;padding:36px 12px;"));
        faceGrid->addWidget(emptyLabel, 0, 0);
    } else {
        for (int index = 0; index < faces.size(); ++index) {
            const QJsonObject face = faces.at(index).toObject();
            const QString faceHash =
                    face.value(QStringLiteral("faceHash")).toString();
            const QString imagePath =
                    face.value(QStringLiteral("imagePath")).toString();

            auto *faceTile = new QFrame(faceContainer);
            faceTile->setFixedSize(152, 172);
            faceTile->setStyleSheet(QStringLiteral(
                "QFrame {"
                "background:#223149;border:none;border-radius:10px;"
                "}"));
            faceTile->setToolTip(faceHash);

            auto *tileLayout = new QVBoxLayout(faceTile);
            tileLayout->setContentsMargins(10, 10, 10, 8);
            tileLayout->setSpacing(6);

            auto *imageLabel = new QLabel(faceTile);
            imageLabel->setFixedSize(132, 132);
            imageLabel->setAlignment(Qt::AlignCenter);
            imageLabel->setStyleSheet(QStringLiteral(
                "background:#101a2a;color:#91a4bc;"
                "border:none;border-radius:8px;font-size:14px;"));
            const QPixmap facePixmap(imagePath);
            if (!facePixmap.isNull()) {
                imageLabel->setPixmap(
                            facePixmap.scaled(
                                imageLabel->size(),
                                Qt::KeepAspectRatio,
                                Qt::SmoothTransformation));
            } else {
                imageLabel->setText(QStringLiteral("图像未同步"));
            }

            auto *caption = new QLabel(
                        QStringLiteral("人脸 %1").arg(index + 1),
                        faceTile);
            caption->setAlignment(Qt::AlignCenter);
            caption->setStyleSheet(QStringLiteral(
                "color:#edf5ff;font-size:14px;font-weight:600;"));
            tileLayout->addWidget(imageLabel);
            tileLayout->addWidget(caption);
            faceGrid->addWidget(faceTile, index / columns, index % columns);
        }
    }

    scrollArea->setWidget(faceContainer);
    shellLayout->addWidget(scrollArea, 1);

    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);

    const QPoint center = mapToGlobal(rect().center());
    dialog.move(center.x() - dialog.width() / 2,
                center.y() - dialog.height() / 2);
    dialog.exec();
}

void AdminPanel::showNetworkSyncStatus(bool ok, const QString &text)
{
    if (!networkSyncToast_ || text.trimmed().isEmpty()) {
        return;
    }

    Q_UNUSED(ok);
    networkSyncToast_->setStyleSheet(
                QStringLiteral(
                    "QLabel#networkSyncToast {"
                    "background:#26364d;"
                    "color:#ffffff;"
                    "border:none;"
                    "border-radius:9px;"
                    "padding:7px 14px;"
                    "font-size:16px;"
                    "font-weight:600;"
                    "}"));
    networkSyncToast_->setText(text);
    positionNetworkSyncToast();
    networkSyncToast_->show();
    networkSyncToast_->raise();
    if (networkSyncToastTimer_) {
        networkSyncToastTimer_->start(3000);
    }
}

void AdminPanel::positionNetworkSyncToast()
{
    if (!networkPeoplePage_ || !networkSyncToast_) {
        return;
    }
    const int sideMargin = 20;
    const int bottomMargin = 18;
    const int availableWidth =
            qMax(1, networkPeoplePage_->width() - sideMargin * 2);
    const int maxToastWidth = qMin(620, availableWidth);
    const QFontMetrics metrics(networkSyncToast_->font());
    const int naturalWidth = metrics.horizontalAdvance(
                networkSyncToast_->text()) + 36;
    const int toastWidth = qMin(maxToastWidth, qMax(160, naturalWidth));
    const bool needsWrap = naturalWidth > maxToastWidth;
    networkSyncToast_->setWordWrap(needsWrap);

    int toastHeight = 44;
    if (needsWrap) {
        const QRect textRect = metrics.boundingRect(
                    QRect(0, 0, qMax(1, toastWidth - 32), 120),
                    Qt::AlignCenter | Qt::TextWordWrap,
                    networkSyncToast_->text());
        toastHeight = qMin(68, qMax(44, textRect.height() + 18));
    }
    const int x = qMax(sideMargin,
                       (networkPeoplePage_->width() - toastWidth) / 2);
    const int y = qMax(sideMargin,
                       networkPeoplePage_->height() -
                       toastHeight - bottomMargin);
    networkSyncToast_->setGeometry(x, y, toastWidth, toastHeight);
}

void AdminPanel::setPassedVerifyLogs(const QVector<VerifyLogViewRecord> &records)
{
    if (!passedLogTable_) {
        return;
    }

    passedLogsLoaded_ = true;

    // 刷新识别记录时暂停表格重绘，避免逐格插入和自动测量列宽造成 UI 卡顿。
    passedLogTable_->setUpdatesEnabled(false);
    passedLogTable_->clearContents();
    passedLogTable_->setRowCount(records.size());
    for (int row = 0; row < records.size(); ++row) {
        const VerifyLogViewRecord &record = records[row];
        passedLogTable_->setItem(row, 0, tableItem(QString::number(record.logId)));
        passedLogTable_->setItem(row, 1, tableItem(record.result));
        passedLogTable_->setItem(row, 2, tableItem(record.personNo));
        passedLogTable_->setItem(row, 3, tableItem(record.name));
        passedLogTable_->setItem(row, 4, tableItem(record.failReason));
        passedLogTable_->setItem(row, 5, tableItem(QString::number(record.cosine, 'f', 3)));
        passedLogTable_->setItem(row, 6, tableItem(QString::number(record.liveScore, 'f', 3)));
        passedLogTable_->setItem(row, 7, tableItem(record.snapshotPath));
        passedLogTable_->setItem(row, 8, tableItem(record.deviceSn));
        passedLogTable_->setItem(row, 9, tableItem(record.eventType));
        passedLogTable_->setItem(row, 10, tableItem(record.direction));
        passedLogTable_->setItem(row, 11, tableItem(QString::number(record.personId)));
        passedLogTable_->setItem(row, 12, tableItem(record.createdAt));
    }
    passedLogTable_->horizontalHeader()->updateGeometry();
    passedLogTable_->setUpdatesEnabled(true);
    passedLogTable_->viewport()->update();
}

/** @brief 更新人员、特征、日志和磁盘容量摘要。 */
void AdminPanel::setStorageStats(const StorageStats &stats)
{
    storageStats_ = stats;
    updateDeviceInfo();
}

/** @brief 填充同步任务表格。 */
void AdminPanel::setSyncTasks(const QVector<SyncTaskRecord> &records)
{
    if (!syncTaskTable_) {
        return;
    }
    syncTaskTable_->setRowCount(records.size());
    for (int row = 0; row < records.size(); ++row) {
        const SyncTaskRecord &record = records[row];
        syncTaskTable_->setItem(row, 0, tableItem(QString::number(record.id)));
        syncTaskTable_->setItem(row, 1, tableItem(record.taskType));
        syncTaskTable_->setItem(row, 2, tableItem(record.syncStatus));
        syncTaskTable_->setItem(row, 3, tableItem(QString::number(record.retryCount)));
        syncTaskTable_->setItem(row, 4, tableItem(record.lastError));
        syncTaskTable_->setItem(row, 5, tableItem(record.createdAt));
        syncTaskTable_->setItem(row, 6, tableItem(record.payloadJson));
    }
    syncTaskTable_->resizeRowsToContents();
}

/** @brief 填充系统事件表格。 */
void AdminPanel::setSystemEvents(const QVector<SystemEventLog> &records)
{
    if (!systemEventTable_) {
        return;
    }
    systemEventTable_->setRowCount(records.size());
    for (int row = 0; row < records.size(); ++row) {
        const SystemEventLog &record = records[row];
        systemEventTable_->setItem(row, 0, tableItem(QString::number(record.id)));
        systemEventTable_->setItem(row, 1, tableItem(record.eventType));
        systemEventTable_->setItem(row, 2, tableItem(record.level));
        systemEventTable_->setItem(row, 3, tableItem(record.message));
        systemEventTable_->setItem(row, 4, tableItem(record.detail));
        systemEventTable_->setItem(row, 5, tableItem(record.createdAt));
    }
    systemEventTable_->resizeRowsToContents();
}

/**
 * @brief 在管理员开关中体现当前活体策略。
 */
void AdminPanel::setLivenessEnabled(bool enabled)
{
    if (livenessCheck_) {
        livenessCheck_->blockSignals(true);
        livenessCheck_->setChecked(enabled);
        livenessCheck_->blockSignals(false);
    }
}

/**
 * @brief 在当前录入控件中显示录入进度。
 */
void AdminPanel::setStatusText(const QString &text)
{
    if (enrollWidget_) {
        enrollWidget_->setStatusText(text);
    }
}

/** @return 当前选中页面或录入模式需要摄像头时返回 true。 */
bool AdminPanel::cameraRequired() const
{
    return stack_
        && stack_->currentIndex() == kPersonManagementPageIndex
        && (!personTabs_ || personTabs_->currentIndex() == kNewPersonTabIndex)
        && (!enrollWidget_ || enrollWidget_->cameraRequired());
}

/** @brief 重新计算当前页面摄像头需求并在变化时发信号。 */
void AdminPanel::updateCameraRequirement()
{
    emit cameraRequiredChanged(cameraRequired());
}

/** @brief 校验选择后请求修改人员编号。 */
void AdminPanel::requestModifySelectedPersonNo()
{
    const qint64 id = selectedPersonId();
    if (id <= 0) {
        AppMessageDialog::information(this, "人员", "请先选择一名人员");
        return;
    }

    bool ok = false;
    const QString newNo = QInputDialog::getText(
        this,
        "修改人员编号",
        "新的人员编号：",
        QLineEdit::Normal,
        selectedPersonNo(),
        &ok).trimmed();
    if (!ok || newNo.isEmpty()) {
        return;
    }

    emit personNoChangeRequested(id, newNo);
}

/** @brief 确认后请求删除选中人员。 */
void AdminPanel::requestDeleteSelectedPerson()
{
    const qint64 id = selectedPersonId();
    if (id <= 0) {
        AppMessageDialog::information(this, "人员", "请先选择一名人员");
        return;
    }

    const QString personNo = selectedPersonNo();
    const bool confirmed = AppMessageDialog::confirm(
        this,
        "删除人员",
        QString("确定将人员 %1 标记为已删除吗？该人员不会再进入识别底库。").arg(personNo),
        "删除",
        "取消");
    if (!confirmed) {
        return;
    }

    emit personDeleteRequested(id);
}

/** @brief 请求切换选中人员启用状态。 */
void AdminPanel::requestToggleSelectedPersonEnabled()
{
    const qint64 id = selectedPersonId();
    if (id <= 0) {
        AppMessageDialog::information(this, "人员", "请先选择一名人员");
        return;
    }

    const bool enableNext = !selectedPersonEnabled();
    emit personEnabledChangeRequested(id, enableNext);
}

/** @brief 提示插入U盘并请求在后台导入 person 目录中的 XLS/XLSX。 */
void AdminPanel::requestImportPeopleFromUsb()
{
    AppMessageDialog::information(
                this,
                QStringLiteral("导入网络人员"),
                QStringLiteral("请插入U盘，并确认U盘根目录下存在 person 文件夹，"
                               "person 文件夹中放置导出的 xls 或 xlsx 人员表。\n\n"
                               "点击确定后，系统将在后台等待并自动导入。"));
    emit peopleImportRequested();
}

void AdminPanel::setPersonImportBusy(bool busy, const QString &message)
{
    if (importPeopleButton_) {
        importPeopleButton_->setEnabled(!busy);
        importPeopleButton_->setText(busy ? QStringLiteral("正在导入…")
                                          : QStringLiteral("导入人员表"));
    }
    if (exportPeopleButton_) exportPeopleButton_->setEnabled(!busy);
    if (!message.isEmpty()) setStatusText(message);
}

/** @brief 提示插入U盘并在同一弹窗内选择导出人员范围。 */
void AdminPanel::requestExportPeopleToUsb()
{
    AppMessageDialog::information(
                this,
                QStringLiteral("导出人员"),
                QStringLiteral("请插入可写U盘。点击确定后请选择要导出的人员范围，"
                               "系统会在后台挂载U盘并保存为固定的 xlsx 文件。"));
    const int selected = AppMessageDialog::choose(
                this,
                QStringLiteral("选择导出人员"),
                QStringLiteral("请选择要导出的人员范围："),
                QStringList() << QStringLiteral("网络人员")
                              << QStringLiteral("本地人员")
                              << QStringLiteral("全部人员"));
    if (selected < 0) return;
    if (selected == 0) {
        emit peopleExportRequested(QStringLiteral("network"));
    } else if (selected == 1) {
        emit peopleExportRequested(QStringLiteral("local"));
    } else {
        emit peopleExportRequested(QStringLiteral("all"));
    }
}

void AdminPanel::setPersonExportBusy(bool busy, const QString &message)
{
    if (exportPeopleButton_) {
        exportPeopleButton_->setEnabled(!busy);
        exportPeopleButton_->setText(busy ? QStringLiteral("正在导出…")
                                          : QStringLiteral("导出人员表"));
    }
    if (importPeopleButton_) importPeopleButton_->setEnabled(!busy);
    if (!message.isEmpty()) setStatusText(message);
}

void AdminPanel::showNetworkPeoplePage()
{
    if (personTabs_) personTabs_->setCurrentIndex(kNetworkPeopleTabIndex);
}

/** @brief 将选中人员信息带入录入页重新采集。 */
void AdminPanel::requestReEnrollSelectedPerson()
{
    const qint64 id = selectedPersonId();
    if (id <= 0 || !peopleTable_ || peopleTable_->currentRow() < 0) {
        AppMessageDialog::information(this, "重新录入人脸", "请先选择一名人员");
        return;
    }
    const QString personNo = selectedPersonNo();
    QTableWidgetItem *nameItem = peopleTable_->item(peopleTable_->currentRow(), 2);
    const QString name = nameItem ? nameItem->text() : QString();
    if (enrollWidget_) {
        enrollWidget_->setManualCaptureMode();
        enrollWidget_->setPersonFields(personNo, name);
        enrollWidget_->setStatusText("重新录入人脸：保留原人员ID，保存后新增一张人脸模板");
    }
    if (personTabs_) {
        personTabs_->setCurrentIndex(kNewPersonTabIndex);
    }
    updateCameraRequirement();
}

/** @brief 收集筛选控件并请求查询验证日志。 */
void AdminPanel::requestQueryVerifyLogs()
{
    VerifyLogFilter filter;
    filter.result = verifyResultCombo_ ? verifyResultCombo_->currentData().toString() : QStringLiteral("all");
    filter.personNo = verifyPersonNoEdit_ ? verifyPersonNoEdit_->text().trimmed() : QString();
    filter.name = verifyNameEdit_ ? verifyNameEdit_->text().trimmed() : QString();
    filter.startTime = verifyStartEdit_ ? verifyStartEdit_->dateTime() : QDateTime();
    filter.endTime = verifyEndEdit_ ? verifyEndEdit_->dateTime() : QDateTime();
    filter.limit = 1000;
    emit passedLogsRefreshRequested(filter);
}

/** @brief 将阈值滑块值转换为浮点配置并请求保存。 */
void AdminPanel::requestSaveThresholdConfig()
{
    config_.faceCosineThreshold = sliderValue(faceCosineSlider_);
    config_.duplicateFaceCosineThreshold = sliderValue(duplicateFaceSlider_);
    config_.faceQualityThreshold = sliderValue(faceQualitySlider_);
    config_.faceDetectThreshold = sliderValue(faceDetectSlider_);
    config_.livenessThreshold = sliderValue(livenessSlider_);

    if (enrollWidget_) {
        enrollWidget_->setAutoEnrollThresholds(config_.faceQualityThreshold,
                                               config_.faceDetectThreshold,
                                               config_.livenessThreshold);
    }

    emit thresholdConfigSaveRequested(
        config_.faceCosineThreshold,
        config_.duplicateFaceCosineThreshold,
        config_.faceQualityThreshold,
        config_.faceDetectThreshold,
        config_.livenessThreshold);
}

/** @brief 将识别和活体阈值恢复为内置默认值。 */
void AdminPanel::resetThresholdDefaults()
{
    const AppConfig defaults = AppConfig::defaults();
    if (faceCosineSlider_) {
        faceCosineSlider_->setValue(qBound(0, static_cast<int>(defaults.faceCosineThreshold * 1000.0f), 1000));
    }
    if (duplicateFaceSlider_) {
        duplicateFaceSlider_->setValue(qBound(0, static_cast<int>(defaults.duplicateFaceCosineThreshold * 1000.0f), 1000));
    }
    if (faceQualitySlider_) {
        faceQualitySlider_->setValue(qBound(0, static_cast<int>(defaults.faceQualityThreshold * 1000.0f), 1000));
    }
    if (faceDetectSlider_) {
        faceDetectSlider_->setValue(qBound(0, static_cast<int>(defaults.faceDetectThreshold * 1000.0f), 1000));
    }
    if (livenessSlider_) {
        livenessSlider_->setValue(qBound(0, static_cast<int>(defaults.livenessThreshold * 1000.0f), 1000));
    }
    requestSaveThresholdConfig();
}

/** @brief 校验网络输入并请求保存。 */
void AdminPanel::requestSaveNetworkConfig()
{
    config_.networkDhcp = dhcpRadio_ ? dhcpRadio_->isChecked() : true;
    if (config_.networkDhcp) {
        const NetworkSnapshot snapshot = currentNetworkSnapshot();
        config_.networkStaticIp = snapshot.ip;
        config_.networkNetmask = snapshot.netmask;
        config_.networkGateway = snapshot.gateway;
        config_.networkDns = snapshot.dns;
        setNetworkEditorText(ipEdit_, config_.networkStaticIp);
        setNetworkEditorText(netmaskEdit_, config_.networkNetmask);
        setNetworkEditorText(gatewayEdit_, config_.networkGateway);
        setNetworkEditorText(dnsEdit_, config_.networkDns);
        qDebug().noquote() << "DHCP 当前网络快照："
                           << "iface=" << snapshot.ifaceName
                           << "ip=" << snapshot.ip
                           << "netmask=" << snapshot.netmask
                           << "gateway=" << snapshot.gateway
                           << "dns=" << snapshot.dns;
    } else {
        config_.networkStaticIp = networkEditValue(ipEdit_);
        config_.networkNetmask = networkEditValue(netmaskEdit_);
        config_.networkGateway = networkEditValue(gatewayEdit_);
        config_.networkDns = networkEditValue(dnsEdit_);
        setNetworkEditorText(ipEdit_, config_.networkStaticIp);
        setNetworkEditorText(netmaskEdit_, config_.networkNetmask);
        setNetworkEditorText(gatewayEdit_, config_.networkGateway);
        setNetworkEditorText(dnsEdit_, config_.networkDns);
    }
    updateNetworkEditorsEnabled();

    qDebug().noquote() << "网络设置请求保存："
                       << "dhcp=" << config_.networkDhcp
                       << "ip=" << config_.networkStaticIp
                       << "netmask=" << config_.networkNetmask
                       << "gateway=" << config_.networkGateway
                       << "dns=" << config_.networkDns
                       << "staticEditorsEnabled=" << (ipEdit_ ? ipEdit_->isEnabled() : false);

    emit networkConfigSaveRequested(
        config_.networkDhcp,
        config_.networkStaticIp,
        config_.networkNetmask,
        config_.networkGateway,
        config_.networkDns);
}

/** @brief 把音频控件内容写入 config_ 快照。 */
void AdminPanel::syncAudioEditorsToConfig()
{
    config_.audioEnabled = audioEnabledCheck_ ? audioEnabledCheck_->isChecked() : config_.audioEnabled;
    config_.audioPlayer = QStringLiteral("aplay");
    if (config_.audioDevice.trimmed().isEmpty()) {
        config_.audioDevice = QStringLiteral("plughw:CARD=rockchiprk809co,DEV=0");
    }
    if (config_.audioDir.trimmed().isEmpty()) {
        config_.audioDir = QStringLiteral("./audio");
    }
    config_.audioVolume = audioVolumeSlider_ ? qBound(0, audioVolumeSlider_->value(), 100) : config_.audioVolume;
    if (config_.audioMixerControl.trimmed().isEmpty()) {
        config_.audioMixerControl = QStringLiteral("Playback");
    }

    auto checkValue = [this](const QString &key, bool fallback) {
        QCheckBox *check = audioPromptEnabledChecks_.value(key, nullptr);
        return check ? check->isChecked() : fallback;
    };
    auto textValue = [this](const QString &key, const QString &fallback) {
        QLineEdit *edit = audioPromptTextEdits_.value(key, nullptr);
        return edit ? edit->text().trimmed() : fallback;
    };

    config_.promptVerifyPassedEnabled = checkValue(QStringLiteral("verify_passed"), config_.promptVerifyPassedEnabled);
    config_.promptVerifyPassedText = textValue(QStringLiteral("verify_passed"), config_.promptVerifyPassedText);

    config_.promptVerifyFailedEnabled = checkValue(QStringLiteral("verify_failed"), config_.promptVerifyFailedEnabled);
    config_.promptVerifyFailedText = textValue(QStringLiteral("verify_failed"), config_.promptVerifyFailedText);

    config_.promptStrangerEnabled = checkValue(QStringLiteral("stranger"), config_.promptStrangerEnabled);
    config_.promptStrangerText = textValue(QStringLiteral("stranger"), config_.promptStrangerText);

    config_.promptLivenessFailedEnabled = checkValue(QStringLiteral("liveness_failed"), config_.promptLivenessFailedEnabled);
    config_.promptLivenessFailedText = textValue(QStringLiteral("liveness_failed"), config_.promptLivenessFailedText);

    config_.promptEnrollSuccessEnabled = checkValue(QStringLiteral("enroll_success"), config_.promptEnrollSuccessEnabled);
    config_.promptEnrollSuccessText = textValue(QStringLiteral("enroll_success"), config_.promptEnrollSuccessText);

    config_.promptEnrollFailedEnabled = checkValue(QStringLiteral("enroll_failed"), config_.promptEnrollFailedEnabled);
    config_.promptEnrollFailedText = textValue(QStringLiteral("enroll_failed"), config_.promptEnrollFailedText);
}

/** @brief 同步音频编辑器后请求保存配置。 */
void AdminPanel::requestSaveAudioConfig()
{
    syncAudioEditorsToConfig();
    emit audioConfigSaveRequested(config_);
}

/** @brief 请求测试指定音频提示。 */
void AdminPanel::requestTestAudioPrompt(const QString &promptKey)
{
    syncAudioEditorsToConfig();
    emit audioConfigSaveRequested(config_);
    emit audioTestPlayRequested(promptKey);
}

/** @brief 打开统一密码弹窗并请求修改管理员密码。 */
void AdminPanel::requestAdminPasswordChange()
{
    hideEmbeddedKeyboard();

    QString oldPassword;
    QString newPassword;
    const bool accepted = AppPasswordDialog::getPasswordChange(this, &oldPassword, &newPassword);
    if (!accepted) {
        return;
    }

    emit adminPasswordChangeRequested(oldPassword, newPassword);
}

/** @brief 请求将日期时间编辑器值应用到系统。 */
void AdminPanel::requestApplyDateTime()
{
    if (!dateTimeEdit_) {
        return;
    }

    const QString value = dateTimeEdit_->dateTime().toString("yyyy-MM-dd HH:mm:ss");
    QProcess dateProcess;
    dateProcess.start("/bin/date", QStringList() << "-s" << value);
    const bool finished = dateProcess.waitForFinished(3000);
    const bool ok = finished && dateProcess.exitStatus() == QProcess::NormalExit && dateProcess.exitCode() == 0;
    QString message;
    if (ok) {
        QProcess::startDetached("/sbin/hwclock", QStringList() << "-w");
        message = "时间日期已设置";
    } else {
        const QString error = QString::fromLocal8Bit(dateProcess.readAllStandardError()).trimmed();
        message = error.isEmpty() ? "时间日期设置失败" : ("时间日期设置失败：" + error);
    }

    setStatusText(message);
    if (!ok) {
        AppMessageDialog::warning(this, "设置", message);
    }
}

/** @brief 根据 DHCP/静态单选状态启用网络输入框。 */
void AdminPanel::updateNetworkEditorsEnabled()
{
    const bool staticMode = staticRadio_ && staticRadio_->isChecked();
    const QList<QLineEdit *> edits = {ipEdit_, netmaskEdit_, gatewayEdit_, dnsEdit_};
    for (QLineEdit *edit : edits) {
        if (!edit) {
            continue;
        }
        if (!staticMode && edit->text().trimmed().isEmpty()) {
            edit->setText("--");
        }
        edit->setEnabled(true);
        edit->setReadOnly(!staticMode);
        edit->setFocusPolicy(staticMode ? Qt::StrongFocus : Qt::NoFocus);
        edit->setPlaceholderText(staticMode ? QString() : QStringLiteral("--"));
    }
}

/** @brief 刷新序列号、资源占用、温度和容量等设备信息。 */
void AdminPanel::updateDeviceInfo()
{
    // 0) 网络状态
    if (serialLabel_) {
        serialLabel_->setText(deviceSerialNumber());
    }
    if (ipLabel_) {
        ipLabel_->setText(firstIpv4Address());
    }
    if (macLabel_) {
        macLabel_->setText(firstMacAddress());
    }

    // 1) 版本信息
    if (versionLabel_) {
        const QString version = QCoreApplication::applicationVersion().isEmpty()
            ? "FaceGate V1.0.0"
            : "FaceGate V" + QCoreApplication::applicationVersion();
        versionLabel_->setText(version);
    }
    if (uptimeLabel_) {
        const qint64 startMs = qApp->property("appStartMsecsSinceEpoch").toLongLong();
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        uptimeLabel_->setText(startMs > 0 ? formatRunDuration((nowMs - startMs) / 1000) : "--");
    }

    // 2) 人脸数据信息
    if (peopleCapacityLabel_) {
        peopleCapacityLabel_->setText(QString("%1/3000").arg(storageStats_.personCount));
    }
    if (faceCapacityLabel_) {
        faceCapacityLabel_->setText(QString("%1/3000").arg(storageStats_.faceFeatureCount));
    }
    if (verifyLogCountLabel_) {
        verifyLogCountLabel_->setText(QString::number(storageStats_.verifyLogCount));
    }

    // 3) 存储容量状态
    if (diskCapacityLabel_) {
        const QStorageInfo storage = storageForApplicationData();
        const qint64 total = storage.bytesTotal();
        const qint64 free = storage.bytesFree();
        const qint64 used = total > 0 ? qBound<qint64>(0, total - free, total) : 0;
        diskCapacityLabel_->setText(total > 0
            ? QString("%1 / %2").arg(formatBytes(used)).arg(formatBytes(total))
            : "--");
        diskCapacityLabel_->setToolTip(storage.rootPath());
    }

    // 4) CPU 占用状态
    if (cpuUsageLabel_) {
        QFile file("/proc/stat");
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QList<QByteArray> fields = file.readLine().simplified().split(' ');
            if (fields.size() >= 8 && fields.first() == "cpu") {
                qint64 values[7] = {0, 0, 0, 0, 0, 0, 0};
                for (int i = 0; i < 7; ++i) {
                    values[i] = fields.value(i + 1).toLongLong();
                }
                const qint64 idle = values[3] + values[4];
                const qint64 total = values[0] + values[1] + values[2] + values[3] + values[4] + values[5] + values[6];
                const qint64 totalDelta = total - lastCpuTotal_;
                const qint64 idleDelta = idle - lastCpuIdle_;
                if (lastCpuTotal_ > 0 && totalDelta > 0) {
                    const double usage = (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta)) * 100.0;
                    cpuUsageLabel_->setText(QString("%1%").arg(qBound(0.0, usage, 100.0), 0, 'f', 1));
                } else {
                    cpuUsageLabel_->setText("--");
                }
                lastCpuTotal_ = total;
                lastCpuIdle_ = idle;
            }
        }
    }

    // 4) 运行内存占用状态
    if (memoryUsageLabel_) {
        QFile file("/proc/meminfo");
        QString meminfoText;
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            meminfoText = QString::fromLocal8Bit(file.readAll());
        } else {
            qWarning().noquote() << "读取 /proc/meminfo 失败：" << file.errorString();
        }

        if (meminfoText.trimmed().isEmpty()) {
            QProcess cat;
            cat.start("/bin/cat", QStringList() << "/proc/meminfo");
            if (cat.waitForFinished(800)) {
                meminfoText = QString::fromLocal8Bit(cat.readAllStandardOutput());
                qWarning().noquote() << "QFile 读取 meminfo 为空，已尝试 /bin/cat 兜底，输出长度：" << meminfoText.size();
            } else {
                qWarning().noquote() << "/bin/cat /proc/meminfo 超时或失败：" << cat.errorString();
            }
        }


        qint64 totalKb = meminfoValueKb(meminfoText, "MemTotal");
        qint64 availableKb = meminfoValueKb(meminfoText, "MemAvailable");
        const qint64 freeKb = meminfoValueKb(meminfoText, "MemFree");
        const qint64 buffersKb = meminfoValueKb(meminfoText, "Buffers");
        const qint64 cachedKb = meminfoValueKb(meminfoText, "Cached");

        if (availableKb <= 0) {
            availableKb = freeKb + buffersKb + cachedKb;
        }
        

        if (totalKb > 0) {
            const qint64 usedKb = qBound<qint64>(0, totalKb - availableKb, totalKb);
            const double usedPercent = static_cast<double>(usedKb) * 100.0 / static_cast<double>(totalKb);
            memoryUsageLabel_->setText(QString("%1% (%2/%3)")
                .arg(usedPercent, 0, 'f', 1)
                .arg(formatBytes(usedKb * 1024))
                .arg(formatBytes(totalKb * 1024)));
        } else {
            memoryUsageLabel_->setText("--");
        }
    }

    // 5) 温度、NPU占用状态
    if (temperatureLabel_) {
        temperatureLabel_->setText(currentTemperature());
    }
    if (npuUsageLabel_) {
        npuUsageLabel_->setText(npuUsage());
    }
}

/** @return 人员表格当前行的人员主键；无选择时返回 0。 */
qint64 AdminPanel::selectedPersonId() const
{
    if (!peopleTable_ || peopleTable_->currentRow() < 0) {
        return 0;
    }
    QTableWidgetItem *item = peopleTable_->item(peopleTable_->currentRow(), 0);
    return item ? item->data(Qt::UserRole).toLongLong() : 0;
}

/** @return 人员表格当前行的业务编号。 */
QString AdminPanel::selectedPersonNo() const
{
    if (!peopleTable_ || peopleTable_->currentRow() < 0) {
        return QString();
    }
    QTableWidgetItem *item = peopleTable_->item(peopleTable_->currentRow(), 1);
    return item ? item->text() : QString();
}

/** @return 人员表格当前行的启用状态。 */
bool AdminPanel::selectedPersonEnabled() const
{
    if (!peopleTable_ || peopleTable_->currentRow() < 0) {
        return false;
    }
    QTableWidgetItem *item = peopleTable_->item(peopleTable_->currentRow(), 3);
    return item && item->data(Qt::UserRole).toInt() != 0;
}

/** @return 人员表格是否应包含软删除记录。 */
bool AdminPanel::includeDeletedPeople() const
{
    return showDeletedPeopleCheck_ && showDeletedPeopleCheck_->isChecked();
}

/**
 * @brief 仅在输入框需要输入时，把 QML 键盘作为管理窗口底部浮层显示。
 */
void AdminPanel::updateEmbeddedKeyboardVisibility()
{
    if (!keyboardWidget_ || !contentShell_) {
        return;
    }

    QWidget *focus = QApplication::focusWidget();
    const bool wantsInput = isVisible()
        && focus
        && isAncestorOf(focus)
        && focus->testAttribute(Qt::WA_InputMethodEnabled);

    if (wantsInput) {
        const int availableHeight = contentShell_->height();
        // contentShell_ 的布局最小宽度一旦被子页面撑大，geometry().width() 可能超过屏幕可视宽度。
        // 键盘只按管理窗口内实际可见的右侧区域显示，避免再次被超宽页面带偏。
        const int visibleWidth = qMax(0, width() - contentShell_->geometry().x());
        const int availableWidth = qMin(contentShell_->width(), visibleWidth);
        // 管理后台的键盘作为底部浮层显示。高度固定为 300，
        // QML 内部会把 360 高的原始键盘等比缩放到当前高度，
        // 这样既不会裁剪按键，也不会遮住大半个管理页面。
        const int height = qMin(kEmbeddedKeyboardHeight, qMax(0, availableHeight));
        keyboardWidget_->setGeometry(0, qMax(0, availableHeight - height), availableWidth, height);
        keyboardWidget_->setFixedHeight(height);
        keyboardWidget_->setVisible(height > 0);
        keyboardWidget_->raise();
        if (QGuiApplication::inputMethod()) {
            QGuiApplication::inputMethod()->show();
        }
    } else {
        hideEmbeddedKeyboard();
    }
}

/** @brief 隐藏内嵌虚拟键盘。 */
void AdminPanel::hideEmbeddedKeyboard()
{
    if (keyboardWidget_) {
        keyboardWidget_->hide();
    }
    if (QGuiApplication::inputMethod()) {
        QGuiApplication::inputMethod()->hide();
    }
}

/** @return widget 属于内嵌键盘对象树时返回 true。 */
bool AdminPanel::clickedWidgetIsEmbeddedKeyboard(QWidget *widget) const
{
    return keyboardWidget_
        && widget
        && (widget == keyboardWidget_ || keyboardWidget_->isAncestorOf(widget));
}

/** @return widget 是可编辑文本控件时返回 true。 */
bool AdminPanel::clickedWidgetIsTextEditor(QWidget *widget) const
{
    if (!widget || !isAncestorOf(widget)) {
        return false;
    }
    QWidget *cursor = widget;
    while (cursor && cursor != this) {
        if (cursor->inherits("QLineEdit") || cursor->testAttribute(Qt::WA_InputMethodEnabled)) {
            return true;
        }
        cursor = cursor->parentWidget();
    }
    return false;
}

/** @brief 管理文本焦点、内嵌键盘点击和键盘可见性。 */
bool AdminPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == contentShell_ && event->type() == QEvent::Resize) {
        updateEmbeddedKeyboardVisibility();
    }
    if (watched == networkPeoplePage_ && event->type() == QEvent::Resize) {
        positionNetworkSyncToast();
    }

    if (event->type() == QEvent::MouseButtonPress) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        QWidget *clicked = QApplication::widgetAt(mouseEvent->globalPos());
        if (!clicked) {
            clicked = qobject_cast<QWidget *>(watched);
        }

        if (clickedWidgetIsTextEditor(clicked)) {
            QTimer::singleShot(0, this, [this]() {
                if (QGuiApplication::inputMethod()) {
                    QGuiApplication::inputMethod()->show();
                }
                updateEmbeddedKeyboardVisibility();
            });
        } else if (keyboardWidget_ && keyboardWidget_->isVisible() &&
                   !clickedWidgetIsEmbeddedKeyboard(clicked)) {
            QWidget *focus = QApplication::focusWidget();
            if (focus && isAncestorOf(focus) && focus->testAttribute(Qt::WA_InputMethodEnabled)) {
                focus->clearFocus();
            }
            hideEmbeddedKeyboard();
        }
    }

    return QDialog::eventFilter(watched, event);
}

/**
 * @brief 通知 MainWindow 当前自动删除对话框正在关闭。
 */
void AdminPanel::closeEvent(QCloseEvent *event)
{
    qApp->removeEventFilter(this);
    hideEmbeddedKeyboard();
    emit closed();
    QDialog::closeEvent(event);
}
