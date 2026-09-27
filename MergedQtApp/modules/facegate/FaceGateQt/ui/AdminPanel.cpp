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
#include "components/quick/QuickNavigationWidget.h"
#include "shell/QuickUiRuntime.h"
#include "AppPasswordDialog.h"
#include "UsbPersonImportSource.h"
#include "common/responsive_ui.h"
#include "common/storage_policy.h"

#include <QtQml/QQmlError>
#include <QAbstractButton>
#include <QAbstractAnimation>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSocket>
#include <QApplication>
#include <QBoxLayout>
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
#include <QEasingCurve>
#include <QFrame>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGroupBox>
#include <QGridLayout>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QHostAddress>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLineEdit>
#include <QLocale>
#include <QNetworkAddressEntry>
#include <QPalette>
#include <QNetworkInterface>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPushButton>
#include <QProcess>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QPixmap>
#include <QIcon>
#include <QRadioButton>
#include <QRegExp>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSize>
#include <QSizePolicy>
#include "components/settings/displayrotationpage.h"
#include <QShowEvent>
#include <QSpinBox>
#include <QStorageInfo>
#include <QStackedWidget>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>

namespace {
/** @brief 与移动端一致的管理设置滑动开关。 */
class AdminToggleSwitch final : public QCheckBox
{
public:
    explicit AdminToggleSwitch(QWidget *parent = nullptr)
        : QCheckBox(parent), animation_(this)
    {
        setText(QString());
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setFixedSize(66, 38);

        animation_.setDuration(190);
        animation_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&animation_, &QVariantAnimation::valueChanged,
                this, [this](const QVariant &value) {
            position_ = value.toReal();
            update();
        });
        connect(this, &QCheckBox::toggled, this, [this](bool checked) {
            animation_.stop();
            animation_.setStartValue(position_);
            animation_.setEndValue(checked ? 1.0 : 0.0);
            animation_.start();
        });
    }

protected:
    bool hitButton(const QPoint &position) const override
    {
        return rect().contains(position);
    }

