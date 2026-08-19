/**
 * @file netinfo_popup.h
 * @brief 显示网络地址、MQTT 路由和 Broker 可达状态的滑出面板。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef NETINFOPOPUP_H
#define NETINFOPOPUP_H

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QPropertyAnimation>
#include <QDir>
#include <QFile>
#include <QMouseEvent>
#include <QSettings>
#include <QEasingCurve>
#include <QProcess>


/** @brief MQTT/IPC 连接状态图标。 */
enum class MqttConnMark
{
    Unknown,     /**< 不显示连接标记。 */
    IpcError,    /**< IPC 异常，显示红色感叹号。 */
    MqttOk,      /**< MQTT 正常，显示绿色对勾。 */
    MqttError    /**< MQTT 异常，显示红色叉号。 */
};


/** @brief 从屏幕侧边滑入的网络、MQTT 和主机可达性信息面板。 */
class NetInfoPanel : public QWidget
{
    Q_OBJECT
public:
    /** @brief 创建信息标签、动画和异步 ping 进程。 */
    explicit NetInfoPanel(QWidget *parent = nullptr);

    /** @brief 更新面板展示的 IP、客户端、路由、主机、主题和 URL。 */
    void setInfo(const QString &ip,
                 const QString &clientId,
                 const QString &routeName,
                 const QString &host,
                 const QString &subTopics,
                 const QString &url);

    /** @brief 更新 MQTT/IPC 状态标记并重绘文本。 */
    void setMqttConnMark(MqttConnMark mark);

    /** @brief 从屏幕外滑入面板并发起主机可达性检查。 */
    void showSlideIn();

    /** @brief 将面板滑出屏幕并隐藏。 */
    void hideSlideOut();

    /** @return 面板逻辑上处于显示状态时返回 true。 */
    bool isShown() const { return m_shown; }

protected:
    /** @brief 点击面板时阻止事件穿透到下层业务控件。 */
    void mousePressEvent(QMouseEvent *e) override;

private:
    /** @brief 根据 1024x768 布局参数计算显示和隐藏位置。 */
    void applyGeometry();

    /** @brief 按当前字段、状态标记和可达性刷新富文本。 */
    void refreshText();

    /** @brief 使用 QProcess 异步 ping 当前主机。 */
    void checkHostReachableAsync();

private:
    QLabel *m_title = nullptr;
    QLabel *m_infoLabel = nullptr;

    QString m_ip;
    QString m_clientId;
    QString m_routeName;
    QString m_host;
    QString m_subTopics;
    QString m_url;

    int m_screenW = 1024;
    int m_screenH = 768;
    int m_panelW = 360;
    int m_panelH = 220;
    int m_panelX = 0;
    int m_panelY = 0;

    MqttConnMark m_mqttMark = MqttConnMark::Unknown;

    bool m_hostReachable = false;
    bool m_hostReachableKnown = false;

    QProcess *m_pingProcess = nullptr;

    bool m_shown = false;
};

#endif // NETINFOPOPUP_H
