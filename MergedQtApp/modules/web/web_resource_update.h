/**
 * @file web_resource_update.h
 * @brief Qt 内部资源归档校验、解压、目录事务与重启调度。
 * @author Du lin
 * @date 2026-10-20
 */
#pragma once

#include <QJsonObject>
#include <QStringList>
#include <QThread>
#include <functional>

/** @brief 路径和命令仅来自 Qt 的受控配置，不接受网页指定。 */
struct WebResourceOptions {
    QString archive;
    QString destination;
    QString result;
    QStringList restart;
    bool recovery = false;
};

/** @brief 专用线程只操作文件，不访问数据库、播放器或 MQTT。 */
class WebResourceUpdate final : public QThread {
    Q_OBJECT

public:

    using Progress = std::function<void(int, const QString &, const QString &)>;

    /** @brief 保存任务参数，实际处理在 start 后的独立工作线程运行。 */
    explicit WebResourceUpdate(const WebResourceOptions &options, QObject *parent = nullptr);

    /** @return 系统 libarchive 的必要读取接口是否可用，不影响普通 Qt 启动。 */
    static bool available();

    /** @return 更新或恢复的实际结果；测试与工作线程使用同一条生产路径。 */
    static QJsonObject execute(const WebResourceOptions &options, Progress progress = {});

signals:
    /** @brief 实际检查、解压和替换进度，由接收方排队更新设备界面。 */
    void progress(int percent, const QString &message, const QString &phase);
    /** @brief 文件处理及结果落盘后，通知 Web 线程登记最终数据库状态。 */
    void completed(const QJsonObject &result);

protected:

    /** @brief 不占用界面或 Web RPC 线程执行大文件处理。 */
    void run() override;

private:

    WebResourceOptions options_;
};