    void paintEvent(QPaintEvent *) override
    {
        if (animation_.state() != QAbstractAnimation::Running) {
            position_ = isChecked() ? 1.0 : 0.0;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF trackRect(1.0, 2.0, width() - 2.0, height() - 4.0);
        painter.setPen(Qt::NoPen);
        const QColor offColor = isEnabled() ? QColor("#566477") : QColor("#3f4855");
        const QColor onColor = isEnabled() ? QColor("#36d4c7") : QColor("#4a6770");
        const auto blend = [this](int off, int on) {
            return qRound(off + (on - off) * position_);
        };
        painter.setBrush(QColor(blend(offColor.red(), onColor.red()),
                                blend(offColor.green(), onColor.green()),
                                blend(offColor.blue(), onColor.blue())));
        painter.drawRoundedRect(trackRect, trackRect.height() / 2.0,
                                trackRect.height() / 2.0);

        const qreal knobDiameter = trackRect.height() - 6.0;
        const qreal knobStart = trackRect.left() + 3.0;
        const qreal knobEnd = trackRect.right() - knobDiameter - 3.0;
        const qreal knobX = knobStart + (knobEnd - knobStart) * position_;
        const QRectF knobRect(knobX, trackRect.top() + 3.0,
                              knobDiameter, knobDiameter);
        painter.setBrush(QColor("#ffffff"));
        painter.drawEllipse(knobRect);
    }

private:
    QVariantAnimation animation_;
    qreal position_ = 0.0;
};

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

/** @brief 为管理页及滚动视口设置不依赖桌面主题的深色调色板。 */
void applyAdminSurface(QWidget *widget, const QColor &background = QColor("#162236"))
{
    if (!widget) {
        return;
    }

    widget->setProperty("adminSurface", true);
    widget->setAttribute(Qt::WA_StyledBackground, true);
    widget->setAutoFillBackground(true);
    QPalette palette = widget->palette();
    palette.setColor(QPalette::Window, background);
    palette.setColor(QPalette::Base, background);
    palette.setColor(QPalette::AlternateBase, QColor("#1b2a40"));
    palette.setColor(QPalette::WindowText, QColor("#e7eef8"));
    palette.setColor(QPalette::Text, QColor("#eaf2fd"));
    palette.setColor(QPalette::ButtonText, QColor("#eaf2fd"));
    palette.setColor(QPalette::Highlight, QColor("#2b7fff"));
    palette.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    widget->setPalette(palette);
}

/** @return 管理后台滚动条使用的统一深色样式。 */
QString adminScrollBarStyle()
{
    return QStringLiteral(
        "QScrollBar:vertical{background:#162236;width:12px;margin:0;border:none;}"
        "QScrollBar:horizontal{background:#162236;height:12px;margin:0;border:none;}"
        "QScrollBar::handle:vertical{background:#405571;min-height:30px;"
        "border:none;border-radius:6px;margin:1px;}"
        "QScrollBar::handle:horizontal{background:#405571;min-width:30px;"
        "border:none;border-radius:6px;margin:1px;}"
        "QScrollBar::handle:hover{background:#5a7292;}"
        "QScrollBar::add-page,QScrollBar::sub-page{background:#162236;border:none;}"
        "QScrollBar::add-line,QScrollBar::sub-line{width:0;height:0;"
        "background:transparent;border:none;}"
        "QScrollBar::up-arrow,QScrollBar::down-arrow,"
        "QScrollBar::left-arrow,QScrollBar::right-arrow{width:0;height:0;}");
}

/** @brief 将深色滚动条样式直接应用到实际滚动区域。 */
void applyAdminScrollBars(QAbstractScrollArea *area)
{
    if (!area) {
        return;
    }
    const QString style = adminScrollBarStyle();
    if (area->verticalScrollBar()) {
        area->verticalScrollBar()->setStyleSheet(style);
    }
    if (area->horizontalScrollBar()) {
        area->horizontalScrollBar()->setStyleSheet(style);
    }
}

/** @brief 为输入框设置不受 Weston 原生主题覆盖的深色样式。 */
void applyAdminEditorStyle(QLineEdit *edit)
{
    if (!edit) {
        return;
    }

    QPalette palette = edit->palette();
    palette.setColor(QPalette::Base, QColor("#111c2c"));
    palette.setColor(QPalette::Text, QColor("#eef5ff"));
    palette.setColor(QPalette::Highlight, QColor("#2b7fff"));
    palette.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    palette.setColor(QPalette::Disabled, QPalette::Base, QColor("#182538"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#91a0b3"));
    edit->setPalette(palette);
    edit->setAttribute(Qt::WA_StyledBackground, true);
    edit->setStyleSheet(QStringLiteral(
        "QLineEdit{background-color:#111c2c;color:#eef5ff;"
        "border:1px solid #394b63;border-radius:10px;padding:6px 9px;"
        "selection-background-color:#2b7fff;selection-color:#ffffff;}"
        "QLineEdit:focus{background-color:#142236;color:#ffffff;"
        "border:1px solid #5eead4;}"
        "QLineEdit:disabled{background-color:#182538;color:#91a0b3;"
        "border:1px solid #2c3e55;}"));
}

/** @brief 统一深色管理界面内所有控件的前景色调色板。 */
void applyAdminForegroundPalette(QWidget *root)
{
    if (!root) {
        return;
    }

    QList<QWidget *> widgets = root->findChildren<QWidget *>();
    widgets.prepend(root);
    for (QWidget *widget : widgets) {
        QPalette palette = widget->palette();
        for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive}) {
            palette.setColor(group, QPalette::WindowText, QColor("#e7eef8"));
            palette.setColor(group, QPalette::Text, QColor("#eef5ff"));
            palette.setColor(group, QPalette::ButtonText, QColor("#f4f8ff"));
            palette.setColor(group, QPalette::ToolTipText, QColor("#ffffff"));
        }
        palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#8292a8"));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#8292a8"));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#8292a8"));
        widget->setPalette(palette);
    }
}

/** @brief 统一表格、表头和视口的深色调色板，避免 Weston 使用系统白底。 */
void applyAdminTablePalette(QTableWidget *table)
{
    if (!table) {
        return;
    }

    applyAdminSurface(table, QColor("#111c2c"));
    table->setAutoFillBackground(false);
    if (table->viewport()) {
        applyAdminSurface(table->viewport(), QColor("#111c2c"));
    }
    if (table->horizontalHeader()) {
        applyAdminSurface(table->horizontalHeader(), QColor("#213149"));
        table->horizontalHeader()->setStyleSheet(QStringLiteral(
            "QHeaderView{background:#213149;color:#d8e5f4;border:none;}"
            "QHeaderView::section{background:#213149;color:#d8e5f4;"
            "border:none;border-right:1px solid #33465f;"
            "border-bottom:1px solid #33465f;padding:7px;font-weight:700;}"));
        if (table->horizontalHeader()->viewport()) {
            applyAdminSurface(table->horizontalHeader()->viewport(), QColor("#213149"));
            table->horizontalHeader()->viewport()->setStyleSheet(
                        QStringLiteral("background:#213149;color:#d8e5f4;"));
        }
    }
    if (table->verticalHeader()) {
        applyAdminSurface(table->verticalHeader(), QColor("#213149"));
        if (table->verticalHeader()->viewport()) {
            applyAdminSurface(table->verticalHeader()->viewport(), QColor("#213149"));
        }
    }

    // 某些 RK356x/Weston 组合会让 QPA 原生主题覆盖父窗口的 QSS。
    // 把关键样式直接挂到表格及其实际滚动条，确保视口、空白区和表头都生效。
    table->setStyleSheet(QStringLiteral(
        "QTableWidget{background:#111c2c;alternate-background-color:#162338;"
        "color:#eaf2fd;border:1px solid #30445f;border-radius:12px;"
        "gridline-color:#26374d;outline:0;}"
        "QTableWidget::item{background:#111c2c;color:#eaf2fd;padding:7px;border:none;}"
        "QTableWidget::item:alternate{background:#162338;}"
        "QTableWidget::item:selected{background:#2b7fff;color:#ffffff;}"
        "QHeaderView{background:#213149;color:#d8e5f4;}"
        "QHeaderView::section{background:#213149;color:#d8e5f4;border:none;"
        "border-right:1px solid #33465f;border-bottom:1px solid #33465f;"
        "padding:7px;font-weight:700;}"
        "QTableCornerButton::section{background:#213149;border:none;}"
        "QAbstractScrollArea::corner{background:#162236;border:none;}"));

    if (table->viewport()) {
        table->viewport()->setStyleSheet(QStringLiteral(
            "QWidget{background-color:#111c2c;color:#eaf2fd;border:none;}"));
    }

    applyAdminScrollBars(table);
}

/** @brief 直接设置筛选下拉框及弹出列表的深色样式。 */
void applyAdminComboStyle(QComboBox *combo)
{
    if (!combo || !combo->view()) {
        return;
    }

    combo->setStyleSheet(QStringLiteral(
        "QComboBox{background:#111c2c;color:#eef5ff;border:1px solid #394b63;"
        "border-radius:10px;padding:6px 34px 6px 9px;min-height:30px;}"
        "QComboBox:focus{border:1px solid #5eead4;background:#142236;}"
        "QComboBox::drop-down{width:30px;border:none;background:#213149;"
        "border-top-right-radius:10px;border-bottom-right-radius:10px;}"
        "QComboBox QAbstractItemView{background:#111c2c;color:#eef5ff;"
        "selection-background-color:#2b7fff;selection-color:#ffffff;"
        "border:1px solid #46617f;outline:0;padding:3px;}"
        "QComboBox QAbstractItemView::item{background:#111c2c;color:#eef5ff;"
        "min-height:34px;padding:4px 8px;}"
        "QComboBox QAbstractItemView::item:selected{background:#2b7fff;color:#ffffff;}"));

    applyAdminSurface(combo->view(), QColor("#111c2c"));
    combo->view()->setStyleSheet(QStringLiteral(
        "QAbstractItemView{background:#111c2c;color:#eef5ff;"
        "alternate-background-color:#162338;border:1px solid #46617f;"
        "selection-background-color:#2b7fff;selection-color:#ffffff;outline:0;}"
        "QAbstractItemView::item{background:#111c2c;color:#eef5ff;"
        "min-height:34px;padding:4px 8px;}"
        "QAbstractItemView::item:selected{background:#2b7fff;color:#ffffff;}"));
    if (combo->view()->viewport()) {
        applyAdminSurface(combo->view()->viewport(), QColor("#111c2c"));
        combo->view()->viewport()->setStyleSheet(QStringLiteral(
            "background:#111c2c;color:#eef5ff;border:none;"));
    }
    QWidget *popupWindow = combo->view()->window();
    if (popupWindow) {
        applyAdminSurface(popupWindow, QColor("#111c2c"));
    }
}

/** @brief 将输入弹窗固定为 Wayland 父窗口内的局部坐标子控件。 */
void prepareAdminInputDialog(QInputDialog *dialog, QWidget *parent)
{
    if (!dialog) {
        return;
    }

    dialog->setObjectName(QStringLiteral("adminInputDialog"));
    dialog->setModal(true);
    dialog->setAttribute(Qt::WA_StyledBackground, true);
    if (parent && !parent->styleSheet().isEmpty()) {
        // QInputDialog 在 Wayland 下可能被当作独立窗口处理，显式继承管理页
        // 样式，避免内部标签、输入框和按钮退回系统白色主题。
        dialog->setStyleSheet(parent->styleSheet());
    }

    const QList<QLineEdit *> editors = dialog->findChildren<QLineEdit *>();
    for (QLineEdit *editor : editors) {
        editor->setAttribute(Qt::WA_InputMethodEnabled, true);
        applyAdminEditorStyle(editor);
    }

    if (!ResponsiveUi::isWaylandPlatform() || !parent) {
        return;
    }

    dialog->setWindowFlags(Qt::Widget);
    dialog->ensurePolished();
    const int width = qBound(320, 500, qMax(320, parent->width() - 36));
    const int height = qBound(210, dialog->sizeHint().height() + 30,
                              qMax(210, parent->height() - 36));
    dialog->setFixedSize(width, height);

    // 输入时共享键盘占据底部，弹窗固定在键盘上方区域的中央。
    const int keyboardHeight = ResponsiveUi::sharedKeyboardHeight(parent->size());
    const int availableHeight = qMax(height, parent->height() - keyboardHeight);
    const int x = qMax(0, (parent->width() - width) / 2);
    const int y = qMax(0, (availableHeight - height) / 2);
    dialog->move(x, y);
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

/** @brief 在专用线程内读取系统设备状态并维护 CPU 采样基线。 */
class DeviceStatusCollector : public QObject {
public:
    DeviceStatusSnapshot collect()
    {
        DeviceStatusSnapshot snapshot;
        snapshot.serialNumber = deviceSerialNumber();
        snapshot.ipAddress = firstIpv4Address();
        snapshot.macAddress = firstMacAddress();

        const QStorageInfo storage = storageForApplicationData();
        const qint64 total = storage.bytesTotal();
        const qint64 free = storage.bytesFree();
        const qint64 used = total > 0 ? qBound<qint64>(0, total - free, total) : 0;
        snapshot.diskCapacity = total > 0
            ? QString("%1 / %2").arg(formatBytes(used)).arg(formatBytes(total))
            : QStringLiteral("--");
        snapshot.diskRootPath = storage.rootPath();

        QFile statFile(QStringLiteral("/proc/stat"));
        if (statFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QList<QByteArray> fields = statFile.readLine().simplified().split(' ');
            if (fields.size() >= 8 && fields.first() == "cpu") {
                qint64 values[7] = {0, 0, 0, 0, 0, 0, 0};
                for (int i = 0; i < 7; ++i) {
                    values[i] = fields.value(i + 1).toLongLong();
                }
                const qint64 idle = values[3] + values[4];
                const qint64 cpuTotal = values[0] + values[1] + values[2] + values[3]
                    + values[4] + values[5] + values[6];
                const qint64 totalDelta = cpuTotal - lastCpuTotal_;
                const qint64 idleDelta = idle - lastCpuIdle_;
                if (lastCpuTotal_ > 0 && totalDelta > 0) {
                    const double usage =
                        (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta)) * 100.0;
                    snapshot.cpuUsage = QString("%1%").arg(qBound(0.0, usage, 100.0), 0, 'f', 1);
                }
                lastCpuTotal_ = cpuTotal;
                lastCpuIdle_ = idle;
            }
        }
        if (snapshot.cpuUsage.isEmpty()) {
            snapshot.cpuUsage = QStringLiteral("--");
        }

        QFile meminfoFile(QStringLiteral("/proc/meminfo"));
        QString meminfoText;
        if (meminfoFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            meminfoText = QString::fromLocal8Bit(meminfoFile.readAll());
        } else {
            qWarning().noquote() << "读取 /proc/meminfo 失败：" << meminfoFile.errorString();
        }
        if (meminfoText.trimmed().isEmpty()) {
            QProcess cat;
            cat.start(QStringLiteral("/bin/cat"), QStringList() << QStringLiteral("/proc/meminfo"));
            if (cat.waitForFinished(800)) {
                meminfoText = QString::fromLocal8Bit(cat.readAllStandardOutput());
                qWarning().noquote() << "QFile 读取 meminfo 为空，已尝试 /bin/cat 兜底，输出长度："
                                     << meminfoText.size();
            } else {
                qWarning().noquote() << "/bin/cat /proc/meminfo 超时或失败：" << cat.errorString();
            }
        }

        const qint64 totalKb = meminfoValueKb(meminfoText, QStringLiteral("MemTotal"));
        qint64 availableKb = meminfoValueKb(meminfoText, QStringLiteral("MemAvailable"));
        const qint64 freeKb = meminfoValueKb(meminfoText, QStringLiteral("MemFree"));
        const qint64 buffersKb = meminfoValueKb(meminfoText, QStringLiteral("Buffers"));
        const qint64 cachedKb = meminfoValueKb(meminfoText, QStringLiteral("Cached"));
        if (availableKb <= 0) {
            availableKb = freeKb + buffersKb + cachedKb;
        }
        if (totalKb > 0) {
            const qint64 usedKb = qBound<qint64>(0, totalKb - availableKb, totalKb);
            const double usedPercent = static_cast<double>(usedKb) * 100.0 / static_cast<double>(totalKb);
            snapshot.memoryUsage = QString("%1% (%2/%3)")
                .arg(usedPercent, 0, 'f', 1)
                .arg(formatBytes(usedKb * 1024))
                .arg(formatBytes(totalKb * 1024));
        } else {
            snapshot.memoryUsage = QStringLiteral("--");
        }

        snapshot.temperature = currentTemperature();
        snapshot.npuUsage = npuUsage();
        return snapshot;
    }

private:
    qint64 lastCpuTotal_ = 0;
    qint64 lastCpuIdle_ = 0;
};

const int kAdminSidebarWidth = 198;
const int kAdminOuterMargin = 16;
const int kAdminTabMargin = 12;
const int kAdminRowSpacing = 6;
const int kDeviceInfoPageIndex = 0;
const int kPersonManagementPageIndex = 1;
const int kFaceRecordPageIndex = 2;
const int kMaintenancePageIndex = 3;
const int kNewPersonTabIndex = 0;
const int kViewPeopleTabIndex = 1;
const int kNetworkPeopleTabIndex = 2;
}

