/**
 * @file auto_connect_manager.cpp
 * @brief 自动连接管理器的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "auto_connect_manager.h"
#include "mqttpage.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>
#include <QMetaObject>


/**
 * @brief 构造一个 AutoConnectManager 对象。
 *
 * 该类用于管理系统启动后的自动连接流程，包括：
 * - 自动网络连接
 * - 自动 MQTT 连接
 * - 自动 FTP 准备
 *
 * @param parent 父对象指针。
 */
AutoConnectManager::AutoConnectManager(QObject *parent)
    : QObject(parent)
{
}


/**
 * @brief 设置自动连接配置文件所在目录及文件名。
 *
 * 后续所有配置项读写操作都会基于该路径进行。
 *
 * @param cfgDir 配置文件所在目录。
 * @param fileName 配置文件名。
 */
void AutoConnectManager::setConfigLocation(const QString &cfgDir, const QString &fileName)
{
    m_cfgDir = cfgDir;
    m_fileName = fileName;
}


/**
 * @brief 绑定 MQTT 页面对象。
 *
 * 自动 MQTT 连接流程会通过该页面复用已有的配置加载逻辑和连接按钮逻辑。
 *
 * @param page MQTT 页面对象指针，可为空。
 */
void AutoConnectManager::bindMqttPage(MqttPage *page)
{
    m_mqttPage = page;
}


/**
 * @brief 获取完整的配置文件路径。
 *
 * 当配置目录或文件名为空时，返回空字符串。
 *
 * @return 配置文件完整路径；若路径信息不完整则返回空字符串。
 */
QString AutoConnectManager::configFilePath() const
{
    if (m_cfgDir.isEmpty() || m_fileName.isEmpty()) {
        return QString();
    }
    return QDir(m_cfgDir).filePath(m_fileName);
}


/**
 * @brief 从配置文件中读取布尔值。
 *
 * 若配置文件路径无效或配置文件不存在，则返回默认值。
 *
 * @param key 配置项键名，例如 "auto/enable"。
 * @param defaultValue 默认值；当读取失败或配置不存在时返回该值。
 * @return 配置项对应的布尔值。
 */
bool AutoConnectManager::readBoolValue(const QString &key, bool defaultValue) const
{
    const QString path = configFilePath();
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return defaultValue;
    }

    QSettings ini(path, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");
    return ini.value(key, defaultValue).toBool();
}


/**
 * @brief 向配置文件写入布尔值。
 *
 * 写入失败时会通过 logMessage 信号输出日志。
 *
 * @param key 配置项键名，例如 "auto/auto_mqtt"。
 * @param value 要写入的布尔值。
 * @return 写入成功返回 true，否则返回 false。
 */
