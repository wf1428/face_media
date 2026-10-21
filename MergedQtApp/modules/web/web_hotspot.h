/**
 * @file web_hotspot.h
 * @brief Qt 身份就绪文件和 NetworkManager 热点访问入口。
 * @author Du lin
 * @date 2026-10-16
 */

#pragma once

#include <QObject>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// Paths are supplied by Qt deployment configuration, never by HTTP requests.
class WebHotspot final : public QObject {
public:

    /** @brief 绑定热点配置和就绪文件，定时发布设备身份而不在启动时配置 WiFi。 */
    WebHotspot(const QString &networkConfig,
               const QString &helper,
               const QString &readyFile,
               QObject *parent);

    /** @brief 销毁身份发布对象时移除本进程的 Web 就绪标记。 */
    ~WebHotspot() override;

    /** @brief 将当前 Qt PID、设备号和默认 SSID 原子写入就绪文件。 */
    void synchronize();

    /** @brief 返回实际设备号、热点名称和待重启/更新错误状态。 */
    QJsonObject info() const;

    /** @brief 保存 SSID 和可选新密码，延迟重新激活热点；空密码保留原密码。 */
    QJsonObject save(const QString &ssid, const QString &password = {});

private:

    /** @brief 执行受控热点命令，限时等待并按需取得标准输出。 */
    bool command(const QStringList &arguments,
                 QString *output = nullptr,
                 const QByteArray &input = {}) const;
    QString networkConfig_, helper_, readyFile_, updateError_;
    bool reloadPending_ = false;
};
