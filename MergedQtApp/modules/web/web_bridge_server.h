/**
 * @file web_bridge_server.h
 * @brief 限长 NDJSON 本地 IPC 服务，数据操作在自己的 QThread 中执行。
 * @author Du lin
 * @date 2026-10-09
 */

#pragma once

#include "web_service.h"
#include <QThread>
class QLocalServer;

/** @brief 工作线程内拥有 Unix socket 与 WebService 的服务器对象。 */
class WebBridgeServer final : public QObject {
    Q_OBJECT

public:

    /** @brief 保存配置和 socket 路径，不在构造时打开数据库。 */
    WebBridgeServer(const WebServiceConfig &config, const QString &socketPath);

public slots:

    /** @brief 启动恢复、设置本地 socket 权限并连接处理器。 */
    void start();

    /** @brief 在所属线程停止接入并关闭数据库连接。 */
    void stop();

private:

    /** @brief 每个连接只处理一条限长请求，以 requestId 对应一次响应。 */
    void acceptConnections();

    WebServiceConfig config_;        /**< 板端资源路径。 */
    QString socketPath_;             /**< systemd 私有运行目录中的 socket。 */
    QLocalServer *server_ = nullptr; /**< 此线程中的 IPC listener。 */
    WebService *service_ = nullptr;  /**< 此线程中的业务服务。 */
    int clients_ = 0;                /**< 限制 IPC 并发连接避免未完成请求耗尽内存。 */
};

/** @brief 主线程生命周期包装；故障只禁用新增 Web 模块，原 Qt 业务继续。 */
class WebRuntime final : public QObject {
    Q_OBJECT

public:

    /** @brief MERGEDQT_WEB_ENABLE=1 时创建独立线程并启动服务。 */
    explicit WebRuntime(QObject *parent = nullptr);

    /** @brief 退出时等待当前文件/人员写入安全结束，避免销毁仍运行线程。 */
    ~WebRuntime() override;

private:

    QThread thread_;                    /**< Web SQL 与 IPC 线程。 */
    WebBridgeServer *server_ = nullptr; /**< 被移入 thread_ 的服务器。 */
};