/** @brief 按当前配置创建导航和内容页面。 */
AdminPanel::AdminPanel(const AppConfig &config, QWidget *parent)
    : QDialog(parent), config_(config)
{
    setObjectName(QStringLiteral("AdminPanel"));
    // Wayland 下在创建子控件前确定窗口类型，管理页始终复用人脸根窗口
    // 的 surface；虚拟键盘由 FaceGateMainWindow 的唯一共享实例提供。
    if (ResponsiveUi::isWaylandPlatform() && parentWidget()) {
        setWindowFlags(Qt::Widget);
        setAttribute(Qt::WA_StyledBackground, true);
    }
    // 管理面板作为人脸主界面上的全屏子控件时必须自行绘制完整不透明背景。
    // 不能依赖底层页面清屏，否则 Wayland 复合时未绘制区域会露出摄像头主页。
    applyAdminSurface(this, QColor("#142033"));

    setWindowTitle("人脸闸机管理");
    setModal(false);
    setMinimumSize(ResponsiveUi::kMinimumScreenExtent,
                   ResponsiveUi::kMinimumScreenExtent);
    resize(ResponsiveUi::hostRect(parent).size());

    // 左侧为导航栏，右侧为当前管理页面。
    rootLayout_ = new QHBoxLayout(this);
    rootLayout_->setContentsMargins(0, 0, 0, 0);
    rootLayout_->setSpacing(0);
    rootLayout_->setSizeConstraint(QLayout::SetNoConstraint);

    sidebar_ = new QFrame(this);
    sidebar_->setObjectName("sidebar");
    sideLayout_ = new QGridLayout(sidebar_);
    sideLayout_->setContentsMargins(14, 18, 14, 18);
    sideLayout_->setSpacing(10);

    adminTitle_ = new QLabel("管理", sidebar_);
    adminTitle_->setObjectName("adminTitle");
    adminSubtitle_ = new QLabel("闸机控制台", sidebar_);
    adminSubtitle_->setObjectName("adminSubtitle");

    // 管理页面导航顺序必须与 stack_ 页面添加顺序一致。
    navList_ = new QListWidget(sidebar_);
    navList_->setObjectName("navList");
    navList_->setIconSize(QSize(22, 22));
    navList_->setMinimumHeight(0);
    navList_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    navList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navList_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    navList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    navList_->addItem(new QListWidgetItem(QIcon(QStringLiteral(":/icons/nav/device.svg")), QStringLiteral("本机信息")));
    navList_->addItem(new QListWidgetItem(QIcon(QStringLiteral(":/icons/nav/users.svg")), QStringLiteral("人员管理")));
    navList_->addItem(new QListWidgetItem(QIcon(QStringLiteral(":/icons/nav/logs.svg")), QStringLiteral("识别记录")));
    navList_->addItem(new QListWidgetItem(QIcon(QStringLiteral(":/icons/nav/tools.svg")), QStringLiteral("运维维护")));
    navList_->addItem(new QListWidgetItem(QIcon(QStringLiteral(":/icons/nav/settings.svg")), QStringLiteral("设置")));
    navList_->setCurrentRow(kDeviceInfoPageIndex);

    livenessCheck_ = new AdminToggleSwitch(this);
    livenessCheck_->setObjectName("livenessSwitch");
    livenessCheck_->setChecked(true);

    backButton_ = new QPushButton(sidebar_);
    backButton_->setObjectName("backButton");
    backButton_->setToolTip(QStringLiteral("返回"));
    backButton_->setAccessibleName(QStringLiteral("返回"));
    setButtonIcon(backButton_, QStringLiteral(":/icons/action/x.svg"));
    backButton_->setFixedSize(44, 40);

    sideLayout_->addWidget(adminTitle_, 0, 0);
    sideLayout_->addWidget(adminSubtitle_, 1, 0);
    sideLayout_->addWidget(navList_, 3, 0);
    sideLayout_->addWidget(backButton_, 5, 0);
    sideLayout_->setRowStretch(3, 1);

    stack_ = new QStackedWidget(this);
    stack_->setObjectName("contentStack");
    applyAdminSurface(stack_);
    // 右侧页面不能把管理窗口的最小宽度撑大；宽表格和工具栏在页面内部滚动或换行。
    stack_->setMinimumWidth(0);
    stack_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

    enrollWidget_ = new FaceEnrollWidget(this);
    enrollWidget_->setAutoEnrollThresholds(config_.faceQualityThreshold, config_.faceDetectThreshold, config_.livenessThreshold);

    stack_->addWidget(createDeviceInfoPage());
    stack_->addWidget(createPersonManagementPage());
    stack_->addWidget(createLogPage());
    stack_->addWidget(createMaintenancePage());
    stack_->addWidget(createSettingsPage());

    auto *contentShell = new QWidget(this);
    contentShell_ = contentShell;
    contentShell_->setObjectName("contentShell");
    contentShell_->setMinimumWidth(0);
    contentShell_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    contentShell_->setAttribute(Qt::WA_StyledBackground, true);

    auto *contentLayout = new QVBoxLayout(contentShell_);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    contentLayout->setSizeConstraint(QLayout::SetNoConstraint);
    contentLayout->addWidget(stack_, 1);

    // 导航外观和动画由共用 Quick 组件承担；原 QListWidget 继续保存既有
    // 页面索引和业务触发逻辑，但不再参与绘制。
    quickNavigation_ = new QuickNavigationWidget(this);
    quickNavigation_->setFaceTheme(true);
    quickNavigation_->setItems({QStringLiteral("本机信息"),
                                QStringLiteral("人员管理"),
                                QStringLiteral("识别记录"),
                                QStringLiteral("运维维护"),
                                QStringLiteral("设置")});
    quickNavigation_->setHeader(QStringLiteral("管理"), QString());
    quickNavigation_->setBackVisible(true);
    quickNavigation_->setCurrentIndex(kDeviceInfoPageIndex);
    sidebar_->hide();

    rootLayout_->addWidget(quickNavigation_);
    // QML 加载失败时保留原导航作为运行时兜底；正常情况下该控件始终隐藏。
    rootLayout_->addWidget(sidebar_);
    rootLayout_->addWidget(contentShell_, 1);

    actionToast_ = new QLabel(this);
    actionToast_->setAlignment(Qt::AlignCenter);
    actionToast_->setWordWrap(true);
    actionToast_->setAttribute(Qt::WA_TransparentForMouseEvents);
    actionToast_->hide();
    actionToastOpacity_ = new QGraphicsOpacityEffect(actionToast_);
    actionToastOpacity_->setOpacity(0.0);
    actionToast_->setGraphicsEffect(actionToastOpacity_);
    actionToastAnimation_ = new QPropertyAnimation(actionToastOpacity_, "opacity", this);
    connect(actionToastAnimation_, &QPropertyAnimation::finished, this, [this]() {
        if (actionToast_ && actionToastAnimation_->endValue().toReal() < 0.01) {
            actionToast_->hide();
        }
    });
    actionToastTimer_ = new QTimer(this);
    actionToastTimer_->setSingleShot(true);
    actionToastTimer_->setInterval(1700);
    connect(actionToastTimer_, &QTimer::timeout, this, [this]() {
        if (!actionToast_ || !actionToastOpacity_) {
            return;
        }
        actionToastAnimation_->stop();
        actionToastAnimation_->setDuration(220);
        actionToastAnimation_->setStartValue(actionToastOpacity_->opacity());
        actionToastAnimation_->setEndValue(0.0);
        actionToastAnimation_->setEasingCurve(QEasingCurve::InCubic);
        actionToastAnimation_->start();
    });

    connect(enrollWidget_, &FaceEnrollWidget::enrollRequested, this, &AdminPanel::enrollRequested);
    connect(enrollWidget_, &FaceEnrollWidget::cameraRequirementChanged, this, &AdminPanel::updateCameraRequirement);
    connect(livenessCheck_, &QCheckBox::toggled, this, &AdminPanel::livenessEnabledChanged);
    connect(backButton_, &QPushButton::clicked, this, &AdminPanel::close);
    connect(quickNavigation_, &QuickNavigationWidget::backRequested,
            backButton_, &QPushButton::click);
    connect(quickNavigation_, &QuickNavigationWidget::itemActivated,
            this, [this](int row) {
        if (navList_ && row >= 0 && row < navList_->count()) {
            navList_->setCurrentRow(row);
        }
    });
    connect(navList_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (quickNavigation_) {
            quickNavigation_->setCurrentIndex(row);
        }
        stack_->setCurrentIndex(row);
        updateDeviceStatusRefreshState();
        if (row == kDeviceInfoPageIndex) {
            updateDeviceInfo();
            emit deviceInfoRefreshRequested();
        } else if (row == kPersonManagementPageIndex) {
            emit peopleRefreshRequested();
        } else if (row == kFaceRecordPageIndex && !passedLogsLoaded_) {
            QTimer::singleShot(0, this, [this]() { requestQueryVerifyLogs(); });
        } else if (row == kMaintenancePageIndex) {
            emit deviceInfoRefreshRequested();
        }
        updateCameraRequirement();
    });

    deviceStatusTimer_ = new QTimer(this);
    deviceStatusTimer_->setInterval(1000);
    connect(deviceStatusTimer_, &QTimer::timeout, this, &AdminPanel::updateDeviceInfo);

    auto *deviceStatusCollector = new DeviceStatusCollector;
    deviceStatusCollector_ = deviceStatusCollector;
    deviceStatusCollector_->moveToThread(&deviceStatusThread_);
    deviceStatusThread_.setObjectName(QStringLiteral("DeviceStatusThread"));
    connect(&deviceStatusThread_, &QThread::finished,
            deviceStatusCollector_, &QObject::deleteLater);
    deviceStatusThread_.start();

    QFile adminStyleFile(QStringLiteral(":/qss/admin_panel.qss"));
    if (adminStyleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setStyleSheet(QString::fromUtf8(adminStyleFile.readAll()));
    }

    // 父级 QSS 完成 polish 后再次设置控件级样式，避免 Weston 原生样式
    // 在最后一次 polish 时恢复表格视口、表头或滚动条的白色背景。
    applyAdminForegroundPalette(this);
    const QList<QLineEdit *> adminEditors = findChildren<QLineEdit *>();
    for (QLineEdit *editor : adminEditors) {
        applyAdminEditorStyle(editor);
    }
    QTableWidget *adminTables[] = {
        peopleTable_, networkPeopleTable_, passedLogTable_,
        systemEventTable_
    };
    for (QTableWidget *table : adminTables) {
        applyAdminTablePalette(table);
    }
    applyAdminComboStyle(verifyResultCombo_);
    applyResponsiveLayout();
}

