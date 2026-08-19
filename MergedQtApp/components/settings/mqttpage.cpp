/**
 * @file mqttpage.cpp
 * @brief MQTT/IPC 配置、路由选择、连接操作和运行日志页面的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "mqttpage.h"

#include <QDebug>
#include <QMessageBox>
#include <QGroupBox>
#include <QGridLayout>
#include <QDateTime>
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QScreen>
#include <QtMath>
#include <QScrollArea>
#include <QScrollBar>
#include <QAbstractScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSettings>
#include <QFile>
#include <QDir>
#include <QFileInfo>

#include "appstyle.h"
#include "platform/rk3566_platform.h"


/* ==================== SHA-256哈希法生成唯一 client_id / online_v1 / online_v2 topic ==================== */

// 把字符串做 SHA-256，并输出十六进制字符串
static QString sha256Hex(const QString &s)
{
    const QByteArray digest = QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(digest.toHex());
}

// RK3566 优先读取设备树 serial-number，缺失时使用 machine-id/MAC。
static QString readSunxiSerial()
{
    return Rk3566Platform::deviceSerial();
}

/** @brief 从芯片序列号派生稳定且不暴露原值的 MQTT 客户端 ID。 */
static QString buildClientIdFromSunxiSerial()
{
    QString serial = readSunxiSerial();
    if (serial.isEmpty()) {
        serial = QStringLiteral("rk3566-fallback-device");
    }

    const QString salt = QStringLiteral("YC");
    const QString raw = QStringLiteral("rk3566:") + serial;
    const QString h = sha256Hex(raw + QStringLiteral("|") + salt).left(10).toUpper();
    return QStringLiteral("YCEEQZ") + h;
}

/** @return 第一版在线协议的设备订阅主题。 */
static QString buildOnlineV1SubTopic(const QString &clientId)
{
    return QString("device/ycLinux/%1/request").arg(clientId.trimmed());
}

/** @return 第一版在线协议的设备发布主题。 */
static QString buildOnlineV1PubTopic(const QString &clientId)
{
    return QString("device/ycLinux/%1/event").arg(clientId.trimmed());
}

/** @return 第二版在线协议的旧响应主题。 */
static QString buildOnlineV2SubTopic(const QString &clientId)
{
    return QString("device/yc/%1/responses").arg(clientId.trimmed());
}

/** @return 第二版在线协议的设备发布主题。 */
static QString buildOnlineV2PubTopic(const QString &clientId)
{
    return QString("device/yc/%1/event").arg(clientId.trimmed());
}

/** @brief 基于芯片序列号生成稳定客户端 ID，并补齐两套在线协议主题。 */
static bool prepareFixedClientIdAndTopics(const QString &cfgPath, QString *clientIdOut, QString *v1SubOut,
                                            QString *v1PubOut, QString *v2SubOut, QString *v2PubOut, QString *errOut = nullptr)
{
    if (errOut) errOut->clear();

    const QString clientId = buildClientIdFromSunxiSerial();
    if (clientId.trimmed().isEmpty()) {
        if (errOut) *errOut = "生成 client_id 失败";
        return false;
    }

    const QString v1Sub = buildOnlineV1SubTopic(clientId);
    const QString v1Pub = buildOnlineV1PubTopic(clientId);
    const QString v2Sub = buildOnlineV2SubTopic(clientId);
    const QString v2Pub = buildOnlineV2PubTopic(clientId);

    QFileInfo fi(cfgPath);
    QDir dir = fi.dir();
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            if (errOut) *errOut = QString("无法创建目录: %1").arg(dir.absolutePath());
            return false;
        }
    }

    QSettings ini(cfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    ini.beginGroup("mqtt");
    ini.setValue("client_id", clientId);
    ini.setValue("sub_topics_online_v1", v1Sub);
    ini.setValue("publish_topic_online_v1", v1Pub);
    ini.setValue("sub_topics_online_v2", v2Sub);
    ini.setValue("publish_topic_online_v2", v2Pub);
    //ini.remove("stream_url_topic_online_v1");
    ini.endGroup();

    ini.sync();
    if (ini.status() != QSettings::NoError) {
        if (errOut) *errOut = QString("写入 MQTT 固定 topic 失败: %1").arg(cfgPath);
        return false;
    }

    if (clientIdOut) *clientIdOut = clientId;
    if (v1SubOut) *v1SubOut = v1Sub;
    if (v1PubOut) *v1PubOut = v1Pub;
    if (v2SubOut) *v2SubOut = v2Sub;
    if (v2PubOut) *v2PubOut = v2Pub;
    return true;
}


