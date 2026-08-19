/**
 * @file auto_connect_manager.h
 * @brief 自动连接管理器。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef AUTOCONNECTMANAGER_H
#define AUTOCONNECTMANAGER_H

#include <QObject>
#include <QString>
#include <QPointer>

class MqttPage;

/**
 * @brief 自动连接管理器
 *
 * 负责：
 *  1. 读取/保存配置文件中的 [auto] 开关
 *  2. 启动时判断是否启用自动模式
 *  3. 自动触发网络 / MQTT / FTP 初始化流程
 *
 * 说明：
 *  - 不替代原有页面按钮功能
 *  - 仅做“自动启动流程”总控
 *  - 通过信号通知网络和 FTP 模块
 *  - 通过绑定的 MqttPage 复用现有 MQTT 连接逻辑
 */
class AutoConnectManager : public QObject
{
    Q_OBJECT

public:
    /** @brief 创建尚未绑定配置路径和 MQTT 页面实例的管理器。 */
    explicit AutoConnectManager(QObject *parent = nullptr);

    /** @brief 设置包含 [auto] 配置的目录和文件名。 */
    void setConfigLocation(const QString &cfgDir, const QString &fileName);

    /** @brief 绑定用于复用现有连接逻辑的 MQTT 页面弱指针。 */
    void bindMqttPage(MqttPage *page);

    /** @brief 若总开关启用，在 delayMs 后执行一次自动启动流程。 */
    void startIfEnabled(int delayMs = 300);

    /** @return [auto] 总开关。 */
    bool isAutoEnabled() const;

    /** @brief 持久化 [auto] 总开关。 */
    bool setAutoEnabled(bool enabled);

    /** @return 网络自动连接子开关。 */
    bool isAutoNetworkEnabled() const;

    /** @return MQTT 自动连接子开关。 */
    bool isAutoMqttEnabled() const;

    /** @return FTP 自动准备子开关。 */
    bool isAutoFtpEnabled() const;

    /** @brief 持久化网络自动连接子开关。 */
    bool setAutoNetworkEnabled(bool enabled);

    /** @brief 持久化 MQTT 自动连接子开关。 */
    bool setAutoMqttEnabled(bool enabled);

    /** @brief 持久化 FTP 自动准备子开关。 */
    bool setAutoFtpEnabled(bool enabled);

signals:
    /** @brief 请求网络页面按保存配置自动连接。 */
    void requestAutoConnectNetwork();
    /** @brief 请求 FTP 页面预加载自动下载配置。 */
    void requestAutoPrepareFtp();

    /** @brief 输出自动启动流程日志。 */
    void logMessage(const QString &text);

    /** @brief 本轮已触发所有启用的自动子流程。 */
    void autoStartFinished();

private:
    /** @return cfgDir 和 fileName 组合后的配置路径。 */
    QString configFilePath() const;

    /** @return [auto] 中指定布尔键，缺失时返回默认值。 */
    bool readBoolValue(const QString &key, bool defaultValue) const;

    /** @brief 写入 [auto] 布尔键并同步文件。 */
    bool writeBoolValue(const QString &key, bool value);

    /** @brief 按子开关顺序触发网络、MQTT、FTP 自动流程。 */
    void doAutoStart();

private:
    QString m_cfgDir;
    QString m_fileName;
    QPointer<MqttPage> m_mqttPage;
};

#endif // AUTOCONNECTMANAGER_H