/** @brief 停止设备状态采集线程后销毁面板。 */
AdminPanel::~AdminPanel()
{
    if (deviceStatusTimer_) {
        deviceStatusTimer_->stop();
    }
    deviceStatusThread_.quit();
    deviceStatusThread_.wait();
    deviceStatusCollector_ = nullptr;
}

/** @return 人员录入和管理组合页面。 */
QWidget *AdminPanel::createPersonManagementPage()
{
    auto *page = new QWidget(this);
    applyAdminSurface(page);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(kAdminOuterMargin, 18, kAdminOuterMargin, 18);
    layout->setSpacing(10);

    auto *title = new QLabel("人员管理", page);
    title->setObjectName("pageTitle");

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
    layout->addWidget(personTabs_, 1);
    return page;
}

/** @return 人员列表页面。 */
QWidget *AdminPanel::createPeoplePage()
{
    auto *page = new QWidget(this);
    applyAdminSurface(page, QColor("#1b2a40"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(kAdminOuterMargin, 18, kAdminOuterMargin, 18);
    layout->setSpacing(10);

    // 原来所有筛选框和操作按钮都放在同一条横向布局里，
    // 在 1024x768 屏幕上会把右侧页面最小宽度撑到超过可视区域，
    // 进而导致右侧内容和内嵌键盘一起被裁剪。这里改为两行紧凑布局。
    auto *filterGrid = new QGridLayout();
    filterGrid->setHorizontalSpacing(kAdminRowSpacing);
    filterGrid->setVerticalSpacing(kAdminRowSpacing);
    filterGrid->setContentsMargins(0, 0, 0, 0);
    verifyResultCombo_ = new QComboBox(page);
    verifyResultCombo_->addItem("全部", "all");
    verifyResultCombo_->addItem("通过", "passed");
    verifyResultCombo_->addItem("失败", "failed");
    verifyResultCombo_->addItem("活体失败", "liveness_failed");
    verifyResultCombo_->addItem("陌生人", "stranger");
    verifyResultCombo_->addItem("门禁拒绝", "access_denied");
    verifyResultCombo_->addItem("出门按钮", "exit_button");
    verifyResultCombo_->addItem("报警", "alarm");
    verifyResultCombo_->setMinimumWidth(80);
    applyAdminComboStyle(verifyResultCombo_);
    verifyPersonNoEdit_ = new QLineEdit(page);
    verifyPersonNoEdit_->setPlaceholderText("人员编号");
    verifyPersonNoEdit_->setMinimumWidth(84);
    verifyNameEdit_ = new QLineEdit(page);
    verifyNameEdit_->setPlaceholderText("姓名");
    verifyNameEdit_->setMinimumWidth(76);
    verifyStartEdit_ = new QDateTimeEdit(QDateTime::currentDateTime().addDays(-7), page);
    verifyStartEdit_->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
    verifyStartEdit_->setMinimumWidth(132);
    verifyEndEdit_ = new QDateTimeEdit(QDateTime::currentDateTime(), page);
    verifyEndEdit_->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
    verifyEndEdit_->setMinimumWidth(132);
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
    auto *showDeletedShell = new QWidget(page);
    auto *showDeletedLayout = new QHBoxLayout(showDeletedShell);
    showDeletedLayout->setContentsMargins(4, 0, 4, 0);
    showDeletedLayout->setSpacing(10);
    auto *showDeletedLabel = new QLabel(QStringLiteral("显示已删除"), showDeletedShell);
    showDeletedLabel->setObjectName(QStringLiteral("optionLabel"));
    showDeletedPeopleCheck_ = new AdminToggleSwitch(showDeletedShell);
    showDeletedLayout->addWidget(showDeletedLabel);
    showDeletedLayout->addWidget(showDeletedPeopleCheck_);
    showDeletedLayout->addStretch();

    filterGrid->addWidget(verifyResultCombo_, 0, 0);
    filterGrid->addWidget(verifyPersonNoEdit_, 0, 1);
    filterGrid->addWidget(verifyNameEdit_, 0, 2);
    filterGrid->addWidget(refreshButton, 0, 3);
    filterGrid->addWidget(verifyStartEdit_, 1, 0, 1, 2);
    filterGrid->addWidget(verifyEndEdit_, 1, 2, 1, 2);
    filterGrid->setColumnStretch(0, 1);
    filterGrid->setColumnStretch(1, 1);
    filterGrid->setColumnStretch(2, 1);
    filterGrid->setColumnStretch(3, 1);

    buttonGrid->addWidget(modifyButton, 0, 0);
    buttonGrid->addWidget(permissionButton, 0, 1);
    buttonGrid->addWidget(reEnrollButton, 0, 2);
    buttonGrid->addWidget(importPeopleButton_, 0, 3);
    buttonGrid->addWidget(deleteButton, 1, 0);
    buttonGrid->addWidget(exportPeopleButton_, 1, 1);
    buttonGrid->addWidget(showDeletedShell, 1, 2, 1, 2);
    buttonGrid->setColumnStretch(3, 1);

    peopleTable_ = new QTableWidget(page);
    applyAdminTablePalette(peopleTable_);
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

    layout->addLayout(filterGrid);
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
    applyAdminSurface(page, QColor("#1b2a40"));
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
    applyAdminTablePalette(networkPeopleTable_);
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
    applyAdminSurface(page);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(kAdminOuterMargin, 18, kAdminOuterMargin, 18);
    layout->setSpacing(10);

    auto *title = new QLabel("识别记录", page);
    title->setObjectName("pageTitle");

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
    applyAdminTablePalette(passedLogTable_);
    passedLogTable_->setMinimumWidth(0);
    passedLogTable_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    passedLogTable_->setColumnCount(8);
    passedLogTable_->setHorizontalHeaderLabels(QStringList()
        << "日志ID" << "结果" << "人员ID" << "姓名" << "失败原因"
        << "事件类型" << "方向" << "时间");
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
    logHeader->resizeSection(4, 120);
    logHeader->resizeSection(5, 110);
    logHeader->resizeSection(6, 70);
    logHeader->resizeSection(7, 178);

    layout->addWidget(title);
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

/** @return 备份和清理维护页面。 */
QWidget *AdminPanel::createMaintenancePage()
{
    auto *page = new QWidget(this);
    applyAdminSurface(page);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(kAdminOuterMargin, 18, kAdminOuterMargin, 18);
    layout->setSpacing(10);

    auto *title = new QLabel("运维维护", page);
    title->setObjectName("pageTitle");

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
    applyAdminTablePalette(systemEventTable_);
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
    layout->addLayout(buttonGrid);
    layout->addWidget(systemEventTable_, 1);

    connect(backupButton, &QPushButton::clicked, this, &AdminPanel::maintenanceBackupRequested);
    connect(cleanup30Button, &QPushButton::clicked, this, [this]() { emit maintenanceCleanupRequested(30); });
    connect(cleanup90Button, &QPushButton::clicked, this, [this]() { emit maintenanceCleanupRequested(90); });
    connect(cleanupCustomButton, &QPushButton::clicked, this, [this]() {
        QInputDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("自定义清理天数"));
        dialog.setLabelText(QStringLiteral("清理多少天前的数据："));
        dialog.setInputMode(QInputDialog::IntInput);
        dialog.setIntRange(1, 3650);
        dialog.setIntStep(1);
        dialog.setIntValue(30);
        dialog.setOkButtonText(QStringLiteral("确定"));
        dialog.setCancelButtonText(QStringLiteral("取消"));
        prepareAdminInputDialog(&dialog, this);
        if (dialog.exec() == QDialog::Accepted) {
            emit maintenanceCleanupRequested(dialog.intValue());
        }
    });
    connect(refreshErrorsButton, &QPushButton::clicked, this, &AdminPanel::deviceInfoRefreshRequested);
    return page;
}

/** @return 设备资源与运行状态页面。 */
QWidget *AdminPanel::createDeviceInfoPage()
{
    auto *page = new QWidget(this);
    applyAdminSurface(page);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(kAdminOuterMargin, 18, kAdminOuterMargin, 18);
    layout->setSpacing(10);

    auto *title = new QLabel("本机信息", page);
    title->setObjectName("pageTitle");

    auto *scrollArea = new QScrollArea(page);
    scrollArea->setObjectName("deviceInfoScroll");
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    applyAdminScrollBars(scrollArea);
    applyAdminSurface(scrollArea);
    if (scrollArea->viewport()) {
        applyAdminSurface(scrollArea->viewport());
    }

    auto *scrollContent = new QWidget(scrollArea);
    scrollContent->setObjectName("deviceInfoScrollContent");
    applyAdminSurface(scrollContent);
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
    layout->addWidget(scrollArea, 1);
    updateDeviceSummaryLabels();
    return page;
}

/** @return 阈值、网络、音频、显示和密码设置页面。 */
QWidget *AdminPanel::createSettingsPage()
{
    auto *page = new QWidget(this);
    applyAdminSurface(page);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(kAdminOuterMargin, 18, kAdminOuterMargin, 18);
    layout->setSpacing(10);

    auto *title = new QLabel("设置", page);
    title->setObjectName("pageTitle");

    auto *tabs = new QTabWidget(page);
    tabs->setObjectName(QStringLiteral("settingsTabs"));

    auto *networkPage = new QWidget(tabs);
    applyAdminSurface(networkPage, QColor("#1b2a40"));
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
    auto *networkSaveButton = new QPushButton("保存", networkPage);
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
    applyAdminSurface(thresholdPage, QColor("#1b2a40"));
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

    auto *livenessGroup = new QGroupBox(QStringLiteral("活体检测"), thresholdPage);
    auto *livenessLayout = new QHBoxLayout(livenessGroup);
    livenessLayout->setContentsMargins(14, 18, 14, 14);
    livenessLayout->setSpacing(14);
    auto *livenessTextBox = new QVBoxLayout();
    livenessTextBox->setSpacing(3);
    auto *livenessTitle = new QLabel(QStringLiteral("启用活体检测"), livenessGroup);
    livenessTitle->setObjectName(QStringLiteral("switchTitle"));
    auto *livenessHint = new QLabel(
                QStringLiteral("关闭后仅执行人脸比对，不再校验活体通过状态"),
                livenessGroup);
    livenessHint->setObjectName(QStringLiteral("switchHint"));
    livenessHint->setWordWrap(true);
    livenessTextBox->addWidget(livenessTitle);
    livenessTextBox->addWidget(livenessHint);
    livenessCheck_->setParent(livenessGroup);
    livenessLayout->addLayout(livenessTextBox, 1);
    livenessLayout->addWidget(livenessCheck_, 0, Qt::AlignVCenter);
    thresholdLayout->addWidget(livenessGroup);

    auto *thresholdSaveButton = new QPushButton("保存", thresholdPage);
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
    applyAdminScrollBars(audioScrollPage);
    audioScrollPage->viewport()->setAutoFillBackground(false);
    applyAdminSurface(audioScrollPage, QColor("#1b2a40"));
    if (audioScrollPage->viewport()) {
        applyAdminSurface(audioScrollPage->viewport(), QColor("#1b2a40"));
    }

    auto *audioPage = new QWidget(audioScrollPage);
    audioPage->setObjectName(QStringLiteral("audioSettingsScrollContent"));
    applyAdminSurface(audioPage, QColor("#1b2a40"));
    auto *audioLayout = new QVBoxLayout(audioPage);
    audioLayout->setContentsMargins(kAdminTabMargin, kAdminTabMargin, kAdminTabMargin, kAdminTabMargin + 8);
    audioLayout->setSpacing(16);

    auto *audioBasicGroup = new QGroupBox(QStringLiteral("音频播报"), audioPage);
    auto *audioBasicLayout = new QGridLayout(audioBasicGroup);
    audioBasicLayout->setContentsMargins(14, 22, 14, 16);
    audioBasicLayout->setHorizontalSpacing(14);
    audioBasicLayout->setVerticalSpacing(14);

    auto *audioEnabledLabel = new QLabel(QStringLiteral("启用本地 WAV 音频播报"), audioBasicGroup);
    audioEnabledLabel->setObjectName(QStringLiteral("optionLabel"));
    audioEnabledCheck_ = new AdminToggleSwitch(audioBasicGroup);
    audioEnabledCheck_->setChecked(config_.audioEnabled);

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

    auto *audioVolumeNameLabel = new QLabel(QStringLiteral("语音播报音量"), audioBasicGroup);
    audioVolumeNameLabel->setObjectName("metricName");
    audioVolumeNameLabel->setMinimumHeight(34);

    audioBasicLayout->addWidget(audioEnabledLabel, 0, 0, 1, 2);
    audioBasicLayout->addWidget(audioEnabledCheck_, 0, 2, Qt::AlignRight | Qt::AlignVCenter);
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

        auto *enabledCheck = new AdminToggleSwitch(promptGroup);
        enabledCheck->setChecked(enabled);

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

    auto *audioSaveButton = new QPushButton(QStringLiteral("保存"), audioPage);
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
    applyAdminSurface(basicPage, QColor("#1b2a40"));
    auto *basicLayout = new QVBoxLayout(basicPage);
    basicLayout->setContentsMargins(kAdminTabMargin, kAdminTabMargin, kAdminTabMargin, kAdminTabMargin);
    basicLayout->setSpacing(12);
    auto *basicText = new QLabel(QString("采集帧数：%1\n活体通过帧数：%2\n识别窗口：%3 ms\n结果保持：%4 ms\n同人抑制：%5 ms\n权限校验超时：%6 ms")
        .arg(config_.collectFrameCount)
        .arg(config_.livenessPassRequired)
        .arg(config_.recognitionWindowMs)
        .arg(config_.resultHoldMs)
        .arg(config_.sameFaceSuppressMs)
        .arg(config_.accessDecisionTimeoutMs), basicPage);
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
    auto *displayRotationPage = new DisplayRotationPage(tabs);
    applyAdminSurface(displayRotationPage, QColor("#1b2a40"));
    auto *accessPhotoGroup = new QGroupBox(
                QStringLiteral("通行照片显示"), displayRotationPage);
    auto *accessPhotoLayout = new QHBoxLayout(accessPhotoGroup);
    accessPhotoLayout->setContentsMargins(16, 22, 16, 16);
    accessPhotoLayout->setSpacing(12);
    auto *accessPhotoTitle = new QLabel(
                QStringLiteral("显示通行照片和姓名"), accessPhotoGroup);
    accessPhotoTitle->setObjectName(QStringLiteral("metricName"));
    auto *accessPhotoSwitch = new AdminToggleSwitch(accessPhotoGroup);
    accessPhotoSwitch->setObjectName(QStringLiteral("accessPhotoDisplaySwitch"));
    accessPhotoSwitch->setChecked(config_.showAccessPhoto);
    accessPhotoLayout->addWidget(accessPhotoTitle, 1);
    accessPhotoLayout->addWidget(accessPhotoSwitch, 0, Qt::AlignVCenter);
    if (auto *displayLayout = qobject_cast<QVBoxLayout *>(displayRotationPage->layout())) {
        displayLayout->insertWidget(1, accessPhotoGroup);
    }
    connect(accessPhotoSwitch, &QCheckBox::toggled,
            this, &AdminPanel::accessPhotoDisplayChanged);
    // 显示方向页面由多媒体和人脸后台共用。人脸后台必须把样式直接
    // 应用到下拉框及其独立的 Wayland 弹出窗口，避免出现白底白字。
    const QList<QComboBox *> displayRotationCombos =
            displayRotationPage->findChildren<QComboBox *>();
    for (QComboBox *combo : displayRotationCombos) {
        applyAdminComboStyle(combo);
    }
    tabs->addTab(displayRotationPage, QStringLiteral("显示设置"));

    layout->addWidget(title);
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

/** @brief 清除录入画面并在预览区域显示摄像头异常提示。 */
void AdminPanel::setCameraUnavailableMessage(const QString &message)
{
    if (enrollWidget_) {
        enrollWidget_->setCameraUnavailableMessage(message);
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

/** @brief 按正常/已删除视图过滤缓存记录，并重建网络人员表格。 */
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

/** @brief 切换网络人员删除状态筛选，并同步页签样式。 */
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

/** @brief 在模态网格中展示指定网络人员的全部已同步人脸原图。 */
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
    const bool embeddedForWayland = ResponsiveUi::isWaylandPlatform();
    dialog.setModal(true);
    dialog.setObjectName(QStringLiteral("networkFacesDialog"));
    if (embeddedForWayland) {
        dialog.setWindowFlags(Qt::Widget);
        dialog.setGeometry(rect());
        dialog.setMinimumSize(0, 0);
        dialog.setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    } else {
        dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        dialog.setFixedSize(dialogWidth, dialogHeight);
    }
    dialog.setAttribute(Qt::WA_TranslucentBackground);
    dialog.setAttribute(Qt::WA_StyledBackground, true);

    auto *outerLayout = new QVBoxLayout(&dialog);
    outerLayout->setContentsMargins(8, 8, 8, 8);

    auto *shell = new QFrame(&dialog);
    shell->setObjectName(QStringLiteral("networkFacesShell"));
    shell->setFixedSize(dialogWidth - 16, dialogHeight - 16);
    shell->setStyleSheet(QStringLiteral(
        "QFrame#networkFacesShell {"
        "background:#162236;"
        "border:none;"
        "border-radius:14px;"
        "}"
        "QLabel { color:#edf5ff; border:none; background:transparent; }"));
    outerLayout->addWidget(shell, 0, Qt::AlignCenter);

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

    if (!embeddedForWayland) {
        const QPoint center = mapToGlobal(rect().center());
        dialog.move(center.x() - dialog.width() / 2,
                    center.y() - dialog.height() / 2);
    } else {
        dialog.raise();
    }
    dialog.exec();
}

/** @brief 显示网络同步结果提示，并启动自动隐藏计时器。 */
void AdminPanel::showNetworkSyncStatus(bool ok, const QString &text)
{
    if (text.trimmed().isEmpty()) {
        return;
    }

    if (QuickUiRuntime::instance()->showToast(text.trimmed(), 3000, ok, true)) {
        return;
    }
    if (!networkSyncToast_) return;

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

/** @brief 根据文本宽度将网络同步提示条定位到页面底部中央。 */
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

/** @brief 填充验证通过记录表，并在批量更新期间暂停重绘。 */
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
        const QString personId = record.personId > 0
            ? QString::number(record.personId)
            : record.personNo.trimmed();
        passedLogTable_->setItem(row, 0, tableItem(QString::number(record.logId)));
        passedLogTable_->setItem(row, 1, tableItem(record.result));
        passedLogTable_->setItem(row, 2, tableItem(personId.isEmpty() ? QStringLiteral("--") : personId));
        passedLogTable_->setItem(row, 3, tableItem(record.name));
        passedLogTable_->setItem(row, 4, tableItem(record.failReason));
        passedLogTable_->setItem(row, 5, tableItem(record.eventType));
        passedLogTable_->setItem(row, 6, tableItem(record.direction));
        passedLogTable_->setItem(row, 7, tableItem(record.createdAt));
    }
    passedLogTable_->horizontalHeader()->updateGeometry();
    passedLogTable_->setUpdatesEnabled(true);
    passedLogTable_->viewport()->update();
}

/** @brief 更新人员、特征、日志和磁盘容量摘要。 */
void AdminPanel::setStorageStats(const StorageStats &stats)
{
    storageStats_ = stats;
    updateDeviceSummaryLabels();
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

/** @brief 在管理界面中央显示安卓风格的轻量操作结果提示。 */
void AdminPanel::showActionToast(const QString &text, bool success)
{
    if (text.trimmed().isEmpty()) {
        return;
    }

    if (QuickUiRuntime::instance()->showToast(text.trimmed(), 1700, success, true)) {
        return;
    }
    if (!actionToast_) return;

    actionToast_->setText(text.trimmed());
    actionToast_->setStyleSheet(QStringLiteral(
        "QLabel{background:%1;color:#ffffff;border:1px solid %2;"
        "border-radius:13px;padding:10px 18px;font-size:16px;font-weight:700;}"
    ).arg(success ? QStringLiteral("rgba(26,42,58,238)")
                  : QStringLiteral("rgba(70,31,39,242)"),
          success ? QStringLiteral("#4b647e") : QStringLiteral("#e56b7d")));

    const int maxWidth = qMax(180, qMin(420, width() - 48));
    const QFontMetrics metrics(actionToast_->font());
    const QRect textRect = metrics.boundingRect(
                QRect(0, 0, maxWidth - 36, 140),
                Qt::AlignCenter | Qt::TextWordWrap,
                actionToast_->text());
    actionToast_->setFixedSize(qMin(maxWidth, qMax(180, textRect.width() + 42)),
                               qMax(46, textRect.height() + 24));
    positionActionToast();
    actionToastOpacity_->setOpacity(0.0);
    actionToast_->show();
    actionToast_->raise();

    actionToastAnimation_->stop();
    actionToastAnimation_->setDuration(150);
    actionToastAnimation_->setStartValue(0.0);
    actionToastAnimation_->setEndValue(1.0);
    actionToastAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    actionToastAnimation_->start();
    actionToastTimer_->start();
}

/** @brief 将轻量提示保持在当前管理窗口正中央。 */
void AdminPanel::positionActionToast()
{
    if (!actionToast_) {
        return;
    }
    actionToast_->move(qMax(0, (width() - actionToast_->width()) / 2),
                       qMax(0, (height() - actionToast_->height()) / 2));
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

    QInputDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("修改人员编号"));
    dialog.setLabelText(QStringLiteral("新的人员编号："));
    dialog.setInputMode(QInputDialog::TextInput);
    dialog.setTextEchoMode(QLineEdit::Normal);
    dialog.setTextValue(selectedPersonNo());
    dialog.setOkButtonText(QStringLiteral("确定"));
    dialog.setCancelButtonText(QStringLiteral("取消"));
    prepareAdminInputDialog(&dialog, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString newNo = dialog.textValue().trimmed();
    if (newNo.isEmpty()) {
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
        QString("确定删除人员 %1 吗？Excel导入人员将彻底清除，手工录入人员将标记为已删除。").arg(personNo),
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

/** @brief 提示插入U盘并请求在后台导入本地人员及人脸图片。 */
void AdminPanel::requestImportPeopleFromUsb()
{
    QString requestError;
    const QString requestToken = UsbPersonImportSource::beginImportRequest(&requestError);
    if (requestToken.isEmpty()) {
        AppMessageDialog::warning(
                    this, QStringLiteral("导入本地人员"),
                    requestError.isEmpty() ? QStringLiteral("无法准备人员导入")
                                           : requestError);
        return;
    }

    const bool confirmed = AppMessageDialog::confirm(
                this,
                QStringLiteral("导入本地人员"),
                QStringLiteral("请先插入U盘再点击确定后进行导入"));
    if (!confirmed) {
        UsbPersonImportSource::endImportRequest(requestToken);
        return;
    }
    emit peopleImportRequested(requestToken);
}

/** @brief 切换导入忙碌状态和居中动画进度弹窗。 */
void AdminPanel::setPersonImportBusy(bool busy, int progress)
{
    if (importPeopleButton_) {
        importPeopleButton_->setEnabled(!busy);
        importPeopleButton_->setText(busy ? QStringLiteral("正在导入…")
                                          : QStringLiteral("导入人员表"));
    }
    if (exportPeopleButton_) exportPeopleButton_->setEnabled(!busy);

    if (!busy) {
        QuickUiRuntime::instance()->hideProgress();
        if (personImportSpinnerTimer_) personImportSpinnerTimer_->stop();
        if (personImportProgressOverlay_) {
            personImportProgressOverlay_->hide();
            personImportProgressOverlay_->deleteLater();
        }
        personImportProgressOverlay_ = nullptr;
        personImportSpinnerLabel_ = nullptr;
        personImportProgressBar_ = nullptr;
        personImportSpinnerTimer_ = nullptr;
        personImportSpinnerAngle_ = 0;
        return;
    }

    if (QuickUiRuntime::instance()->showProgress(QStringLiteral("导入中"), progress)) {
        if (personImportSpinnerTimer_) personImportSpinnerTimer_->stop();
        if (personImportProgressOverlay_) {
            personImportProgressOverlay_->hide();
            personImportProgressOverlay_->deleteLater();
        }
        personImportProgressOverlay_ = nullptr;
        personImportSpinnerLabel_ = nullptr;
        personImportProgressBar_ = nullptr;
        personImportSpinnerTimer_ = nullptr;
        personImportSpinnerAngle_ = 0;
        return;
    }

    if (!personImportProgressOverlay_) {
        personImportProgressOverlay_ = new QWidget(this);
        personImportProgressOverlay_->setObjectName(QStringLiteral("personImportProgressOverlay"));
        personImportProgressOverlay_->setAttribute(Qt::WA_StyledBackground, true);
        personImportProgressOverlay_->setStyleSheet(
                    QStringLiteral("QWidget#personImportProgressOverlay { background: rgba(2, 6, 23, 178); }"));

        auto *overlayLayout = new QVBoxLayout(personImportProgressOverlay_);
        overlayLayout->setContentsMargins(24, 24, 24, 24);

        auto *card = new QFrame(personImportProgressOverlay_);
        card->setObjectName(QStringLiteral("personImportProgressCard"));
        card->setMinimumSize(420, 260);
        card->setMaximumWidth(520);
        card->setStyleSheet(QStringLiteral(
            "QFrame#personImportProgressCard { background: #0f172a; border: 1px solid #475569; border-radius: 18px; }"
            "QLabel#personImportProgressTitle { color: #f8fafc; font-size: 24px; font-weight: 700; }"
            "QProgressBar#personImportProgressBar { color: #f8fafc; background: #1e293b; border: 1px solid #475569;"
            " border-radius: 9px; text-align: center; min-height: 22px; font-size: 15px; }"
            "QProgressBar#personImportProgressBar::chunk { background: #2dd4bf; border-radius: 8px; }"));

        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(36, 30, 36, 30);
        cardLayout->setSpacing(24);

        auto *title = new QLabel(QStringLiteral("导入中"), card);
        title->setObjectName(QStringLiteral("personImportProgressTitle"));
        title->setAlignment(Qt::AlignCenter);
        cardLayout->addWidget(title);

        personImportSpinnerLabel_ = new QLabel(card);
        personImportSpinnerLabel_->setFixedSize(72, 72);
        personImportSpinnerLabel_->setAlignment(Qt::AlignCenter);
        cardLayout->addWidget(personImportSpinnerLabel_, 0, Qt::AlignHCenter);

        personImportProgressBar_ = new QProgressBar(card);
        personImportProgressBar_->setObjectName(QStringLiteral("personImportProgressBar"));
        personImportProgressBar_->setRange(0, 100);
        personImportProgressBar_->setFormat(QStringLiteral("%p%"));
        personImportProgressBar_->setTextVisible(true);
        cardLayout->addWidget(personImportProgressBar_);

        overlayLayout->addWidget(card, 0, Qt::AlignCenter);
        personImportSpinnerTimer_ = new QTimer(personImportProgressOverlay_);
        const auto updateSpinner = [this]() {
            if (!personImportSpinnerLabel_) return;
            QPixmap pixmap(72, 72);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.translate(36.0, 36.0);
            painter.rotate(personImportSpinnerAngle_);
            painter.translate(-36.0, -36.0);
            QPen pen(QColor(QStringLiteral("#2dd4bf")), 6.0, Qt::SolidLine, Qt::RoundCap);
            painter.setPen(pen);
            painter.drawArc(QRectF(9.0, 9.0, 54.0, 54.0), 0, 270 * 16);
            personImportSpinnerLabel_->setPixmap(pixmap);
            personImportSpinnerAngle_ = (personImportSpinnerAngle_ + 12) % 360;
        };
        connect(personImportSpinnerTimer_, &QTimer::timeout, this, updateSpinner);
        updateSpinner();
        personImportSpinnerTimer_->start(40);
    }

    personImportProgressBar_->setValue(qBound(0, progress, 100));
    personImportProgressOverlay_->setGeometry(rect());
    personImportProgressOverlay_->raise();
    personImportProgressOverlay_->show();
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

/** @brief 切换导出忙碌状态，防止导出和导入任务并发启动。 */
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

/** @brief 切换到网络人员页，供同步完成后直接展示刷新结果。 */
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
    config_.audioPlayer = QStringLiteral("gstreamer");
    if (config_.audioDevice.trimmed().isEmpty()) {
        config_.audioDevice = QStringLiteral("plughw:CARD=rockchiprk809co,DEV=0");
    }
    if (config_.audioDir.trimmed().isEmpty()) {
        config_.audioDir = QStringLiteral("./common/audio");
    }
    config_.audioVolume = audioVolumeSlider_ ? qBound(0, audioVolumeSlider_->value(), 100) : config_.audioVolume;
    if (config_.audioMixerControl.trimmed().isEmpty()) {
        config_.audioMixerControl = QStringLiteral("numid=3");
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
    showActionToast(ok ? QStringLiteral("时间设置成功")
                       : QStringLiteral("时间设置失败"), ok);
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

/** @brief 更新轻量摘要，并把系统状态读取投递到后台线程。 */
void AdminPanel::updateDeviceInfo()
{
    updateDeviceSummaryLabels();
    requestDeviceStatusRefresh();
}

/** @brief 只更新不需要访问系统节点的设备信息。 */
void AdminPanel::updateDeviceSummaryLabels()
{
    if (versionLabel_) {
        const QString version = QCoreApplication::applicationVersion().isEmpty()
            ? QStringLiteral("FaceGate V1.0.0")
            : QStringLiteral("FaceGate V") + QCoreApplication::applicationVersion();
        versionLabel_->setText(version);
    }
    if (uptimeLabel_) {
        const qint64 startMs = qApp->property("appStartMsecsSinceEpoch").toLongLong();
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        uptimeLabel_->setText(startMs > 0
            ? formatRunDuration((nowMs - startMs) / 1000)
            : QStringLiteral("--"));
    }
    if (peopleCapacityLabel_) {
        peopleCapacityLabel_->setText(QString("%1/3000").arg(storageStats_.personCount));
    }
    if (faceCapacityLabel_) {
        const qint64 gibibyte = 1024LL * 1024LL * 1024LL;
        const qint64 registrationGiB = StoragePolicy::registrationLimitBytes() / gibibyte;
        const int photoCapacity = StoragePolicy::registrationPhotoCapacity();
        faceCapacityLabel_->setText(
            QStringLiteral("%1 张（%2 / %3 GiB，约 %4 张）")
                .arg(storageStats_.faceFeatureCount)
                .arg(formatBytes(storageStats_.registrationPhotoBytes))
                .arg(registrationGiB)
                .arg(photoCapacity));
    }
    if (verifyLogCountLabel_) {
        verifyLogCountLabel_->setText(QString::number(storageStats_.verifyLogCount));
    }
}

/** @brief 仅在面板可见且停留在设备信息页时运行定时刷新。 */
void AdminPanel::updateDeviceStatusRefreshState()
{
    if (!deviceStatusTimer_ || !stack_) {
        return;
    }
    const bool shouldRefresh = isVisible()
        && stack_->currentIndex() == kDeviceInfoPageIndex;
    if (shouldRefresh) {
        if (!deviceStatusTimer_->isActive()) {
            deviceStatusTimer_->start();
        }
    } else {
        deviceStatusTimer_->stop();
    }
}

/** @brief 在专用线程执行一次设备状态采集，避免阻塞 GUI 事件循环。 */
void AdminPanel::requestDeviceStatusRefresh()
{
    if (!isVisible() || !stack_ || stack_->currentIndex() != kDeviceInfoPageIndex
        || !deviceStatusCollector_ || !deviceStatusThread_.isRunning()
        || deviceStatusRequestPending_) {
        return;
    }

    auto *collector = static_cast<DeviceStatusCollector *>(deviceStatusCollector_);
    deviceStatusRequestPending_ = true;
    const bool queued = QMetaObject::invokeMethod(
        collector,
        [this, collector]() {
            const DeviceStatusSnapshot snapshot = collector->collect();
            QMetaObject::invokeMethod(
                this,
                [this, snapshot]() {
                    deviceStatusRequestPending_ = false;
                    if (isVisible() && stack_
                        && stack_->currentIndex() == kDeviceInfoPageIndex) {
                        applyDeviceStatusSnapshot(snapshot);
                    }
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    if (!queued) {
        deviceStatusRequestPending_ = false;
        qWarning() << "设备状态后台采集请求投递失败";
    }
}

/** @brief 在 GUI 线程更新后台采集完成的设备状态标签。 */
void AdminPanel::applyDeviceStatusSnapshot(const DeviceStatusSnapshot &snapshot)
{
    if (serialLabel_) serialLabel_->setText(snapshot.serialNumber);
    if (ipLabel_) ipLabel_->setText(snapshot.ipAddress);
    if (macLabel_) macLabel_->setText(snapshot.macAddress);
    if (diskCapacityLabel_) {
        diskCapacityLabel_->setText(snapshot.diskCapacity);
        diskCapacityLabel_->setToolTip(snapshot.diskRootPath);
    }
    if (cpuUsageLabel_) cpuUsageLabel_->setText(snapshot.cpuUsage);
    if (memoryUsageLabel_) memoryUsageLabel_->setText(snapshot.memoryUsage);
    if (temperatureLabel_) temperatureLabel_->setText(snapshot.temperature);
    if (npuUsageLabel_) npuUsageLabel_->setText(snapshot.npuUsage);
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

/** @brief 处理内容页和网络同步提示的尺寸变化。 */
bool AdminPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == networkPeoplePage_ && event->type() == QEvent::Resize) {
        positionNetworkSyncToast();
    }
    return QDialog::eventFilter(watched, event);
}

/** @brief 尺寸变化时重排导航和提示。 */
void AdminPanel::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    applyResponsiveLayout();
    positionNetworkSyncToast();
    positionActionToast();
    if (personImportProgressOverlay_) {
        personImportProgressOverlay_->setGeometry(rect());
        personImportProgressOverlay_->raise();
    }
}

void AdminPanel::applyResponsiveLayout()
{
    if (!rootLayout_ || !sideLayout_ || !sidebar_ || !navList_ ||
            !adminTitle_ || !adminSubtitle_ || !livenessCheck_ || !backButton_ ||
            !quickNavigation_) {
        return;
    }

    const bool top = ResponsiveUi::usesTopNavigation(
                ResponsiveUi::layoutReferenceSize(this));
    topNavigation_ = top;

    const bool quickReady = quickNavigation_->status() == QQuickWidget::Ready;
    quickNavigation_->setVisible(quickReady);
    sidebar_->setVisible(!quickReady);
    if (!quickReady) {
        rootLayout_->setDirection(QBoxLayout::LeftToRight);
        sidebar_->setMinimumWidth(kAdminSidebarWidth);
        sidebar_->setMaximumWidth(kAdminSidebarWidth);
        rootLayout_->invalidate();
        return;
    }

    // Quick 导航只占用页面边缘的一块小 FBO；管理后台显示时多媒体已经暂停，
    // 因而不会形成视频 + QML 的持续叠加。横竖屏只调整同一实例，不重建场景。
    rootLayout_->setDirection(top ? QBoxLayout::TopToBottom
                                  : QBoxLayout::LeftToRight);
    quickNavigation_->setHorizontal(top);
    if (top) {
        quickNavigation_->setMinimumWidth(0);
        quickNavigation_->setMaximumWidth(QWIDGETSIZE_MAX);
        quickNavigation_->setMinimumHeight(92);
        quickNavigation_->setMaximumHeight(112);
    } else {
        quickNavigation_->setMinimumHeight(0);
        quickNavigation_->setMaximumHeight(QWIDGETSIZE_MAX);
        quickNavigation_->setMinimumWidth(kAdminSidebarWidth);
        quickNavigation_->setMaximumWidth(kAdminSidebarWidth);
    }
    rootLayout_->invalidate();
}

/** @brief 通知 MainWindow 当前管理面板正在关闭。 */
void AdminPanel::closeEvent(QCloseEvent *event)
{
    if (deviceStatusTimer_) {
        deviceStatusTimer_->stop();
    }
    QWidget *focus = QApplication::focusWidget();
    if (focus && isAncestorOf(focus)) {
        focus->clearFocus();
    }
    emit closed();
    QDialog::closeEvent(event);
}

/** @brief 面板显示后，仅在设备信息页启动后台状态刷新。 */
void AdminPanel::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    applyResponsiveLayout();
    updateDeviceStatusRefreshState();
    if (stack_ && stack_->currentIndex() == kDeviceInfoPageIndex) {
        updateDeviceInfo();
    }
}

/** @brief 面板不可见时立即停止周期刷新。 */
void AdminPanel::hideEvent(QHideEvent *event)
{
    if (deviceStatusTimer_) {
        deviceStatusTimer_->stop();
    }
    QDialog::hideEvent(event);
}
