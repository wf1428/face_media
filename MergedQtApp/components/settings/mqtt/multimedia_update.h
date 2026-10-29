/**
 * @file multimedia_update.h
 * @brief online_v1 多媒体模板清单下载、校验与成功回执后的资源发布。
 * @author Du lin
 * @date 2026-10-24
 */
#pragma once
#include <QJsonObject>
#include <QMutex>
#include <QStringList>
#include <QThread>
#include <QWaitCondition>
#include <QList>

/** @brief 四个进度区间的单次回执门控；由 MQTT 所在线程串行调用。 */
class MultimediaProgressBands final {
public:
    /**
     * @brief 返回本次已进入且尚未报告的区间进度；跨区间时补齐已经经过的边界。
     * @param percent 实际整包字节百分比，0~100；0 不上报，100 只补齐进度而不生成终态。
     * @return 最多四个百分比，分别位于 1~24、25~49、50~74、75~99；每段最多一次。
     */
    QList<int> advance(int percent) {
        QList<int> values;
        const int lower[] = {1, 25, 50, 75};
        const int upper[] = {24, 49, 74, 99};
        while (next_ < 4 && percent >= lower[next_]) {
            const int bucket = next_++;
            values.append(qBound(lower[bucket], percent, upper[bucket]));
        }
        return values;
    }
private:
    int next_ = 0; /**< 下一个尚未报告的区间索引；4 表示四段全部报告。 */
};

/** @brief 由本机指定的更新选项，平台不能指定目标根目录或重启程序。 */
struct MultimediaUpdateOptions {
    QJsonObject data; /**< 下发 data；screen、rollback 不读取。 */
    QString root; /**< 本机 images 根目录，暂存与最终资源位于同一文件系统。 */
    QStringList restart; /**< 完成替换后执行的受控重启调度程序及独立参数。 */
    QString netConfigMirror; /**< 与 U 盘流程一致的 net_cfg.ini 镜像路径；空值禁用镜像。 */
};

/**
 * @brief 在专用低优先级线程串行下载、校验并发布整包资源。
 * @details 工作线程不访问 QWidget、数据库或 MQTT；prepared 后等待主线程提交 ok，
 *          authorizeCommit(true) 才允许清理和覆盖正式资源。终态发出之后的本地错误仅记录。
 */
class MultimediaUpdate final : public QThread {
    Q_OBJECT
public:
    /**
     * @brief 复制本机选项；实际网络和文件操作在 start 后的工作线程执行。
     * @param options 本机指定的路径和重启命令，以及下发 data。
     * @param parent 对象所有者，可为空；销毁前必须取消并等待线程退出。
     */
    explicit MultimediaUpdate(const MultimediaUpdateOptions &options, QObject *parent = nullptr);
    /**
     * @brief 从主线程解除提交屏障，或取消等待。
     * @param submitted true 表示成功回执已提交本机 mqttd；false 表示终止且不改正式资源。
     * @note 线程安全，不执行文件操作；每个工作线程仅接受第一次决定。
     */
    void authorizeCommit(bool submitted);

signals:
    /**
     * @brief 后台字节进度，通过排队连接回到 MQTT/界面线程。
     * @param percent 整包实际下载百分比，0~100。
     * @param current 清单相对路径，仅供平台回执；界面详情不显示该值。
     */
    void progress(int percent, const QString &current);
    /** @brief 所有文件校验并同步完成，等待主线程提交成功回执。 */
    void prepared();
    /**
     * @brief 通知任务结束；跨线程排队回到所有者线程。
     * @param success 是否完成替换并安排重启。
     * @param acknowledged 是否已经获得成功回执提交授权；true 时禁止再回 failed。
     * @param errorCode 失败码；成功为空。
     * @param errorMsg 可读错误，不包含 FTP 凭据。
     */
    void completed(bool success, bool acknowledged, const QString &errorCode, const QString &errorMsg);
protected:
    /** @brief 执行全部网络等待、哈希、文件同步、清理、替换及重启调度；不占 UI 线程。 */
    void run() override;
private:
    MultimediaUpdateOptions options_; /**< 启动前复制的选项，运行中不再修改。 */
    QMutex decisionMutex_; /**< 只保护 decided_ 和 submitted_，不在锁内执行文件或网络操作。 */
    QWaitCondition decisionReady_; /**< 主线程提交回执或取消时唤醒工作线程。 */
    bool decided_ = false; /**< 首次提交决定到达时置 true。 */
    bool submitted_ = false; /**< true 允许覆盖资源；false 保持正式资源不变。 */
};
