/**
 * @file netInfo_popup.cpp
 * @brief 显示网络地址、MQTT 路由和 Broker 可达状态的滑出面板。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "netinfo_popup.h"
#include "platform/rk3566_platform.h"

/** @brief 创建信息标签、动画和异步 ping 进程。 */
NetInfoPanel::NetInfoPanel(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);

    m_title = new QLabel("Info", this);
    m_title->setStyleSheet("color:white; font-size:14px; font-weight:500;");

    m_infoLabel = new QLabel(this);
    m_infoLabel->setWordWrap(true);
    m_infoLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_infoLabel->setStyleSheet("color: rgba(255,255,255,220); font-size:12px;");

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(6);
    lay->addWidget(m_title, 0, Qt::AlignLeft);
    lay->addWidget(m_infoLabel, 1);

    m_pingProcess = new QProcess(this);

    connect(m_pingProcess, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        m_hostReachableKnown = true;
        m_hostReachable = (exitStatus == QProcess::NormalExit && exitCode == 0);
        refreshText();
    });

    connect(m_pingProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        m_hostReachableKnown = true;
        m_hostReachable = false;
        refreshText();
    });


    setStyleSheet(R"(
    NetInfoPanel {
        background: rgba(22, 22, 22, 210);
        border: 1px solid rgba(255, 255, 255, 40);
        border-radius: 16px;
    }

    QLabel {
        color: rgba(255, 255, 255, 220);
        font-size: 12px;
        font-weight: 400;
    }
    )");

    applyGeometry();

    // 初始隐藏在屏幕左侧外
    move(-m_panelW, m_panelY);
    hide();
}

/** @brief 根据 1024x768 布局参数计算显示和隐藏位置。 */
void NetInfoPanel::applyGeometry()
{
    m_screenW = 1024;
    m_screenH = 768;

    const QString configPath = Rk3566Platform::uiConfigPath();

    if (QFile::exists(configPath)) {
        QSettings settings(configPath, QSettings::IniFormat);

        bool okW = false;
        bool okH = false;

        settings.beginGroup("backRect");
        int w = settings.value("backPic_x_Size", 1024).toInt(&okW);
        int h = settings.value("backPic_y_Size", 768).toInt(&okH);

        if (okW && w > 0) m_screenW = w;
        if (okH && h > 0) m_screenH = h;
    }

    // 改成横版、两行显示
    m_panelW = 720;
    m_panelH = 96;

    setFixedSize(m_panelW, m_panelH);

    // 左上角留一点边距
    m_panelX = 12;
    m_panelY = 12;
}

/** @brief 更新面板展示的 IP、客户端、路由、主机、主题和 URL。 */
void NetInfoPanel::setInfo(const QString &ip,
                           const QString &clientId,
                           const QString &routeName,
                           const QString &host,
                           const QString &subTopics,
                           const QString &url)
{
    m_ip = ip;
    m_clientId = clientId;
    m_routeName = routeName.trimmed();
    m_host = host;
    m_subTopics = subTopics;
    m_url = url;

    m_hostReachableKnown = false;
    m_hostReachable = false;

    refreshText();
    checkHostReachableAsync();
}

/** @brief 按当前字段、状态标记和可达性刷新富文本。 */
void NetInfoPanel::refreshText()
{
    const QString ip        = m_ip.isEmpty() ? "N/A" : m_ip;
    const QString clientId  = m_clientId.isEmpty() ? "N/A" : m_clientId;
    const QString routeName = m_routeName.isEmpty() ? QStringLiteral("mqtt") : m_routeName;
    const QString host      = m_host.isEmpty() ? "N/A" : m_host;
    const QString subTopics = m_subTopics.isEmpty() ? "N/A" : m_subTopics;
    const QString url       = m_url.isEmpty() ? "N/A" : m_url;

    QString mqttMarkText;
    switch (m_mqttMark) {
    case MqttConnMark::MqttOk:
        mqttMarkText = " <img src=':/static/styles/connect_ok.png' width='16' height='16'/>";
        break;
    case MqttConnMark::MqttError:
        mqttMarkText = " <img src=':/static/styles/mqtt_error.png' width='16' height='16'/>";
        break;
    case MqttConnMark::IpcError:
        mqttMarkText = " <img src=':/static/styles/ipc_error.png' width='16' height='16'/>";
        break;
    case MqttConnMark::Unknown:
    default:
        break;
    }

    QString hostMarkText;
    if (m_hostReachableKnown && m_hostReachable) {
        hostMarkText = " <img src=':/static/styles/connect_ok.png' width='16' height='16'/>";
    }else{
        hostMarkText = " <img src=':/static/styles/net_error.png' width='16' height='16'/>";
    }


    // 两横排
    const QString text = QString(
            "IP: %1    Client ID: %2    Host[%3]: %4%5<br/>"
            "Sub Topics[%3]: %6%7    URL: %8")
            .arg(ip)
            .arg(clientId)
            .arg(routeName)
            .arg(host)
            .arg(hostMarkText)
            .arg(subTopics)
            .arg(mqttMarkText)
            .arg(url);

    m_infoLabel->setTextFormat(Qt::RichText);
    m_infoLabel->setText(text);
}

/** @brief 从屏幕外滑入面板并发起主机可达性检查。 */
void NetInfoPanel::showSlideIn()
{
    if (m_shown) return;
    m_shown = true;

    applyGeometry();
    show();
    raise();

    auto *anim = new QPropertyAnimation(this, "pos");
    anim->setDuration(180);
    anim->setStartValue(QPoint(-width(), m_panelY));
    anim->setEndValue(QPoint(m_panelX, m_panelY));
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

/** @brief 将面板滑出屏幕并隐藏。 */
void NetInfoPanel::hideSlideOut()
{
    if (!m_shown) return;
    m_shown = false;

    auto *anim = new QPropertyAnimation(this, "pos");
    anim->setDuration(160);
    anim->setStartValue(pos());
    anim->setEndValue(QPoint(-width(), m_panelY));
    anim->setEasingCurve(QEasingCurve::InCubic);
    connect(anim, &QPropertyAnimation::finished, this, [this](){
        hide();
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

/** @brief 点击面板时阻止事件穿透到下层业务控件。 */
void NetInfoPanel::mousePressEvent(QMouseEvent *e)
{
    e->accept();
}


/** @brief 更新 MQTT/IPC 状态标记并重绘文本。 */
void NetInfoPanel::setMqttConnMark(MqttConnMark mark)
{
    m_mqttMark = mark;
    refreshText();
}


/** @brief 使用 QProcess 异步 ping 当前主机。 */
void NetInfoPanel::checkHostReachableAsync()
{
    const QString host = m_host.trimmed();

    if (host.isEmpty() || host == "N/A") {
        m_hostReachableKnown = false;
        m_hostReachable = false;
        refreshText();
        return;
    }

    if (!m_pingProcess) {
        return;
    }

    if (m_pingProcess->state() != QProcess::NotRunning) {
        m_pingProcess->kill();
        m_pingProcess->waitForFinished(200);
    }

    // -c 1 发一个包，-W 1 等待1秒
    m_pingProcess->start("ping", QStringList() << "-c" << "1" << "-W" << "1" << host);
}
