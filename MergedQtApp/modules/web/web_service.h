/**
 * @file web_service.h
 * @brief Qt 独立 Web 工作线程：上传发布、文件清单和本地 Excel 批次。
 * @author Du lin
 * @date 2026-10-09
 */
#pragma once
#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariantMap>
#include <QStringList>

/** @brief 板端 Web 路径配置；生产由环境/平台确定，测试可以使用临时目录。 */
struct WebServiceConfig {
    QString database; /**< 现有统一 SQLite 路径。 */
    QString media; /**< 现有视频根目录，不由浏览器指定。 */
    QString mount; /**< 预期的真实媒体挂载点。 */
    QString imports; /**< Excel/图片批次暂存根目录。 */
    QString factory; /**< 仅空 Web 账号库读入的工厂摘要文件。 */
};

/** @brief 仅在专属线程访问 DbStore；所有 RPC 由白名单操作分发。 */
class WebService final : public QObject {
    Q_OBJECT
public:
    /** @brief 保存受控配置，启动后才打开本线程数据库。 */
    explicit WebService(const WebServiceConfig &config, QObject *parent = nullptr);
    /** @brief 退出时释放唯一导入预约，未完成批次在重启后核对。 */
    ~WebService() override;
    /** @brief 初始化 Web schema、工厂凭据与中断任务恢复。 */
    void initialize();
    /** @return 版本化操作的 JSON 数据；失败抛出 WebError，由 IPC 统一封装。 */
    QJsonObject dispatch(const QString &operation, const QJsonObject &args, int owner);
public slots:
    /** @brief 接收异步人脸结果，只接受当前批次 token。 */
    void applyFaces(const QString &token, const QJsonArray &accepted, const QJsonArray &failed);
signals:
    /** @brief 异步提交质量/防重校验给现有推理 worker。 */
    void faceValidationRequested(const QString &token, const QJsonArray &faces);
    /** @brief 已写入本地数据，需要刷新现有图库/列表。 */
    void personnelChanged();
private:
    /** @return 隐藏内部磁盘路径后的任务状态。 */
    QJsonObject publicTask(const QVariantMap &task) const;
    /** @return 新上传任务及 daemon 所需的受控临时路径。 */
    QJsonObject createUpload(const QJsonObject &args, int owner);
    /** @return 幂等发布结果；执行长度、内容、空间及同步检查。 */
    QJsonObject commit(const QString &id, int owner);
    /** @return 基于目录实际文件构建的完整视频清单。 */
    QJsonObject files();
    /** @brief 根据当前真实空间、原配额及 Web 预留检查资源。 */
    void checkBudget(const QString &kind, qint64 incoming = 0);
    /** @brief 后台执行封存批次，部分成功保留，不自动重放。 */
    void importBatch(const QString &id);
    /** @brief 保存最终结果并释放本地导入互斥预约。 */
    void finishImport(const QString &state);
    /** @brief 只清理受控 UUID 批次的中间文件；保留数据库统计，不删除正式视频/注册照。 */
    void cleanupBatch(const QString &id);
    /** @brief 恢复已落盘未登记任务；不完整接收标失败并清理临时文件。 */
    void recover();
    WebServiceConfig config_; /**< 不可由请求修改的设备配置。 */
    QString currentBatch_; /**< 单次 Web 本地导入的批次号。 */
    QJsonObject importResult_; /**< 逐阶段保留的实际结果。 */
    bool importLease_ = false; /**< 当前持有本地导入互斥权。 */
};
