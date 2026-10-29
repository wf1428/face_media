/**
 * @file networkpage.cpp
 * @brief 有线网络 DHCP/静态地址配置与自动恢复页面的实现。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-10-27；修改人：Du lin；手动应用只在目标连接激活、IPv4 参数就绪并保存后弹“网络设置成功”；DHCP 等待沿用后台采样且不弹中间状态，提示按配置代次失效并去重，缓存保存拒绝非普通文件路径，保留原通知和正常配置规则。
 */

#include "networkpage.h"
#include "ui_networkpage.h"

#include "components/dialogs/ui_message_box.h"
#include "common/network_config.h"
#include <QHostAddress>
#include <QAbstractSocket>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QDebug>
#include <QThread>
#include <QSignalBlocker>
#include <QCheckBox>
#include <QKeyEvent>
#include "common/toggle_switch.h"

#include "platform/rk3566_platform.h"

/**
 * @brief 复用工程滑动开关外观，为速率选择增加左/右点击、拖动和方向键定位。
 * @note 不自行配置网络；true 只表示右侧百兆，false 表示左侧千兆。
 */
class NetworkRateSwitch final : public ToggleSwitch {
public:

    /** @brief 创建仅编辑速率选择的工程滑块。
     *  @param parent Qt 父控件，继承共用控件的 66×38 尺寸和动画生命周期。 */
    explicit NetworkRateSwitch(QWidget *parent = nullptr) : ToggleSwitch(parent)
    {
    }

protected:

    /** @brief 开始一次左键选择；其他按键沿用原控件行为。
     *  @param event Qt 分派的输入事件，借用且仅在调用期间有效。 */
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            ToggleSwitch::mousePressEvent(event);
            return;
        }
        setFocus(Qt::MouseFocusReason);
        setDown(true);
        event->accept();
    }

    /** @brief 拖动时只移动滑块，释放时再提交界面选择；不调用网络。
     *  @param event Qt 分派的输入事件，借用且仅在调用期间有效。 */
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!isDown()) {
            ToggleSwitch::mouseMoveEvent(event);
            return;
        }
        setChecked(event->pos().x() >= width() / 2);
        event->accept();
    }

    /** @brief 在释放位置选择左千兆/右百兆，点击已选一侧也保持该侧。
     *  @param event Qt 分派的输入事件，借用且仅在调用期间有效。 */
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !isDown()) {
            ToggleSwitch::mouseReleaseEvent(event);
            return;
        }
        setDown(false);
        setChecked(event->pos().x() >= width() / 2);
        emit clicked(isChecked());
        event->accept();
    }

    /** @brief 左右方向键选择对应端点，空格仍使用原开关切换行为。
     *  @param event Qt 分派的输入事件，借用且仅在调用期间有效。 */
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
            setChecked(event->key() == Qt::Key_Right);
            emit clicked(isChecked());
            event->accept();
            return;
        }
        ToggleSwitch::keyPressEvent(event);
    }
};

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

/**
 * @brief 后台串行运行系统调用，结果和界面操作回到 GUI 线程执行。
 * @param work 在专用网络线程串行执行的工作，不能直接刷新控件。
 * @param completed 结果回到 GUI 线程后的完成回调。
 * @note 同一时刻只允许一个后台网络任务；配置代次变化时丢弃过期回调并保留未应用的速率选择，完成后再处理延期操作。
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
            else {
                if (!webNetworkToken_.isEmpty()) {
                    const auto token = webNetworkToken_;
                    webNetworkToken_.clear();
                    emit webNetworkFinished(
                        token,
                        {},
                        409,
                        QStringLiteral("网络设置已被设备端新操作取代，请刷新核对"));
                }
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
 * @brief 在工作线程按需启动 NM 并合并状态回显请求，丢弃过期结果。
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
        ManagedNetwork::Backend backend;
        // Qt 异步使能服务；全局服务供有线和热点共用，离线仅断开有线。
        if (!backend.ensureService(&state->error)) return;
        *state = backend.query(iface);
    }, [this, state, completed]() {
        if (!state->available) return;
        ui->ipAddressEdit->setText(state->ip.isEmpty() ? "-" : state->ip);
        ui->subnetMaskEdit->setText(state->mask.isEmpty() ? "-" : state->mask);
        ui->gatewayEdit->setText(state->gateway.isEmpty() ? "-" : state->gateway);
        ui->dnsEdit->setText(state->dns.isEmpty() ? "-" : state->dns);
        if (completed) completed();
    });
}

/**
 * @brief 创建网络 UI、速率选项、串行工作线程及两秒 NM 状态采样定时器。
 * @param parent Qt 父对象；对象销毁由父子关系管理，空指针表示无父对象。
 */
