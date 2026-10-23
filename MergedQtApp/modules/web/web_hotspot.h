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

/** @brief 管理受控热点配置和 Qt 身份标记；所有路径来自部署配置。 */
class WebHotspot final : public QObject {
public:

    /**
     * @brief 绑定热点配置和就绪文件，定时发布设备身份而不在启动时配置 WiFi。
     * @param networkConfig net_cfg.ini 路径。
     * @param helper 固定的热点管理辅助程序路径。
     * @param readyFile Qt Web 就绪标记文件路径。
     * @param parent Qt 父对象；对象销毁由父子关系管理，空指针表示无父对象。
     */
    WebHotspot(const QString &networkConfig,
               const QString &helper,
               const QString &readyFile,
               QObject *parent);

    /**
     * @brief 销毁身份发布对象时移除本进程的 Web 就绪标记。
     */
    ~WebHotspot() override;

    /**
     * @brief 将当前 Qt PID、设备号和默认 SSID 原子写入就绪文件。
     */
    void synchronize();

    /**
     * @brief 返回实际设备号、热点名称和待重启/更新错误状态。
     * @return 实际热点配置及辅助程序报告的运行状态。
     */
    QJsonObject info() const;

    /**
     * @brief 保存 SSID 和可选新密码，延迟重新激活热点；空密码保留原密码。
     * @param ssid 热点名称，按 UTF-8 长度及字符规则校验。
     * @param password 新热点密码；空值表示保留原密码。
     * @return 保存后的热点状态；空密码保留原密码，重载异步执行。
     */
    QJsonObject save(const QString &ssid, const QString &password = {});

private:

    /**
     * @brief 执行受控热点命令，限时等待并按需取得标准输出。
     * @param arguments 传给固定辅助程序的独立命令参数。
     * @param[out] output 可选输出，接收辅助程序标准输出。
     * @param input 通过标准输入提供的数据，避免敏感内容出现在命令参数中。
     * @return 固定辅助程序成功结束时为 true；失败或超时为 false。
     */
    bool command(const QStringList &arguments,
                 QString *output = nullptr,
                 const QByteArray &input = {}) const;
    QString networkConfig_, helper_, readyFile_, updateError_; /**< 网络 INI、固定辅助程序、就绪标记路径及最近错误。 */
    bool reloadPending_ = false; /**< 热点重新激活已排队，避免并发重载。 */
};