bool AutoConnectManager::writeBoolValue(const QString &key, bool value)
{
    const QString path = configFilePath();
    if (path.isEmpty()) {
        emit logMessage("AutoConnectManager: 配置文件路径为空，无法写入");
        return false;
    }

    QSettings ini(path, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");
    ini.setValue(key, value);
    ini.sync();

    if (ini.status() != QSettings::NoError) {
        emit logMessage(QString("AutoConnectManager: 写入配置失败 key=%1").arg(key));
        return false;
    }

    return true;
}


/**
 * @brief 设置总自动启动开关。
 *
 * 对应配置项：auto/enable
 *
 * @param enabled 是否启用自动启动。
 * @return 设置成功返回 true，否则返回 false。
 */
bool AutoConnectManager::setAutoEnabled(bool enabled)
{
    return writeBoolValue("auto/enable", enabled);
}


/**
 * @brief 判断是否启用了总自动启动开关。
 *
 * 对应配置项：auto/enable
 *
 * @return 启用返回 true，否则返回 false。
 */
bool AutoConnectManager::isAutoEnabled() const
{
    return readBoolValue("auto/enable", false);
}


/**
 * @brief 判断是否启用了自动网络连接。
 *
 * 对应配置项：auto/auto_network
 *
 * @return 启用返回 true，否则返回 false。
 */
bool AutoConnectManager::isAutoNetworkEnabled() const
{
    return readBoolValue("auto/auto_network", true);
}


/**
 * @brief 判断是否启用了自动 MQTT 连接。
 *
 * 对应配置项：auto/auto_mqtt
 *
 * @return 启用返回 true，否则返回 false。
 */
bool AutoConnectManager::isAutoMqttEnabled() const
{
    return readBoolValue("auto/auto_mqtt", true);
}


/**
 * @brief 判断是否启用了自动 FTP 准备流程。
 *
 * 对应配置项：auto/auto_ftp
 *
 * @return 启用返回 true，否则返回 false。
 */
bool AutoConnectManager::isAutoFtpEnabled() const
{
    return readBoolValue("auto/auto_ftp", true);
}


/**
 * @brief 设置是否启用自动网络连接。
 *
 * 对应配置项：auto/auto_network
 *
 * @param enabled 是否启用。
 * @return 设置成功返回 true，否则返回 false。
 */
bool AutoConnectManager::setAutoNetworkEnabled(bool enabled)
{
    return writeBoolValue("auto/auto_network", enabled);
}


/**
 * @brief 设置是否启用自动 MQTT 连接。
 *
 * 对应配置项：auto/auto_mqtt
 *
 * @param enabled 是否启用。
 * @return 设置成功返回 true，否则返回 false。
 */
bool AutoConnectManager::setAutoMqttEnabled(bool enabled)
{
    return writeBoolValue("auto/auto_mqtt", enabled);
}


/**
 * @brief 设置是否启用自动 FTP 准备流程。
 *
 * 对应配置项：auto/auto_ftp
 *
 * @param enabled 是否启用。
 * @return 设置成功返回 true，否则返回 false。
 */
bool AutoConnectManager::setAutoFtpEnabled(bool enabled)
{
    return writeBoolValue("auto/auto_ftp", enabled);
}


/**
 * @brief 当总自动启动开关启用时，在指定延时后触发自动启动流程。
 *
 * 若未启用自动启动，则直接输出日志并发送 autoStartFinished() 信号。
 *
 * @param delayMs 延迟执行时间，单位毫秒。
 */
void AutoConnectManager::startIfEnabled(int delayMs)
{
    if (!isAutoEnabled()) {
        emit logMessage("自动连接未启用，跳过自动启动");
        emit autoStartFinished();
        return;
    }

    emit logMessage("检测到 auto/enable=true，准备自动启动");

    QTimer::singleShot(delayMs, this, [this]() {
        doAutoStart();
    });
}


/**
 * @brief 执行自动启动流程。
 *
 * 自动启动流程包含以下步骤：
 * 1. 根据配置决定是否触发自动网络连接
 * 2. 根据配置决定是否执行自动 MQTT 连接
 * 3. 根据配置决定是否触发 FTP 自动准备
 *
 * MQTT 自动连接流程会复用已有页面逻辑：
 * - 调用 MqttPage::loadInputsFromConfigFile() 读取配置
 * - 调用 MqttPage::onConnectButtonClicked() 触发连接
 *
 * 执行结束后会发送 autoStartFinished() 信号。
 */
void AutoConnectManager::doAutoStart()
{
    const QString path = configFilePath();
    emit logMessage(QString("自动启动开始，配置文件：%1").arg(path));

    // 1) 自动网络
    if (isAutoNetworkEnabled()) {
        emit logMessage("触发自动连接网络");
        emit requestAutoConnectNetwork();
    } else {
        emit logMessage("auto/auto_network=false，跳过网络自动连接");
    }

    // 3) 自动 FTP
    if (isAutoFtpEnabled()) {
        emit logMessage("触发 FTP 自动准备");
        emit requestAutoPrepareFtp();
    } else {
        emit logMessage("auto/auto_ftp=false，跳过 FTP 自动准备");
    }

    emit logMessage("自动启动流程结束");
    emit autoStartFinished();
}
