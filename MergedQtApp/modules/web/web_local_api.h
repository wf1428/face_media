/**
 * @file web_local_api.h
 * @brief 独立于 MQTT 的标准 Web 本地业务入口声明。
 * @author Du lin
 * @date 2026-10-16
 */

#pragma once
#include "web_service.h"
#include <QHash>

/** @brief 在 Web 工作线程集中处理本地标准业务、事务和任务状态。 */
class WebLocalApi final : public QObject {
    Q_OBJECT

public:

    /** @brief 接收本地异步完成信号，并定时清理本模块的临时图片。 */
    explicit WebLocalApi(const WebServiceConfig &config, QObject *parent = nullptr);

    /** @brief 销毁业务对象时释放未完成人脸任务持有的人员互斥门。 */
    ~WebLocalApi() override;

    /** @brief 初始化本地辅助表、兼容字段及中断任务恢复状态。 */
    void initialize();

    /** @brief 分派标准本地业务指令，处理能力缺口、幂等写入与异步任务提交。 */
    QJsonObject dispatch(const QString &operation, const QJsonObject &args, int owner);

    /** @brief 接收既有推理线程结果，复核图库冲突并追加单张本地人脸。 */
    bool applyFaces(const QString &token, const QJsonArray &accepted, const QJsonArray &failed);

signals:

    /** @brief 本地人员或识别特征实际变化后，通知原业务刷新清单及图库。 */
    void personnelChanged();

    /** @brief 将资源清单送往既有人脸质量和特征提取线程。 */
    void faceValidationRequested(const QString &token, const QJsonArray &faces);

private:

    /** @brief 读取未删除本地人员的公开资料和编辑版本。 */
    QJsonObject person(qint64 id) const;

    /** @brief 从原本地表汇总人员的卡、二维码、人脸、规则、楼层和实际用量。 */
    QJsonObject credential(qint64 id, bool images = true) const;

    /** @brief 按同一 SQL 快照统计人员凭证，并读取实际热点 IP 与媒体分区容量。 */
    QJsonObject overview() const;

    /** @brief 报告当前已接入能力、硬件就绪状态和允许编辑的规则字段。 */
    QJsonObject capabilities() const;

    /** @brief 按账号归属读取异步任务状态、结果和业务错误。 */
    QJsonObject job(const QString &id, int owner) const;

    /** @brief 按账号、业务类型和幂等键登记或复用任务，校验请求摘要及资源上限。 */
    QString beginJob(const QString &kind, const QJsonObject &args, int owner);

    /** @brief 保存任务完成事实；严格模式下让结果与业务写入在同一事务提交。 */
    void finishJob(const QString &id,
                   const QJsonObject &data,
                   int code,
                   const QString &message,
                   bool strict = false);

    /** @brief 按保留时间清理 Web 暂存图片和已结束任务，不处理原导入目录。 */
    void collectAssets();

    /** @brief 事务化新增、差量修改或软删除本地人员，保留现有凭证与消费记录。 */
    QJsonObject mutatePerson(const QString &operation, const QJsonObject &args, int owner);

    /** @brief 处理人脸资源或事务化修改指定凭证、规则和人员楼层。 */
    QJsonObject mutateCredential(const QString &operation, const QJsonObject &args, int owner);

    /** @brief 读取公共梯控配置，或保存楼层/模式后通过本地 IO 异步发送。 */
    QJsonObject elevator(const QString &operation, const QJsonObject &args, int owner);

    WebServiceConfig config_;         /**< 由 Qt 部署环境提供的受控路径和辅助程序。 */
    QHash<QString, bool> faceLeases_; /**< 尚持有人员互斥门的人脸任务。 */
};