NetworkPage::NetworkPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::NetworkPage)
{
    ui->setupUi(this);
    // 使用工程滑块暂存选择；只有网络页应用按钮提交，后台采样不提交。
    auto *linkLabel = new QLabel(QStringLiteral("速率:"), ui->connectionTypeGroup);
    linkLabel->setStyleSheet(ui->label->styleSheet());
    auto *rateGroup = new QWidget(ui->connectionTypeGroup);
    auto *rateLayout = new QHBoxLayout(rateGroup);
    rateLayout->setContentsMargins(0, 0, 0, 0);
    rateLayout->setSpacing(7);
    gigabitRateLabel_ = new QLabel(QStringLiteral("千兆"), rateGroup);
    gigabitRateLabel_->setObjectName(QStringLiteral("gigabitRateLabel"));
    linkModeSwitch_ = new NetworkRateSwitch(rateGroup);
    linkModeSwitch_->setObjectName(QStringLiteral("linkModeSwitch"));
    linkModeSwitch_->setAccessibleName(QStringLiteral("有线速率：左千兆，右百兆"));
    hundredRateLabel_ = new QLabel(QStringLiteral("百兆"), rateGroup);
    hundredRateLabel_->setObjectName(QStringLiteral("hundredRateLabel"));
    rateLayout->addWidget(gigabitRateLabel_);
    rateLayout->addWidget(linkModeSwitch_);
    rateLayout->addWidget(hundredRateLabel_);
    ui->horizontalSpacer->changeSize(0, 0, QSizePolicy::Minimum, QSizePolicy::Minimum);
    ui->horizontalLayout->insertWidget(2, linkLabel);
    ui->horizontalLayout->insertWidget(3, rateGroup);
    QSettings linkIni(QDir(configDir_).filePath(configFileName_), QSettings::IniFormat);
    ifaceName_ = linkIni.value("network/iface", "eth0").toString().trimmed();
    if (ifaceName_.isEmpty()) ifaceName_ = "eth0";
    loadSavedLinkMode(true);
    updateLinkModeControl();
    // 真实点击仅更新待应用值；启动回显和后台轮询不提交选择。
    connect(linkModeSwitch_, &QCheckBox::clicked, this, &NetworkPage::handleLinkModeClicked);

    networkThread_ = new QThread(this);
    networkThread_->setObjectName(QStringLiteral("NetworkManagerThread"));
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
    refreshUiFromSystem(ifaceName_);

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
    updateLinkModeControl();
    if (index == 0) {
        // 有线页面
        ui->networkStack->setCurrentIndex(0);

        // 有线状态回显
        refreshUiFromSystem(ifaceName_);
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
 * @brief 从 net_cfg.ini 读取保存速率，缺少键时持久化百兆默认值。
 * @param discardSelection true 丢弃未应用选择；false 保留已编辑的滑块位置。
 * @return 文件可读、模式有效且必要的初始化写入成功时为 true，否则记录失败并返回 false。
 * @note 本方法不启用网络或修改 NM；只回显，不触发 clicked。
 */
bool NetworkPage::loadSavedLinkMode(bool discardSelection)
{
    const QString path = QDir(configDir_).filePath(configFileName_);
    QString error;
    const auto config = NetworkConfig::read(path, &error);
    const QString mode = config.value("linkMode").toString("100-full");
    if (!error.isEmpty() || !NetworkConfig::isLinkModeValid(mode)) {
        appendLog(error.isEmpty() ? QStringLiteral("net_cfg.ini 中的链路模式无效") : error);
        return false;
    }
    QSettings ini(path, QSettings::IniFormat);
    if (!ini.contains("network/link_mode") &&
        !NetworkConfig::saveLinkMode(path, config.value("iface").toString(), mode, &error)) {
        appendLog(error);
        return false;
    }
    if (discardSelection || !linkModeSelectionDirty_)
        selectedLinkMode_ = mode;
    linkMode_ = mode;
    linkModeSelectionDirty_ = selectedLinkMode_ != linkMode_;
    updateLinkModeControl();
    return true;
}

/** @brief 回显待应用选择；左右标签始终固定，网络命令只由应用按钮发起。 */
void NetworkPage::updateLinkModeControl()
{
    if (!linkModeSwitch_)
        return;
    const QSignalBlocker blocker(linkModeSwitch_);
    const bool hundred = selectedLinkMode_ == "100-full";
    linkModeSwitch_->setChecked(hundred);
    linkModeSwitch_->setEnabled(ui->connectionTypeCombo->currentIndex() == 0);
    linkModeSwitch_->setToolTip(linkModeSelectionDirty_
                                    ? QStringLiteral("速率尚未应用，请点击下方应用")
                                    : (selectedLinkMode_ == "auto"
                                           ? QStringLiteral("已保存旧自动协商模式；选择后点击应用")
                                           : QStringLiteral("左侧千兆，右侧百兆；点击应用后生效")));
    gigabitRateLabel_->setStyleSheet(
        hundred ? QStringLiteral("font-size:14px;color:#666;")
                : QStringLiteral("font-size:14px;color:#1677ff;font-weight:600;"));
    hundredRateLabel_->setStyleSheet(
        hundred ? QStringLiteral("font-size:14px;color:#1677ff;font-weight:600;")
                : QStringLiteral("font-size:14px;color:#666;"));
}

/**
 * @brief 只记录速率编辑，不调用 NM、不写 INI，也不取消正在采集的网络状态。
 * @param hundred true 为右侧百兆；false 为左侧千兆全双工。
 */
void NetworkPage::handleLinkModeClicked(bool hundred)
{
    selectedLinkMode_ = hundred ? QStringLiteral("100-full") : QStringLiteral("1000-full");
    linkModeSelectionDirty_ = selectedLinkMode_ != linkMode_;
    updateLinkModeControl();
}

/**
 * @brief 保存并应用当前网络设置。
 */
void NetworkPage::handleApplyClicked()
{
    applyNetworkConfig(true);
}

/**
 * @brief 校验并固定点击应用时的 IPv4 与速率输入，再串行提交 NM；权限和无线规则保持原样。
 * @param showMessage 是否显示设备端提示框。
 * @return 已受理后台任务时为 true；权限或输入失败返回 false。
 */
bool NetworkPage::applyNetworkConfig(bool showMessage)
{
    const quint64 generation = networkGeneration_->fetch_add(1) + 1;
    networkRefreshPending_ = false;
    const QString iface = ifaceName_.trimmed().isEmpty() ? QString("eth0") : ifaceName_;
    if (!isNetworkFeatureEnabled()) {
        if (featureSettings_.offline_v1) {
            if (networkTaskPending_) {
                deferredNetworkAction_ = [this, iface, generation]() {
                    if (generation == networkGeneration_->load())
                        clearManagedNetwork(iface);
                };
            } else
                clearManagedNetwork(iface);
        }
        if (showMessage) UiMessageBox::warning(this, "权限不足", "未启用网络功能");
        appendLog("未启用网络功能");
        return false;
    }
    if (ui->connectionTypeCombo->currentIndex() == 1) {
        if (showMessage) UiMessageBox::information(this, "提示", "无线网络功能开发中，暂不可用。");
        appendLog("无线网络功能开发中，暂不可用");
        return false;
    }
    if (!loadSavedLinkMode())
        return false;
    const bool dhcp = ui->ipModeComboBox->currentIndex() == 0;
    QJsonObject config{{"iface", iface}, {"dhcp", dhcp}, {"linkMode", selectedLinkMode_}};
    if (!dhcp) {
        config["ip"] = ui->ipAddressEdit->text().trimmed();
        config["netmask"] = ui->subnetMaskEdit->text().trimmed();
        config["gateway"] = ui->gatewayEdit->text().trimmed();
        config["dns"] = ui->dnsEdit->text().trimmed();
        QString error;
        if (!isIPv4(config.value("ip").toString())) error = "IP 地址不合法";
        else if (netmaskToPrefix(config.value("netmask").toString()) < 0) error = "子网掩码不合法";
        else if (!config.value("gateway").toString().isEmpty() && !isIPv4(config.value("gateway").toString())) error = "网关不合法";
        else if (!config.value("dns").toString().isEmpty() && !isIPv4(config.value("dns").toString())) error = "DNS 不合法";
        if (!error.isEmpty()) {
            if (showMessage) UiMessageBox::warning(this, "输入错误", error);
            appendLog(error);
            return false;
        }
    }
    if (networkTaskPending_) {
        // 在点击应用时固定整份输入；排队期间的新滑块选择留到下一次应用。
        deferredNetworkAction_ = [this, config, showMessage, generation]() {
            if (generation == networkGeneration_->load())
                applyManagedNetworkConfig(config, showMessage);
        };
    } else
        applyManagedNetworkConfig(config, showMessage);
    return true;
}

/**
 * @brief 在网络线程统一应用 IPv4 与所选速率，NM 接受后同步 INI。
 * @param config 已校验的有线配置；DHCP 地址只作缓存，不转换成静态配置。
 * @param showMessage 是否显示设备端提示；成功弹窗等待参数就绪并保存，原业务通知链保持不变。
 * @note DHCP 等待时仅保存速率，IPv4 字段沿用原延后保存规则；后台采样不提交待应用速率。
 *       涉及速率变更的应用或保存失败时尝试按原 INI 恢复 NM，失败会报告，不承诺跨文件原子提交。
 */
void NetworkPage::applyManagedNetworkConfig(const QJsonObject &config, bool showMessage)
{
    /** @brief 单次本地请求的线程间值对象；不持有界面对象。 */
    struct Result {
        NetworkRuntimeState state; /**< 工作线程取得的 NM 状态。 */
        QString error;             /**< 后端或落盘失败说明，含速率恢复请求结果。 */
        bool saved = false;        /**< true 表示 NM 已受理并通过必要回读，且本次 INI 同步成功。 */
    };
    auto result = std::make_shared<Result>();
    const auto generation = networkGeneration_;
    const quint64 expected = generation->load();
    const QString path = QDir(configDir_).filePath(configFileName_);
    runNetworkTask(
        [config, path, result, generation, expected]() {
            const auto valid = [generation, expected]() {
                return generation->load() == expected;
            };
            const auto original = NetworkConfig::read(path, &result->error);
            if (!result->error.isEmpty() || !valid())
                return;
            ManagedNetwork::Backend backend({}, valid);
            const bool accepted = backend.apply(config, &result->state, &result->error);
            if (accepted && valid()) {
                // 一次同步保存已提交的速率和原 IPv4 字段，后台缓存保存入口不会读取滑块草稿。
                if (!QFileInfo(path).isFile()) {
                    result->error = QStringLiteral("保存网络配置失败：net_cfg.ini 不是普通文件");
                } else {
                    QSettings ini(path, QSettings::IniFormat);
                    ini.setIniCodec("UTF-8");
                    ini.sync();
                    QString iface = ini.value("network/iface", "eth0").toString().trimmed();
                    if (iface.isEmpty())
                        iface = "eth0";
                    if (ini.status() != QSettings::NoError ||
                        iface != config.value("iface").toString()) {
                        result->error =
                            QStringLiteral("网络配置读取失败或目标接口已变化，请刷新设置");
                    } else if (valid()) {
                        ini.setValue("network/link_mode", config.value("linkMode").toString());
                        const auto &state = result->state;
                        if (!config.value("dhcp").toBool() || state.hasAddress()) {
                            ini.setValue("network/use_config", true);
                            ini.setValue("network/iface", iface);
                            ini.setValue("network/auto_connect", true);
                            ini.setValue("network/dhcp", config.value("dhcp").toBool());
                            ini.setValue("network/ip", state.ip);
                            ini.setValue("network/netmask", state.mask);
                            ini.setValue("network/gateway", state.gateway);
                            ini.setValue("network/dns", state.dns);
                        } // DHCP 尚无地址时保留原 IPv4 保存时机，不改变 Web 联网意图。
                        ini.sync();
                        result->saved = ini.status() == QSettings::NoError;
                        if (!result->saved)
                            result->error = QStringLiteral("保存网络配置失败：") + path;
                    }
                }
            }
            if (!result->saved && valid() &&
                config.value("linkMode") != original.value("linkMode")) {
                const QString reason = result->error;
                QString restoreError;
                NetworkRuntimeState restoredState;
                ManagedNetwork::Backend restore({}, valid);
                const bool restored = restore.apply(original, &restoredState, &restoreError);
                result->error =
                    reason + (restored ? QStringLiteral("；已按原 INI 选择提交恢复")
                                       : QStringLiteral("；恢复原配置失败：") + restoreError);
            }
        },
        [this, config, showMessage, result, expected]() {
            // 应用期间仍可编辑下一次选择；仅本次提交的选择随成功/失败回显，不吞掉更新的编辑。
            const QString selection = selectedLinkMode_;
            const bool newerSelection = selection != config.value("linkMode").toString();
            loadSavedLinkMode(true);
            selectedLinkMode_ = newerSelection ? selection : linkMode_;
            linkModeSelectionDirty_ = selectedLinkMode_ != linkMode_;
            updateLinkModeControl();
            if (!result->saved) {
                appendLog(result->error);
                if (showMessage)
                    UiMessageBox::warning(this, "网络配置失败", result->error);
                return;
            }
            appendLog(QStringLiteral("网络配置已保存：") +
                      QDir(configDir_).filePath(configFileName_));
            const auto &state = result->state;
            const bool dhcp = config.value("dhcp").toBool();
            ui->ipAddressEdit->setText(state.ip.isEmpty() ? "-" : state.ip);
            ui->subnetMaskEdit->setText(state.mask.isEmpty() ? "-" : state.mask);
            ui->gatewayEdit->setText(state.gateway.isEmpty() ? "-" : state.gateway);
            ui->dnsEdit->setText(state.dns.isEmpty() ? "-" : state.dns);
            lastCarrierUp_ = state.carrierUp;
            dhcpHadValidIp_ = state.hasAddress();
            lastIp_ = state.ip;
            lastMask_ = state.mask;
            lastGateway_ = state.gateway;
            lastDns_ = state.dns;
            lastDhcpAutoTryMs_ = QDateTime::currentMSecsSinceEpoch();
            const QString message =
                dhcp ? (state.hasAddress()
                            ? QStringLiteral("DHCP 已获取网络参数")
                            : QStringLiteral("DHCP 已提交 NetworkManager，等待获取网络参数"))
                     : QStringLiteral("静态 IP 已应用");
            appendLog(message);
            if (showMessage) {
                pendingNetworkSuccessGeneration_ = expected;
                pendingNetworkSuccessIface_ = config.value("iface").toString();
                pendingNetworkSuccessUuid_ = state.uuid;
                // 等待中只登记提示；现有两秒采样在租约就绪且缓存保存后再显示。
                showNetworkSuccessIfReady(pendingNetworkSuccessIface_, state);
            }
            emit networkAppliedSuccessfully();
        });
}

/**
 * @brief 仅在当前手动应用的目标连接已激活、参数保存后弹一次成功提示。
 * @param iface 已保存参数所属的接口名称。
 * @param state 本次实际快照，不接受失败、仍激活中或没有有效地址的结果。
 * @note GUI 线程调用；先消费提示再开模态框，避免嵌套事件循环中的轮询重复弹窗。
 *       配置代次变化会取消旧提示；可选网关/DNS 为空不影响合法配置成功。
 */
void NetworkPage::showNetworkSuccessIfReady(const QString &iface, const NetworkRuntimeState &state)
{
    if (!pendingNetworkSuccessGeneration_)
        return;
    if (pendingNetworkSuccessGeneration_ != networkGeneration_->load()) {
        pendingNetworkSuccessGeneration_ = 0;
        return;
    }
    if (iface != pendingNetworkSuccessIface_ || pendingNetworkSuccessUuid_.isEmpty() ||
        state.uuid != pendingNetworkSuccessUuid_ || !state.available || state.deviceState != 100 ||
        !state.hasAddress())
        return;
    pendingNetworkSuccessGeneration_ = 0;
    UiMessageBox::information(this, QStringLiteral("网络设置"), QStringLiteral("网络设置成功"));
}

/**
 * @brief 经 NM 异步断开有线连接并沿用原离线地址缓存清理，不停止热点所需的全局服务。
 * @param iface 原配置确定的目标有线接口。
 * @note 和其他网络任务串行；失败记录原因，配置清理仍按原 offline 规则执行。
 */
void NetworkPage::clearManagedNetwork(const QString &iface)
{
    auto error = std::make_shared<QString>();
    const auto generation = networkGeneration_;
    const quint64 expected = generation->load();
    runNetworkTask([iface, error, generation, expected]() {
        ManagedNetwork::Backend backend({}, [generation, expected]() { return generation->load() == expected; });
        backend.disconnect(iface, error.get());
    }, [this, iface, error]() {
        if (!error->isEmpty()) appendLog(QStringLiteral("离线清理有线网络失败：") + *error);
        if (!clearNetworkConfigFileIp(configDir_)) appendLog(QStringLiteral("清理配置文件 IP 失败"));
        lastIp_.clear(); lastMask_.clear(); lastGateway_.clear(); lastDns_.clear();
        dhcpHadValidIp_ = false;
        refreshUiFromSystem(iface);
    });
}

/**
 * @brief Web 经统一 NM 后端在共享网络线程应用并回读配置，保留原权限、校验及 INI 保存规则。
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
    // 目标名称由 INI 限定；后台以 NM 的实际 ethernet 类型校验接口，避免第二套网卡识别。
    /** @brief Web 请求的后台结果；所有界面更新在完成回调中执行。 */
    struct Result {
        NetworkRuntimeState state; /**< 实际 NM 状态。 */
        QJsonObject config; /**< 原模式字段及本次回读的租约/速率配置。 */
        QString error; /**< 失败说明，允许在 IPC 回执中返回。 */
        bool saved = false; /**< true 表示 NM 受理后 INI 也同步成功。 */
        bool applied = false; /**< true 表示所需 IPv4 已生效；DHCP 等待时为 false。 */
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
            auto &error = result->error;
            ManagedNetwork::Backend backend({}, valid);
            if (!valid() || !backend.apply(config, &result->state, &error)) return;
            if (!valid()) return;
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
            loadSavedLinkMode(true);
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
 * @brief 从 INI 回显网络输入及链路模式，不直接配置网卡。
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
    if (!loadSavedLinkMode(true))
        return false;
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
 * @brief 按原 use_config/auto_connect 和功能权限恢复联网，复用匹配的 NM 活动连接。
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
 * @brief 保存原 IPv4 字段；租约缓存不转成静态配置，也不覆盖权威速率键。
 * @param cfgDir 网络配置所在目录。
 * @return 现有普通配置文件同步成功为 true；路径异常或写失败时记录原因并返回 false。
 * @note 拒绝目录或缺失文件，避免错误路径产生的待写缓存污染恢复后的原配置。
 */
bool NetworkPage::saveInputsToConfigFile(const QString& cfgDir)
{
    const QString cfgPath = QDir(cfgDir).filePath(configFileName_);
    if (!QFileInfo(cfgPath).isFile()) {
        appendLog(QStringLiteral("保存网络配置失败：配置路径不是普通文件：") + cfgPath);
        return false;
    }

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
 * @brief 更新原功能权限；无 Web 联网意图的 offline 模式经 NM 异步断开有线。
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
        // 目标接口来自持久化配置，不能假定总是 eth0。
        ifaceName_ = networkIni.value("network/iface", "eth0").toString().trimmed();
        clearManagedNetwork(ifaceName_.isEmpty() ? iface : ifaceName_);
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
 * @note 轮询仅提交后台采样请求；已有网络任务时合并或延期，界面线程不执行阻塞 NM 命令。
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
        *state = ManagedNetwork::Backend().query(iface);
    }, [this, iface, state]() {
        applyDhcpRuntimeState(iface, *state);
    });
}

/**
 * @brief 消费 NM 快照并沿用原载波/地址通知规则；参数保存成功后补发手动应用的成功弹窗。
 * @param iface 目标有线网卡名称。
 * @param state 工作线程采集的当前网卡状态。
 */
void NetworkPage::applyDhcpRuntimeState(const QString &iface,
                                      const NetworkRuntimeState &state)
{

    if (!state.available) {
        if (lastNetworkError_ != state.error) appendLog(state.error);
        lastNetworkError_ = state.error;
        return; // 查询失败不能清空已保存地址，也不能触发连接重建。
    }
    lastNetworkError_.clear();
    // 先看物理链路是否存在
    const bool carrierUp = state.carrierUp;
    const bool linkBecameUp = carrierUp && !lastCarrierUp_;
    const bool linkBecameDown = !carrierUp && lastCarrierUp_;

    lastCarrierUp_ = carrierUp;

    if (linkBecameUp) {
        appendLog(QString("检测到网线插入：%1 carrier=1").arg(iface));

        lastDhcpAutoTryMs_ = 0;
        if (!state.activating() && !state.hasAddress()) requestAutoNetworkActivation("网线插入后请求 NM 获取 DHCP");
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

    // NM 自行处理 DHCP 重试和续租；正在激活时不因固定超时反复断开连接。
    if (!hasValidIp) {
        if (!state.activating()) requestAutoNetworkActivation("有线已连接但 NM 未取得 IPv4");
        return;
    }

    const bool changed =
        (ip != lastIp_) ||
        (mask != lastMask_) ||
        (gw != lastGateway_) ||
        (dns != lastDns_);

    const bool firstValidIp = !dhcpHadValidIp_;

    if (!changed && !firstValidIp) {
        // 地址可能早于激活结束到达，或前次保存失败；只为待提示请求重试保存与就绪确认。
        if (pendingNetworkSuccessGeneration_ == networkGeneration_->load() &&
            pendingNetworkSuccessIface_ == iface && pendingNetworkSuccessUuid_ == state.uuid &&
            state.deviceState == 100 && saveInputsToConfigFile(configDir_))
            showNetworkSuccessIfReady(iface, state);
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

    const bool saved = saveInputsToConfigFile(configDir_);
    if (saved)
        showNetworkSuccessIfReady(iface, state);

    appendLog(QString("检测到 DHCP 网络参数有效，已同步配置文件：ip=%1 mask=%2 gw=%3 dns=%4")
              .arg(ip, mask, gw, dns));

    emit networkAppliedSuccessfully();
}


/**
 * @brief 保留原 15 秒恢复节流，仅在 NM 未激活时提交一次有线连接请求。
 * @param reason 触发原因，用于诊断日志。
 * @note 不启动/终止 DHCP 进程，不清空地址路由；NM 激活中由状态采样直接等待。
 */
void NetworkPage::requestAutoNetworkActivation(const QString &reason)
{
    if (!ui || networkTaskPending_ || dhcpAutoApplying_ || !isNetworkFeatureEnabled() ||
        ui->connectionTypeCombo->currentIndex() != 0 || ui->ipModeComboBox->currentIndex() != 0 ||
        !lastCarrierUp_) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastDhcpAutoTryMs_ > 0 && now - lastDhcpAutoTryMs_ < kDhcpAutoRetryIntervalMs) return;
    lastDhcpAutoTryMs_ = now;
    dhcpAutoApplying_ = true;
    appendLog(QStringLiteral("自动网络恢复：") + reason);
    if (!loadSavedLinkMode()) {
        dhcpAutoApplying_ = false;
        return;
    }
    const QString iface = ifaceName_.trimmed().isEmpty() ? QStringLiteral("eth0") : ifaceName_;
    const QJsonObject config{{"iface", iface}, {"dhcp", true}, {"linkMode", linkMode_}};
    const auto generation = networkGeneration_;
    const quint64 expected = generation->load();
    auto error = std::make_shared<QString>();
    runNetworkTask([config, generation, expected, error]() {
        ManagedNetwork::Backend backend({}, [generation, expected]() { return generation->load() == expected; });
        const auto before = backend.query(config.value("iface").toString());
        if (before.available && (before.activating() || before.hasAddress())) return;
        NetworkRuntimeState state;
        backend.apply(config, &state, error.get());
    }, [this, error]() {
        if (!error->isEmpty()) appendLog(*error);
        // 原有定时器下一轮回读有效地址，只在参数实际变化时发出成功通知。
    });
}
