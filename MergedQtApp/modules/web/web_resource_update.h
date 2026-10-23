/**
 * @file web_resource_update.h
 * @brief Qt 内部资源归档校验、解压、目录事务与重启调度。
 * @author Du lin
 * @date 2026-10-20
 * @note 最近修改：2026-10-23；修改人：Du lin；向人员导入提供复用归档读取器的图片展开接口，不执行资源目录替换或重启。
 */
#pragma once

#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QThread>
#include <functional>

/** @brief 路径和命令仅来自 Qt 的受控配置，不接受网页指定。 */
struct WebResourceOptions {
    QString archive; /**< 已上传归档的受控完整路径。 */
    QString destination; /**< 资源 images 最终根目录。 */
    QString result; /**< 可供中断恢复读取的结果 JSON 路径。 */
    QStringList restart; /**< 目录发布完成后运行的固定重启命令及参数。 */
    bool recovery = false; /**< 仅恢复目录事务，不重复解压已完成的归档。 */
};

/** @brief 专用线程只操作文件，不访问数据库、播放器或 MQTT。 */
class WebResourceUpdate final : public QThread {
    Q_OBJECT

public:

    /** @brief 进度回调参数依次为百分比、说明和阶段；在执行线程调用。 */
    using Progress = std::function<void(int, const QString &, const QString &)>;

    /**
     * @brief 保存任务参数，实际处理在 start 后的独立工作线程运行。
     * @param options 受控资源更新参数；路径和重启命令均不来自网页。
     * @param parent Qt 父对象；对象销毁由父子关系管理，空指针表示无父对象。
     */
    explicit WebResourceUpdate(const WebResourceOptions &options, QObject *parent = nullptr);

    /**
     * @brief 检查资源更新所需的 libarchive 接口是否可用。
     * @return 系统 libarchive 的必要读取接口是否可用，不影响普通 Qt 启动。
     */
    static bool available();

    /**
     * @brief 流式展开人员图片包，目录名不限，按原始文件名返回图片。
     * @param archive 已上传的普通归档文件，最多 100 MiB。
     * @param destination 尚不存在的受控批次子目录，失败时由批次负责清理。
     * @param expandedBudget 当前人员配额允许展开的字节数，不含已上传归档。
     * @return 图片资源列表；最多 499 张，每张沿用直接上传的格式、10 MiB 和像素限制。
     * @throws WebError 路径穿越、链接、加密、重复文件名、空间不足或无有效图片时拒绝。
     * @note 在后台线程调用；使用与 UI 资源相同的 libarchive、字符集和固定块读取器。
     */
    static QJsonArray extractFaceImages(const QString &archive, const QString &destination,
                                       qint64 expandedBudget);

    /**
     * @brief 执行受控资源包更新或目录事务恢复。
     * @param options 受控资源更新参数；路径和重启命令均不来自网页。
     * @param progress 实际进度回调，不提供时忽略进度通知。
     * @return 更新或恢复的实际结果；测试与工作线程使用同一条生产路径。
     * @note 仅操作受控文件路径；归档校验、目录替换和结果落盘在线程内完成，不访问数据库或控件。
     */
    static QJsonObject execute(const WebResourceOptions &options, Progress progress = {});

signals:
    /**
     * @brief 实际检查、解压和替换进度，由接收方排队更新设备界面。
     * @param percent 实际处理进度百分比，范围 0~100。
     * @param message 可返回调用方的结果或错误说明。
     * @param phase 资源更新的当前阶段。
     */
    void progress(int percent, const QString &message, const QString &phase);
    /**
     * @brief 文件处理及结果落盘后，通知 Web 线程登记最终数据库状态。
     * @param result 处理结果。
     */
    void completed(const QJsonObject &result);

protected:

    /**
     * @brief 不占用界面或 Web RPC 线程执行大文件处理。
     */
    void run() override;

private:

    WebResourceOptions options_; /**< 线程启动前复制的受控任务参数。 */
};