/** @brief 根据互斥功能开关读取唯一活动的 MQTT 路由名。 */
static bool readActiveRouteNameFromFeature(const QString &cfgPath, QString *routeNameOut, QString *errOut = nullptr)
{
    if (routeNameOut) routeNameOut->clear();
    if (errOut) errOut->clear();

    QSettings ini(cfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    const bool offline_v1 = ini.value("feature/offline_v1", false).toBool();
    const bool online_v1  = ini.value("feature/online_v1", false).toBool();
    const bool online_v2  = ini.value("feature/online_v2", false).toBool();

    const int enabledCount =
            (offline_v1 ? 1 : 0) +
            (online_v1 ? 1 : 0) +
            (online_v2 ? 1 : 0);

    if (enabledCount == 0) {
        if (errOut) *errOut = "离线和网络均未启用。";
        return false;
    }

    if (enabledCount > 1) {
        if (errOut) *errOut = "离线和网络模式只能启用一个。";
        return false;
    }

    if (offline_v1) {
        if (errOut) *errOut = "当前启用的是离线模式, 不建立 MQTT 连接。";
        return false;
    }

    if (online_v1) {
        if (routeNameOut) *routeNameOut = "online_v1(ycLinux)";
        return true;
    }

    if (online_v2) {
        if (routeNameOut) *routeNameOut = "online_v2(yc)";
        return true;
    }

    if (errOut) *errOut = "无法识别当前激活的 feature 路由。";
    return false;
}


/**
 * @brief 构造 MQTT 页面并初始化 manager 连接。
 */
MqttPage::MqttPage(QWidget *parent) : QWidget(parent)
{
    setupUI();

    manager_ = new MqttManager(this);

    connect(manager_, &MqttManager::logMessage,
            this, &MqttPage::appendLog);

    connect(manager_, &MqttManager::ipcStateChanged,
            this, [this](bool ok) {
        updateIpcUiState(ok);
    });

    connect(manager_, &MqttManager::mqttStateTextChanged,
            this, [this](const QString &text) {
        if (mqttStatusEdit) mqttStatusEdit->setText(text);
    });

    connect(manager_, &MqttManager::subTopicsTextChanged,
            this, [this](const QString &text) {
        if (subTopicsEdit) subTopicsEdit->setText(text);
    });

    connect(manager_, &MqttManager::mqttConfigFromStatus,
            this, [this](const QJsonObject &mqtt) {
        const QString host = mqtt.value("host").toString();
        const int port = mqtt.value("port").toInt();
        const QString clientId = mqtt.value("client_id").toString();
        const int qos = mqtt.value("qos").toInt(0);
        const bool tls = mqtt.value("tls").toBool(false);

        auto safeSetText = [](QLineEdit* edit, const QString& v){
            if (!edit) return;
            if (!edit->hasFocus()) edit->setText(v);
        };

        auto safeSetSpin = [](QSpinBox* sp, int v){
            if (!sp) return;
            if (!sp->hasFocus()) sp->setValue(v);
        };

        //if (!host.isEmpty()) safeSetText(brokerAddressEdit, host);
        if (port > 0) safeSetSpin(portSpinBox, port);
        //if (!clientId.isEmpty()) safeSetText(clientIdEdit, clientId);

        if (qosComboBox && !qosComboBox->hasFocus()) {
            int safeQos = qos;
            if (safeQos < 0 || safeQos > 2) safeQos = 0;
            qosComboBox->setCurrentIndex(safeQos);
        }

        if (sslCheckBox && !sslCheckBox->hasFocus()) {
            sslCheckBox->setChecked(tls);
        }
    });

    connect(manager_, &MqttManager::streamUrlReceived,
            this, &MqttPage::streamUrlReceived);
    connect(manager_, &MqttManager::mqttVolumeReceived,
            this, &MqttPage::mqttVolumeReceived);
    connect(manager_, &MqttManager::stopLiveRequested,
            this, &MqttPage::stopLiveRequested);
    connect(manager_, &MqttManager::liveStreamReady,
            this, &MqttPage::liveStreamReady);
    connect(manager_, &MqttManager::ftpDownloadTaskReady,
            this, &MqttPage::ftpDownloadTaskReady);

    connect(manager_, &MqttManager::getPlayInfoRequested,
            this, &MqttPage::onGetPlayInfoRequested);
    connect(manager_, &MqttManager::stopAllPlayRequested,
            this, &MqttPage::stopAllPlayRequested);
    connect(manager_, &MqttManager::mqttConnMarkChanged,
            this, &MqttPage::mqttConnMarkChanged);
    connect(manager_, &MqttManager::videoControlNotice,
            this, &MqttPage::videoControlNotice);

    updateIpcUiState(false);

//    QTimer::singleShot(1000, this, [this]() {
//        startAutoConnect();
//    });
}

/** @brief 断开连接并释放页面资源。 */
MqttPage::~MqttPage()
{
}


// 网络系统路由决策
MqttPage::FeatureRouteDecision MqttPage::decideRouteByFeature(const QString &cfgPath, const QString &clientId) const
{
    FeatureRouteDecision d;

    QSettings ini(cfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    const bool offline_v1 = ini.value("feature/offline_v1", false).toBool();
    d.online_v1 = ini.value("feature/online_v1", false).toBool();
    d.online_v2 = ini.value("feature/online_v2", false).toBool();

    const int enabledCount =
            (offline_v1 ? 1 : 0) +
            (d.online_v1 ? 1 : 0) +
            (d.online_v2 ? 1 : 0);

    if (enabledCount == 0) {
        d.error = "离线和网络均未启用。";
        return d;
    }

    if (enabledCount > 1) {
        d.error = "离线和网络模式只能启用一个。";
        return d;
    }

    if (offline_v1) {
        d.error = "当前启用的是离线模式, 不建立 MQTT 连接。";
        return d;
    }

    const QString cid = clientId.trimmed();
    if (cid.isEmpty()) {
        d.error = "client_id 为空。";
        return d;
    }

    d.deviceName = cid;

    QString protocolMode = ini.value("mqtt/protocol_mode", "online").toString().trimmed().toLower();
    if (protocolMode == "ic") {
        protocolMode = "offline";
    } else if (protocolMode == "legacy") {
        protocolMode = "online";
    }
    if (protocolMode != "offline" && protocolMode != "online") {
        protocolMode = "offline";
    }

    auto splitTopics = [&](const QString &topicText) {
        QString t = topicText;
        t.replace(',', ';');

        QStringList out;
        const QStringList parts = t.split(';', QString::SkipEmptyParts);
        for (QString p : parts) {
            p = p.trimmed();
            if (!p.isEmpty()) out << p;
        }
        out.removeDuplicates();
        return out;
    };

    if (d.online_v1) {
        const QString hostF2 = ini.value("mqtt/host_online_v1").toString().trimmed();
        const QString topicsTextF2 = ini.value("mqtt/sub_topics_online_v1").toString().trimmed();
        const QString publishTopicF2 = ini.value("mqtt/publish_topic_online_v1").toString().trimmed();

        if (hostF2.isEmpty()) {
            d.error = "online_v1 已启用，但 mqtt/host_online_v1 未配置。";
            return d;
        }
        if (topicsTextF2.isEmpty()) {
            d.error = "online_v1 已启用，但 mqtt/sub_topics_online_v1 未配置。";
            return d;
        }
        if (publishTopicF2.isEmpty()) {
            d.error = "online_v1 已启用，但 mqtt/publish_topic_online_v1 未配置。";
            return d;
        }

        QStringList subTopicsF2 = splitTopics(topicsTextF2);
        if (subTopicsF2.isEmpty()) {
            d.error = "online_v1 的 mqtt/sub_topics_online_v1 解析后为空。";
            return d;
        }

        const QString userF2 = ini.value("mqtt/username_online_v1").toString().trimmed();
        const QString passF2 = ini.value("mqtt/password_online_v1").toString();

        d.ok = true;
        d.routeName = "online_v1(ycLinux)";
        d.host = hostF2;
        d.protocolMode = protocolMode;
        d.subscribeTopic = subTopicsF2.first();
        d.publishTopic = publishTopicF2;
        d.username = userF2;
        d.password = passF2;
        d.subTopics = subTopicsF2;
        d.streamUrlTopic.clear();
        d.topicText = d.subTopics.join(" ; ");
        return d;
    }

    const QString hostF3 = ini.value("mqtt/host_online_v2").toString().trimmed();
    const QString topicsTextF3 = ini.value("mqtt/sub_topics_online_v2").toString().trimmed();
    const QString pubTopicF3 = ini.value("mqtt/publish_topic_online_v2").toString().trimmed();
    if (hostF3.isEmpty()) {
        d.error = "online_v2 已启用，但 mqtt/host_online_v2 未配置。";
        return d;
    }
    if (topicsTextF3.isEmpty()) {
        d.error = "online_v2 已启用，但 mqtt/sub_topics_online_v2 未配置。";
        return d;
    }
    if (pubTopicF3.isEmpty()) {
        d.error = "online_v2 已启用，但 mqtt/publish_topic_online_v2 未配置。";
        return d;
    }


    QStringList subTopicsF3 = splitTopics(topicsTextF3);
    if (subTopicsF3.isEmpty()) {
        d.error = "online_v2 的 mqtt/sub_topics_online_v2 解析后为空。";
        return d;
    }

    const QString userF3 = ini.value("mqtt/username_online_v2").toString().trimmed();
    const QString passF3 = ini.value("mqtt/password_online_v2").toString();

    d.ok = true;
    d.routeName = "online_v2(yc)";
    d.host = hostF3;
    d.protocolMode = protocolMode;
    d.subscribeTopic = subTopicsF3.first();   // 内部可用，但不写回配置
    d.publishTopic = pubTopicF3;
    d.username = userF3;
    d.password = passF3;
    d.subTopics = subTopicsF3;
    d.streamUrlTopic.clear();                 // online_v2 不使用它
    d.topicText = d.subTopics.join(" ; ");
    return d;
}


/** @brief 将当前输入保存到被选中的 v1 或 v2 配置键。 */
bool MqttPage::saveUiInputsToActiveRouteConfig(const QString &cfgPath, const QString &clientId, const QString &v1Sub,
                                                const QString &v1Pub, const QString &v2Sub, const QString &v2Pub, QString *error)
{
    if (error) *error = QString();

    QSettings ini(cfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    const bool offline_v1 = ini.value("feature/offline_v1", false).toBool();
    const bool online_v1 = ini.value("feature/online_v1", false).toBool();
    const bool online_v2 = ini.value("feature/online_v2", false).toBool();

    const int enabledCount =
            (offline_v1 ? 1 : 0) +
            (online_v1 ? 1 : 0) +
            (online_v2 ? 1 : 0);

    if (enabledCount == 0) {
        if (error) *error = "离线和网络均未启用。";
        return false;
    }

    if (enabledCount > 1) {
        if (error) *error = "离线和网络模式只能启用一个。";
        return false;
    }

    if (offline_v1) {
        if (error) *error = "当前启用的是离线模式, 不建立 MQTT 连接。";
        return false;
    }

    const QString host = brokerAddressEdit ? brokerAddressEdit->text().trimmed() : QString();
    const QString user = usernameEdit ? usernameEdit->text().trimmed() : QString();
    const QString pass = passwordEdit ? passwordEdit->text() : QString();

    if (host.isEmpty()) {
        if (error) *error = "Broker 地址为空。";
        return false;
    }

    QString protocolMode = ini.value("mqtt/protocol_mode", "online").toString().trimmed().toLower();
    if (protocolMode == "ic") {
        protocolMode = "offline";
    } else if (protocolMode == "legacy") {
        protocolMode = "online";
    }
    if (protocolMode != "offline" && protocolMode != "online") {
        protocolMode = "offline";
    }

    ini.beginGroup("mqtt");

    // 公共项
    ini.setValue("use_config", true);
    ini.setValue("ipc_path", ipcPathEdit ? ipcPathEdit->text().trimmed() : QString());
    ini.setValue("port", portSpinBox ? portSpinBox->value() : 1883);
    ini.setValue("client_id", clientId.trimmed());
    ini.setValue("keepalive", keepaliveSpinBox ? keepaliveSpinBox->value() : 30);
    ini.setValue("qos", qosComboBox ? qosComboBox->currentText().toInt() : 0);
    ini.setValue("tls", sslCheckBox ? sslCheckBox->isChecked() : false);
    ini.setValue("protocol_mode", protocolMode);

    // 只写当前激活分支
    if (online_v1) {
        ini.setValue("host_online_v1", host);
        ini.setValue("sub_topics_online_v1", v1Sub);
        ini.setValue("publish_topic_online_v1", v1Pub);
        ini.setValue("username_online_v1", user);
        ini.setValue("password_online_v1", pass);
    } else if (online_v2) {
        ini.setValue("host_online_v2", host);
        ini.setValue("sub_topics_online_v2", v2Sub);
        ini.setValue("publish_topic_online_v2", v2Pub);
        ini.setValue("username_online_v2", user);
        ini.setValue("password_online_v2", pass);
    }

    ini.endGroup();
    ini.sync();

    if (ini.status() != QSettings::NoError) {
        if (error) *error = QString("写入配置失败：%1").arg(cfgPath);
        return false;
    }

    return true;
}


/** @brief 把路由决定回显到表单并同步 MqttManager 配置。 */
void MqttPage::applyRouteToUi(const FeatureRouteDecision &decision)
{
    auto safeSetText = [](QLineEdit* edit, const QString& v){
        if (!edit) return;
        if (!edit->hasFocus()) edit->setText(v);
    };

    safeSetText(brokerAddressEdit, decision.host);
    safeSetText(topicsInputEdit, decision.topicText);
    safeSetText(usernameEdit, decision.username);
    safeSetText(passwordEdit, decision.password);
    safeSetText(publishTopicEdit, decision.publishTopic);

    streamUrlTopic_ = decision.streamUrlTopic;
    currentUiRouteName_ = decision.routeName;
    routeInputsDirty_ = false;
}


/** @brief 校验、保存、应用路由并连接 mqttd。 */
bool MqttPage::routeConnectAndApply(bool autoMode)
{
    if (!manager_) return false;

    const QString cfgPath = QDir(configDir_).filePath(configFileName_);
    if (autoMode && !QFileInfo::exists(cfgPath)) {
        appendLog(QString("自动模式：MQTT 配置文件不存在 %1").arg(cfgPath));
        return false;
    }

    QString clientId;
    QString v1Sub;
    QString v1Pub;
    QString v2Sub;
    QString v2Pub;
    QString genErr;
    if (!prepareFixedClientIdAndTopics(cfgPath, &clientId, &v1Sub, &v1Pub, &v2Sub, &v2Pub, &genErr)) {
        appendLog(QString("固定生成 client_id/topic 失败：%1").arg(genErr));
        return false;
    }

    if (clientIdEdit) {
        clientIdEdit->setText(clientId);
    }

    QString activeRouteName;
    QString activeRouteErr;
    if (!readActiveRouteNameFromFeature(cfgPath, &activeRouteName, &activeRouteErr)) {
        appendLog(QString("%1%2")
                  .arg(autoMode ? "自动连接取消：" : "连接取消：")
                  .arg(activeRouteErr));
        return false;
    }

    // 手动模式：把编辑框值写回当前分支。
    if (!autoMode) {
        const bool uiRouteMismatch =
                (!currentUiRouteName_.isEmpty() && currentUiRouteName_ != activeRouteName);

        // 只要 feature 已切换，就先强制把 UI 刷成当前路由配置，不允许直接拿旧编辑框去覆盖新分支
        if (uiRouteMismatch) {
            appendLog(QString("检测到当前 feature=%1，但页面仍显示 %2 的内容；已重新加载当前路由配置，请确认后再次点击连接")
                      .arg(activeRouteName, currentUiRouteName_));

            loadInputsFromConfigFile(configDir_);
            return false;
        }

        QString saveError;
        if (!saveUiInputsToActiveRouteConfig(cfgPath, clientId, v1Sub, v1Pub, v2Sub, v2Pub, &saveError)) {
            appendLog(QString("写入配置失败：%1").arg(saveError));
            return false;
        }
    }

    // 真正连接前，一律重新从配置文件读取
    const FeatureRouteDecision decision = decideRouteByFeature(cfgPath, clientId);
    if (!decision.ok) {
        appendLog(QString("%1%2")
                  .arg(autoMode ? "自动连接取消：" : "连接取消：")
                  .arg(decision.error));
        return false;
    }

    applyRouteToUi(decision);

    MqttConfig cfg;
    cfg.ipcPath = ipcPathEdit ? ipcPathEdit->text().trimmed() : QString();
    cfg.host = decision.host;
    cfg.port = portSpinBox ? portSpinBox->value() : 1883;
    cfg.clientId = clientId;
    cfg.username = decision.username;
    cfg.password = decision.password;
    cfg.tls = sslCheckBox ? sslCheckBox->isChecked() : false;
    cfg.keepalive = keepaliveSpinBox ? keepaliveSpinBox->value() : 30;
    cfg.qos = qosComboBox ? qosComboBox->currentText().toInt() : 0;
    cfg.protocolMode = decision.protocolMode;
    cfg.deviceName = decision.deviceName;
    cfg.publishTopic = decision.publishTopic;
    cfg.subscribeTopic = decision.subscribeTopic;
    cfg.subTopics = decision.subTopics;
    cfg.streamUrlTopic = decision.streamUrlTopic;

    appendLog(QString("路由=%1 host=%2 topics=%3 publish=%4")
              .arg(decision.routeName,
                   decision.host,
                   decision.topicText,
                   decision.publishTopic));

    manager_->setConfig(cfg);
    manager_->connectAndApply();
    return true;
}

/**
 * @brief 构建 MQTT 参数、日志与操作按钮界面。
 */
void MqttPage::setupUI()
{
    AppStyle::applyTo(this);

    const double s = AppStyle::uiScaleForScreen();
    auto px = [&](double v) { return int(qRound(v * s)); };

    const int mainMargin  = px(16);
    const int mainSpacing = px(12);

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0,0,0,0);
    root->setSpacing(0);

    QScrollArea *scroll = AppStyle::createScrollArea(this);
    scroll->setWidgetResizable(true);
    root->addWidget(scroll);

    QWidget *content = new QWidget(scroll);
    scroll->setWidget(content);

    QVBoxLayout *mainLayout = new QVBoxLayout(content);
    mainLayout->setContentsMargins(mainMargin, mainMargin, mainMargin, mainMargin);
    mainLayout->setSpacing(mainSpacing);

    titleLabel = new QLabel("MQTT连接设置", this);
    titleLabel->setStyleSheet(QString("font-size:%1px; font-weight:bold; margin-bottom:%2px;")
                              .arg(px(20))
                              .arg(px(12)));
    mainLayout->addWidget(titleLabel);

    // ===== mqttd IPC 区 =====
    QGroupBox *ipcGroup = new QGroupBox("MQTTd 通信（本地 IPC）", this);
    QGridLayout *ipcLayout = new QGridLayout(ipcGroup);

    ipcLayout->setVerticalSpacing(px(8));
    ipcLayout->setHorizontalSpacing(px(10));

    ipcLayout->addWidget(new QLabel("Socket 路径:"), 0, 0);
    ipcPathEdit = new QLineEdit("/tmp/mqttd.sock", ipcGroup);
    ipcLayout->addWidget(ipcPathEdit, 0, 1);

    ipcLayout->addWidget(new QLabel("IPC 状态:"), 1, 0);
    ipcStatusEdit = new QLineEdit(ipcGroup);
    ipcStatusEdit->setReadOnly(true);
    ipcLayout->addWidget(ipcStatusEdit, 1, 1);

    ipcLayout->addWidget(new QLabel("MQTT 状态:"), 2, 0);
    mqttStatusEdit = new QLineEdit(ipcGroup);
    mqttStatusEdit->setReadOnly(true);
    ipcLayout->addWidget(mqttStatusEdit, 2, 1);

    ipcLayout->addWidget(new QLabel("订阅主题:"), 3, 0);
    subTopicsEdit = new QLineEdit(ipcGroup);
    subTopicsEdit->setReadOnly(true);
    ipcLayout->addWidget(subTopicsEdit, 3, 1);

    ipcLayout->setColumnStretch(0, 1);
    ipcLayout->setColumnStretch(1, 3);

    mainLayout->addWidget(ipcGroup);
    mainLayout->addSpacing(px(10));

    // ===== MQTT 参数区 =====
    QGroupBox *mqttSettingsGroup = new QGroupBox("连接参数", this);
    QGridLayout *mqttSettingsLayout = new QGridLayout(mqttSettingsGroup);

    mqttSettingsLayout->setVerticalSpacing(px(8));
    mqttSettingsLayout->setHorizontalSpacing(px(10));

    const QString cfgPath = QDir(configDir_).filePath(configFileName_);

    QString cfgClientId;
    QString genV1Sub;
    QString genV1Pub;
    QString genV2Sub;
    QString genV2Pub;
    QString genErr;
    if (!prepareFixedClientIdAndTopics(cfgPath, &cfgClientId, &genV1Sub, &genV1Pub, &genV2Sub, &genV2Pub, &genErr))
        qWarning() << "prepareFixedClientIdAndTopics failed:" << genErr;

    QSettings uiIni(cfgPath, QSettings::IniFormat);
    uiIni.setIniCodec("UTF-8");
    if (cfgClientId.isEmpty()) {
        cfgClientId = uiIni.value("mqtt/client_id").toString().trimmed();
    }

    const QString cfgIpcPath = uiIni.value("mqtt/ipc_path", "/tmp/mqttd.sock").toString().trimmed();
    const int cfgPort = uiIni.value("mqtt/port", 1883).toInt();
    const int cfgKeepalive = uiIni.value("mqtt/keepalive", 30).toInt();
    const int cfgQos = uiIni.value("mqtt/qos", 0).toInt();
    const bool cfgTls = uiIni.value("mqtt/tls", false).toBool();

    FeatureRouteDecision initDecision = decideRouteByFeature(cfgPath, cfgClientId);

    const QString cfgHost = initDecision.ok ? initDecision.host : QString();
    const QString cfgTopics = initDecision.ok ? initDecision.topicText : QString();
    const QString cfgPublishTopic = initDecision.ok ? initDecision.publishTopic : QString();

    const QString cfgUser = initDecision.ok ? initDecision.username : QString();
    const QString cfgPass = initDecision.ok ? initDecision.password : QString();

    if (initDecision.ok) {
        currentUiRouteName_ = initDecision.routeName;
        routeInputsDirty_ = false;
    } else {
        currentUiRouteName_.clear();
        routeInputsDirty_ = false;
    }

    if (ipcPathEdit) {
        ipcPathEdit->setText(cfgIpcPath.isEmpty() ? QString("/tmp/mqttd.sock") : cfgIpcPath);
    }

    mqttSettingsLayout->addWidget(new QLabel("Broker 地址:"), 0, 0);
    brokerAddressEdit = new QLineEdit(cfgHost, mqttSettingsGroup);
    mqttSettingsLayout->addWidget(brokerAddressEdit, 0, 1);

    mqttSettingsLayout->addWidget(new QLabel("端口:"), 1, 0);
    portSpinBox = new QSpinBox(mqttSettingsGroup);
    portSpinBox->setRange(1, 65535);
    portSpinBox->setValue((cfgPort > 0 && cfgPort <= 65535) ? cfgPort : 1883);
    mqttSettingsLayout->addWidget(portSpinBox, 1, 1);

    mqttSettingsLayout->addWidget(new QLabel("客户端 ID:"), 2, 0);
    clientIdEdit = new QLineEdit(cfgClientId, mqttSettingsGroup);
    mqttSettingsLayout->addWidget(clientIdEdit, 2, 1);

    mqttSettingsLayout->addWidget(new QLabel("订阅 Topic:"), 3, 0);
    topicsInputEdit = new QLineEdit(mqttSettingsGroup);
    topicsInputEdit->setText(cfgTopics);
    mqttSettingsLayout->addWidget(topicsInputEdit, 3, 1);

    // 优先读取配置中的 stream_url_topic；为空时回退为 sub_topics 文本。
    mqttSettingsLayout->addWidget(new QLabel("发布 Topic:"), 4, 0);
    publishTopicEdit = new QLineEdit(mqttSettingsGroup);
    publishTopicEdit->setText(cfgPublishTopic);
    mqttSettingsLayout->addWidget(publishTopicEdit, 4, 1);

    streamUrlTopic_ = cfgPublishTopic;

    mqttSettingsLayout->addWidget(new QLabel("心跳(s):"), 5, 0);
    keepaliveSpinBox = new QSpinBox(mqttSettingsGroup);
    keepaliveSpinBox->setRange(5, 3600);
    keepaliveSpinBox->setValue(qBound(5, cfgKeepalive, 3600));
    mqttSettingsLayout->addWidget(keepaliveSpinBox, 5, 1);

    mqttSettingsLayout->addWidget(new QLabel("QoS:"), 6, 0);
    qosComboBox = new QComboBox(mqttSettingsGroup);
    qosComboBox->addItems({"0", "1", "2"});
    qosComboBox->setCurrentIndex((cfgQos >= 0 && cfgQos <= 2) ? cfgQos : 0);
    mqttSettingsLayout->addWidget(qosComboBox, 6, 1);

    mqttSettingsLayout->addWidget(new QLabel("用户名:"), 7, 0);
    usernameEdit = new QLineEdit(cfgUser, mqttSettingsGroup);
    mqttSettingsLayout->addWidget(usernameEdit, 7, 1);

    mqttSettingsLayout->addWidget(new QLabel("密码:"), 8, 0);
    passwordEdit = new QLineEdit(cfgPass, mqttSettingsGroup);
    passwordEdit->setEchoMode(QLineEdit::Password);
    mqttSettingsLayout->addWidget(passwordEdit, 8, 1);

    sslCheckBox = new QCheckBox("使用 SSL/TLS", mqttSettingsGroup);
    sslCheckBox->setChecked(cfgTls);
    mqttSettingsLayout->addWidget(sslCheckBox, 9, 0, 1, 2);

    mqttSettingsLayout->setColumnStretch(0, 1);
    mqttSettingsLayout->setColumnStretch(1, 3);

    mainLayout->addWidget(mqttSettingsGroup);
    mainLayout->addSpacing(15);

    // ===== 日志显示开关 =====
    showLogCheckBox = new QCheckBox("日志", this);
    showLogCheckBox->setChecked(true);
    mainLayout->addWidget(showLogCheckBox);
    mainLayout->addSpacing(px(6));

    // ===== 日志框 =====
    logGroup = new QGroupBox("日志", this);
    logGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    logGroup->setMinimumHeight(px(260));

    QVBoxLayout *logLayout = new QVBoxLayout(logGroup);
    logLayout->setSpacing(px(8));

    rxLogEdit = new QPlainTextEdit(logGroup);
    rxLogEdit->setReadOnly(true);
    rxLogEdit->setMaximumBlockCount(2000);
    rxLogEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    rxLogEdit->setMinimumHeight(px(220));

    logLayout->addWidget(rxLogEdit);

    mainLayout->addWidget(logGroup, 1);
    mainLayout->addSpacing(px(10));

    // ===== 按钮区 =====
    QHBoxLayout *buttonLayout = new QHBoxLayout();

    btnLoadCfg = new QPushButton("读取文件", this);
    btnLoadCfg->setMinimumWidth(100);
    btnLoadCfg->setStyleSheet(
        "QPushButton { background-color: #4285f4; color: white; border-radius: 4px; padding: 8px 16px; } "
        "QPushButton:hover { background-color: #3367d6; } "
    );

    connectButton = new QPushButton("连接", this);
    connectButton->setMinimumWidth(100);
    connectButton->setStyleSheet(
        "QPushButton { background-color: #34a853; color: white; border-radius: 4px; padding: 8px 16px; } "
        "QPushButton:hover { background-color: #2d8f47; } "
    );

    testConnectionButton = new QPushButton("测试连接", this);
    testConnectionButton->setMinimumWidth(100);
    testConnectionButton->setStyleSheet(
        "QPushButton { background-color: #4285f4; color: white; border-radius: 4px; padding: 8px 16px; } "
        "QPushButton:hover { background-color: #3367d6; } "
    );

    disconnectButton = new QPushButton("断开连接", this);
    disconnectButton->setMinimumWidth(100);
    disconnectButton->setEnabled(false);
    disconnectButton->setStyleSheet(
        "QPushButton { background-color: #f1f3f4; border: 1px solid #dadce0; border-radius: 4px; padding: 8px 16px; } "
        "QPushButton:hover { background-color: #e8eaed; } "
    );

    buttonLayout->addStretch();
    buttonLayout->addWidget(btnLoadCfg);
    buttonLayout->addWidget(connectButton);
    buttonLayout->addWidget(testConnectionButton);
    buttonLayout->addWidget(disconnectButton);
    mainLayout->addLayout(buttonLayout);

    connect(showLogCheckBox, &QCheckBox::toggled, this, [=](bool on){
        if (logGroup) logGroup->setVisible(on);
    });

    logGroup->setVisible(showLogCheckBox->isChecked());

    connect(btnLoadCfg, &QPushButton::clicked, this, [this]{
        loadInputsFromConfigFile(configDir_);
    });

    connect(connectButton, &QPushButton::clicked, this, [this]{
        routeConnectAndApply(false);
    });

    connect(disconnectButton, &QPushButton::clicked, this, [this]{
        if (manager_) manager_->disconnectIpc();
    });

    connect(testConnectionButton, &QPushButton::clicked, this, [this]{
        if (manager_) manager_->testConnection();
    });

    // 给可输入控件安装事件过滤器
    ipcPathEdit->installEventFilter(this);
    brokerAddressEdit->installEventFilter(this);
    topicsInputEdit->installEventFilter(this);
    publishTopicEdit->installEventFilter(this);
    clientIdEdit->installEventFilter(this);
    usernameEdit->installEventFilter(this);
    passwordEdit->installEventFilter(this);


    auto markRouteDirty = [this](const QString &) {
        routeInputsDirty_ = true;
    };

    connect(brokerAddressEdit, &QLineEdit::textEdited, this, markRouteDirty);
    connect(usernameEdit, &QLineEdit::textEdited, this, markRouteDirty);
    connect(passwordEdit, &QLineEdit::textEdited, this, markRouteDirty);

    // 修正输入控件高度
    const int fieldMinH = qMax(px(34), px(30));
    auto fixField = [&](QWidget *w){
        if (!w) return;
        const QFontMetrics fm(w->font());
        const int h = qMax(fieldMinH, fm.height() + px(14));
        w->setMinimumHeight(h);
        w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    };

    fixField(ipcPathEdit);
    fixField(ipcStatusEdit);
    fixField(mqttStatusEdit);
    fixField(subTopicsEdit);
    fixField(keepaliveSpinBox);
    fixField(qosComboBox);
    fixField(topicsInputEdit);
    fixField(publishTopicEdit);
    fixField(brokerAddressEdit);
    fixField(portSpinBox);
    fixField(clientIdEdit);
    fixField(usernameEdit);
    fixField(passwordEdit);

    if (auto *portEdit = portSpinBox->findChild<QLineEdit*>()) {
        portEdit->installEventFilter(this);
    }

    if (auto *kaEdit = keepaliveSpinBox->findChild<QLineEdit*>()) {
        kaEdit->installEventFilter(this);
    }

    setLayout(root);
}

/* ===== UI 小工具 ===== */
void MqttPage::appendLog(const QString &text)
{
    const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    const QString line = QString("[%1] %2").arg(ts, text);

    if (rxLogEdit) rxLogEdit->appendPlainText(line);

    emit mqttLog(text);
}

/** @brief 更新 IPC 连接状态控件和可操作按钮。 */
void MqttPage::updateIpcUiState(bool ipcConnected)
{
    if (ipcStatusEdit) {
        ipcStatusEdit->setText(ipcConnected ? "已连接到 mqttd" : "未连接 mqttd");
    }

    if (connectButton) {
        connectButton->setEnabled(true);
        connectButton->setText(ipcConnected ? "重连" : "连接");
    }

    if (disconnectButton) {
        disconnectButton->setEnabled(ipcConnected);
    }

    if (testConnectionButton) {
        testConnectionButton->setEnabled(ipcConnected);
    }

    if (!ipcConnected) {
        if (mqttStatusEdit) mqttStatusEdit->setText("未知");
        if (subTopicsEdit) subTopicsEdit->clear();
    }
}

// 点击输入控件时弹出软键盘
bool MqttPage::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        if (auto *edit = qobject_cast<QLineEdit *>(watched)) {
            if (!edit->isEnabled() || edit->isReadOnly()) {
                return QWidget::eventFilter(watched, event);
            }

            if (edit == ipcPathEdit) {
                openKeyboardFor(edit, "请输入 MQTTd Socket 路径（例如 /tmp/mqttd.sock）",
                                QLineEdit::Normal, 128);
                return true;
            }

            if (edit == brokerAddressEdit) {
                openKeyboardFor(edit, "请输入 Broker 地址（IP/域名）",
                                QLineEdit::Normal, 64);
                return true;
            }

            if (edit == topicsInputEdit) {
                openKeyboardFor(edit, "请输入订阅 Topic 列表（用 ; 分隔）", QLineEdit::Normal, 256);
                return true;
            }

            if (edit == publishTopicEdit) {
                openKeyboardFor(edit, "请输入发布 Topic", QLineEdit::Normal, 256);
                return true;
            }

            if (edit == clientIdEdit) {
                openKeyboardFor(edit, "请输入 ClientId",
                                QLineEdit::Normal, 64);
                return true;
            }

            if (edit == usernameEdit) {
                openKeyboardFor(edit, "请输入用户名",
                                QLineEdit::Normal, 64);
                return true;
            }

            if (edit == passwordEdit) {
                openKeyboardFor(edit, "请输入密码",
                                QLineEdit::Password, 64);
                return true;
            }

            openKeyboardFor(edit, "请输入内容", edit->echoMode(), 128);
            return true;
        }

        if (auto *portEdit = portSpinBox ? portSpinBox->findChild<QLineEdit*>() : nullptr) {
            if (watched == portEdit) {
                openKeyboardForPort(portSpinBox, "请输入端口号（1-65535）", 1, 65535);
                return true;
            }
        }

        if (keepaliveSpinBox) {
            if (auto *kaEdit = keepaliveSpinBox->findChild<QLineEdit*>()) {
                if (watched == kaEdit) {
                    openKeyboardForPort(keepaliveSpinBox, "请输入心跳 Keepalive 秒数（5-3600）", 5, 3600);
                    return true;
                }
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

/** @brief 使用屏幕键盘编辑文本输入。 */
void MqttPage::openKeyboardFor(QLineEdit *edit, const QString &title,
                               QLineEdit::EchoMode echo, int maxLen)
{
    if (!edit) return;

    bool ok = false;

    const QString text = KeyboardDialog::getText(
        this,
        title,
        edit->text(),
        echo,
        maxLen,
        &ok
    );

    if (ok) {
        edit->setText(text.trimmed());
        edit->setFocus();
    }
}

/** @brief 使用屏幕键盘编辑端口等整数输入。 */
void MqttPage::openKeyboardForPort(QSpinBox *spin, const QString &title, int minV, int maxV)
{
    if (!spin) return;

    bool ok = false;

    const QString text = KeyboardDialog::getText(
        this,
        title,
        QString::number(spin->value()),
        QLineEdit::Normal,
        5,
        &ok
    );

    if (!ok) return;

    bool okNum = false;
    int v = text.trimmed().toInt(&okNum);
    if (!okNum) {
        QMessageBox::warning(this, "提示", "端口必须是数字");
        return;
    }
    if (v < minV || v > maxV) {
        QMessageBox::warning(this, "提示", QString("端口范围 %1-%2").arg(minV).arg(maxV));
        return;
    }

    spin->setValue(v);
    spin->setFocus();
}

/** @brief 从配置文件加载当前功能路由对应的 MQTT 输入。 */
bool MqttPage::loadInputsFromConfigFile(const QString& cfgDir)
{
    const QString cfgPath = QDir(cfgDir).filePath(configFileName_);

    if (!QFileInfo::exists(cfgPath)) {
        appendLog(QString("配置读取失败：文件不存在 %1").arg(cfgPath));
        return false;
    }

    QSettings ini(cfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    const bool useCfg = ini.value("mqtt/use_config", false).toBool();
    if (!useCfg) {
        appendLog("配置文件存在，但 mqtt/use_config=false，不覆盖输入框");
        return true;
    }

    QString cid = ini.value("mqtt/client_id").toString().trimmed();
    if (cid.isEmpty() && clientIdEdit) {
        cid = clientIdEdit->text().trimmed();
    }

    const QString ipcPath = ini.value("mqtt/ipc_path", ipcPathEdit ? ipcPathEdit->text() : QString()).toString().trimmed();
    const int port = ini.value("mqtt/port", portSpinBox ? portSpinBox->value() : 1883).toInt();
    const bool tls = ini.value("mqtt/tls", sslCheckBox ? sslCheckBox->isChecked() : false).toBool();
    const int keepalive = ini.value("mqtt/keepalive", keepaliveSpinBox ? keepaliveSpinBox->value() : 30).toInt();
    const int qos = ini.value("mqtt/qos", 0).toInt();
    QString host;
    QString topicsText;
    QString publishText;
    QString user;
    QString pass;


    FeatureRouteDecision decision = decideRouteByFeature(cfgPath, cid);

    if (decision.ok) {
        host = decision.host;
        topicsText = decision.topicText;
        publishText = decision.publishTopic;
        user = decision.username;
        pass = decision.password;
        streamUrlTopic_ = decision.streamUrlTopic;
        currentUiRouteName_ = decision.routeName;
        routeInputsDirty_ = false;
    } else {
        streamUrlTopic_.clear();
        currentUiRouteName_.clear();
        routeInputsDirty_ = false;
    }

    int safePort = port;
    if (safePort <= 0 || safePort > 65535) safePort = 1883;

    auto safeSetText = [](QLineEdit* edit, const QString& v){
        if (!edit) return;
        if (!edit->hasFocus()) edit->setText(v);
    };
    auto safeSetSpin = [](QSpinBox* sp, int v){
        if (!sp) return;
        if (!sp->hasFocus()) sp->setValue(v);
    };

    safeSetText(ipcPathEdit, ipcPath);
    safeSetText(brokerAddressEdit, host);
    safeSetSpin(portSpinBox, safePort);


    if (!cid.isEmpty()) safeSetText(clientIdEdit, cid);
    safeSetText(usernameEdit, user);
    safeSetText(passwordEdit, pass);
    safeSetText(topicsInputEdit, topicsText);
    safeSetText(publishTopicEdit, publishText);

    if (keepaliveSpinBox && !keepaliveSpinBox->hasFocus())
        keepaliveSpinBox->setValue(qBound(5, keepalive, 3600));

    if (qosComboBox && !qosComboBox->hasFocus()) {
        int safeQos = qos;
        if (safeQos < 0 || safeQos > 2) safeQos = 0;
        qosComboBox->setCurrentIndex(safeQos);
    }

    if (sslCheckBox && !sslCheckBox->hasFocus()) sslCheckBox->setChecked(tls);

    appendLog(QString("配置填充成功：%1").arg(cfgPath));
    if (decision.ok) {
        appendLog(QString("feature 路由=%1 host=%2 sub=%3 pub=%4")
                  .arg(decision.routeName,
                       decision.host,
                       decision.topicText,
                       decision.publishTopic));
    } else {
        appendLog(QString("feature 路由未生效：%1").arg(decision.error));
    }
    appendLog(QString("keepalive=%1 qos=%2").arg(keepalive).arg(qos));

    return true;
}


/** @brief 按保存配置自动选择路由并连接。 */
void MqttPage::startAutoConnect()
{
    const QString cfgPath = QDir(configDir_).filePath(configFileName_);
    if (!QFileInfo::exists(cfgPath)) {
        appendLog(QString("自动模式：MQTT配置文件不存在 %1").arg(cfgPath));
        return;
    }

    QSettings ini(cfgPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");

    const bool autoEnable = ini.value("auto/enable", false).toBool();
    const bool autoMqtt = ini.value("auto/auto_mqtt", false).toBool();
    const bool useCfg = ini.value("mqtt/use_config", false).toBool();

    if (!autoEnable) {
        appendLog("自动模式：auto/enable=false，跳过 MQTT 自动连接");
        return;
    }

    if (!autoMqtt) {
        appendLog("自动模式：auto/auto_mqtt=false，跳过 MQTT 自动连接");
        return;
    }

    if (!useCfg) {
        appendLog("自动模式：mqtt/use_config=false，跳过 MQTT 自动连接");
        return;
    }

    appendLog("自动模式：开始读取 MQTT 配置并连接（按 online_v1/online_v2 路由）");
    loadInputsFromConfigFile(configDir_);
    routeConnectAndApply(true);
}

/** @brief 将当前录播文件同步到 MQTT 播放状态。 */
void MqttPage::onCurrentRecordedFileChanged(const QString &fullPath)
{
    if (!manager_) return;
    manager_->setCurrentRecordedFile(fullPath);
}

/** @brief 将最新播放截图同步到 MQTT 播放状态。 */
void MqttPage::onCurrentPlayImageChanged(const QString &base64Image)
{
    if (!manager_) return;

    manager_->updateCurrentPlayImage(base64Image);

    if (pendingPlayInfoReply_) {
        pendingPlayInfoReply_ = false;

        manager_->sendPlayInfoNow(pendingPlayInfoTopic_,
                                  pendingPlayInfoId_,
                                  pendingPlayInfoDeviceId_);

        pendingPlayInfoTopic_.clear();
        pendingPlayInfoId_.clear();
        pendingPlayInfoDeviceId_.clear();
    }
}


/** @brief 收到播放信息请求后触发截图，随后发布响应。 */
void MqttPage::onGetPlayInfoRequested(const QString &topic,
                                      const QString &reqId,
                                      const QString &deviceId)
{
    pendingPlayInfoReply_ = true;
    pendingPlayInfoTopic_ = topic;
    pendingPlayInfoId_ = reqId;
    pendingPlayInfoDeviceId_ = deviceId;

    emit requestCapturePlayImage();
}


/** @brief 同步播放器当前音量到 MQTT 状态。 */
void MqttPage::onCurrentPlayVolumeChanged(int volume)
{
    if (!manager_) return;

    manager_->updateCurrentPlayVolume(volume);
    manager_->notifyVolumeSetFinished(true, QString());
}


/** @brief 发布本地手动音量变更结果。 */
void MqttPage::onLocalManualVolumeChanged(int volume, const QString &reason)
{
    if (!manager_) return;

    manager_->updateCurrentPlayVolume(volume);

    const QString deviceLog = reason.trimmed().isEmpty()
            ? QStringLiteral("本地手动调整音量：%1").arg(volume)
            : reason.trimmed();

    manager_->updateCurrentDeviceLog(deviceLog);

    manager_->publishLocalVideoControlStatus(volume, true, reason);
}


/** @brief 下载完成后结束延迟 videoControl 确认。 */
void MqttPage::onRecordedDownloadFinished(const QString &localFile)
{
    if (!manager_) return;

    manager_->updateCurrentDeviceLog(
        QString("RECORDED 下载成功：%1").arg(QFileInfo(localFile).fileName())
    );

    manager_->notifyRecordedDownloadResult(true, true,
                                           QStringLiteral("downloaded"),
                                           QStringLiteral("FTP download completed"));
}

/** @brief 下载失败后发布带原因的控制确认。 */
void MqttPage::onRecordedDownloadFailed(const QString &name, const QString &reason)
{
    Q_UNUSED(name);

    if (!manager_) return;

    const QString deviceLog = reason.trimmed();

    appendLog(QString("RECORDED 下载失败：%1，准备发送 videoControl 回执(status=error)").arg(reason));
    appendLog(QString("RECORDED 下载失败：%1").arg(deviceLog));

    // 实时更新 sendPlayInfo.info.deviceLog
    manager_->updateCurrentDeviceLog(deviceLog);

    manager_->notifyRecordedDownloadResult(false, false,
                                           QStringLiteral("retryable_error"),
                                           reason);
}


/** @brief 文件已存在时按成功可用结果完成控制确认。 */
void MqttPage::onRecordedDownloadAlreadyExists(const QString &localFile, const QString &reason)
{
    if (!manager_) return;

    const QString deviceLog = reason.trimmed().isEmpty()
            ? QStringLiteral("RECORDED 文件已存在：%1").arg(QFileInfo(localFile).fileName())
            : reason.trimmed();

    appendLog(deviceLog);
    manager_->updateCurrentDeviceLog(deviceLog);
    manager_->notifyRecordedDownloadResult(true, true,
                                           QStringLiteral("already_exists"),
                                           deviceLog);
}


/** @brief 请求参数或任务状态无效时发布拒绝确认。 */
void MqttPage::onRecordedDownloadRequestRejected(const QString &name, const QString &reason)
{
    Q_UNUSED(name);
    if (!manager_) return;

    const QString deviceLog = reason.trimmed().isEmpty()
            ? QStringLiteral("RECORDED 下载请求未通过本地前置检查")
            : reason.trimmed();

    appendLog(QStringLiteral("%1；按终止性结果回执，不请求服务器重发").arg(deviceLog));
    manager_->updateCurrentDeviceLog(deviceLog);
    manager_->notifyRecordedDownloadResult(true, false,
                                           QStringLiteral("local_rejected"),
                                           deviceLog);
}


/** @brief 磁盘空间不足时发布拒绝确认。 */
void MqttPage::onRecordedDownloadStorageRejected(const QString &name, const QString &reason)
{
    Q_UNUSED(name);
    if (!manager_) return;

    const QString deviceLog = reason.trimmed().isEmpty()
            ? QStringLiteral("存储空间不足，拒绝FTP下载")
            : reason.trimmed();

    appendLog(deviceLog);
    manager_->updateCurrentDeviceLog(deviceLog);
    manager_->notifyRecordedDownloadResult(true, false,
                                           QStringLiteral("storage_limited"),
                                           deviceLog);
}
