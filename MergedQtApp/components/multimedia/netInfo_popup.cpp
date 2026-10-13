/**
 * @file netInfo_popup.cpp
 * @brief 显示网络地址、MQTT 路由和 Broker 可达状态的滑出面板。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "netinfo_popup.h"
#include "platform/rk3566_platform.h"

#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QTimer>

namespace {
constexpr int kFadeDurationMs = 1800;
}

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
        const QString completedHost = m_pingHostInFlight;
        m_pingHostInFlight.clear();

        // 只把结果应用给当前正在展示的主机，避免旧请求覆盖新配置。
        if (completedHost == m_host.trimmed()) {
            m_hostReachableKnown = true;
            m_hostReachable = (exitStatus == QProcess::NormalExit && exitCode == 0);
            refreshText();
        }

        if (!m_pendingPingHost.isEmpty()) {
            const QString nextHost = m_pendingPingHost;
            m_pendingPingHost.clear();
            QTimer::singleShot(0, this, [this, nextHost]() {
                startHostReachabilityCheck(nextHost);
            });
        }
    });

    connect(m_pingProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        // FailedToStart 不一定伴随 finished 信号，需要在这里收口；其余错误
        // 仍交给 finished 处理，避免一次请求被重复结算。
        if (error != QProcess::FailedToStart) {
            return;
        }

        const QString failedHost = m_pingHostInFlight;
        m_pingHostInFlight.clear();
        if (failedHost == m_host.trimmed()) {
            m_hostReachableKnown = true;
            m_hostReachable = false;
            refreshText();
        }

        if (!m_pendingPingHost.isEmpty()) {
            const QString nextHost = m_pendingPingHost;
            m_pendingPingHost.clear();
            QTimer::singleShot(0, this, [this, nextHost]() {
                startHostReachabilityCheck(nextHost);
            });
        }
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

    // 面板位置和尺寸只在构造时从配置读取一次，显示时不再访问磁盘。
    move(m_panelX, m_panelY);

    m_opacityEffect = new QGraphicsOpacityEffect(this);
    m_opacityEffect->setOpacity(0.0);
    setGraphicsEffect(m_opacityEffect);

    m_opacityAnimation = new QPropertyAnimation(m_opacityEffect, "opacity", this);
    m_opacityAnimation->setDuration(kFadeDurationMs);
    m_opacityAnimation->setEasingCurve(QEasingCurve::InOutCubic);
    connect(m_opacityAnimation, &QPropertyAnimation::finished, this, [this]() {
        if (!m_shown) {
            hide();
        }
    });
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

/** @brief 在固定位置淡入面板。 */
void NetInfoPanel::showSlideIn()
{
    if (m_shown) return;
    m_shown = true;

    move(m_panelX, m_panelY);
    show();
    raise();

    m_opacityAnimation->stop();
    m_opacityAnimation->setStartValue(m_opacityEffect->opacity());
    m_opacityAnimation->setEndValue(1.0);
    m_opacityAnimation->start();
}

/** @brief 在固定位置淡出并隐藏面板。 */
void NetInfoPanel::hideSlideOut()
{
    if (!m_shown) return;
    m_shown = false;

    m_opacityAnimation->stop();
    m_opacityAnimation->setStartValue(m_opacityEffect->opacity());
    m_opacityAnimation->setEndValue(0.0);
    m_opacityAnimation->start();
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
        m_pendingPingHost.clear();
        m_hostReachableKnown = false;
        m_hostReachable = false;
        refreshText();
        return;
    }

    if (!m_pingProcess) {
        return;
    }

    if (m_pingProcess->state() != QProcess::NotRunning) {
        // 不在 GUI 线程等待旧进程退出。同一主机沿用当前请求；主机发生
        // 变化时只缓存最新请求，待当前进程异步结束后再启动。
        if (m_pingHostInFlight == host) {
            m_pendingPingHost.clear();
        } else {
            m_pendingPingHost = host;
        }
        return;
    }

    m_pendingPingHost.clear();
    startHostReachabilityCheck(host);
}

/** @brief 启动一次非阻塞的主机可达性检查。 */
void NetInfoPanel::startHostReachabilityCheck(const QString &host)
{
    if (!m_pingProcess || host.isEmpty()) {
        return;
    }
    if (m_pingProcess->state() != QProcess::NotRunning) {
        m_pendingPingHost = host;
        return;
    }

    m_pingHostInFlight = host;
    // -c 1 发一个包，-W 1 等待1秒
    m_pingProcess->start("ping", QStringList() << "-c" << "1" << "-W" << "1" << host);
}
