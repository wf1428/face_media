/**
 * @file web_local_api.h
 * @brief 独立于 MQTT 的标准 Web 本地业务入口声明。
 * @author Du lin
 * @date 2026-10-16
 * @note 最近修改：2026-10-23；修改人：Du lin；补充通行权限保存不返回人脸图片的契约，保留凭证查询、额度更新及幂等事务语义。
 */

#pragma once
#include "web_service.h"
#include <QHash>

/** @brief 在 Web 工作线程集中处理本地标准业务、事务和任务状态。 */
class WebLocalApi final : public QObject {
    Q_OBJECT

public:

    /**
     * @brief 接收本地异步完成信号，并定时清理本模块的临时图片。
     * @param config 设备受控配置，包含运行所需路径或功能设置。
     * @param parent Qt 父对象；对象销毁由父子关系管理，空指针表示无父对象。
     */
    explicit WebLocalApi(const WebServiceConfig &config, QObject *parent = nullptr);

    /**
     * @brief 销毁业务对象时释放未完成人脸任务持有的人员互斥门。
     */
    ~WebLocalApi() override;

    /**
     * @brief 初始化本地辅助表、兼容字段及中断任务恢复状态。
     */
    void initialize();

    /**
     * @brief 分派标准本地业务指令，处理能力缺口、幂等写入与异步任务提交。
     * @param operation 白名单业务操作名。
     * @param args 操作对应的结构化参数。
     * @param owner 已认证 Web 账号主键，用于任务归属校验。
     * @return 白名单操作的业务结果；校验或执行失败抛出 WebError。
     */
    QJsonObject dispatch(const QString &operation, const QJsonObject &args, int owner);

    /**
     * @brief 接收既有推理线程结果，复核图库冲突并追加单张本地人脸。
     * @param token 贯穿请求与异步结果的任务标识。
     * @param accepted 通过质量、防重校验的人脸条目。
     * @param failed 校验失败的人脸条目及原因。
     * @return 结果属于本对象管理的单项人脸任务时为 true，否则为 false。
     */
    bool applyFaces(const QString &token, const QJsonArray &accepted, const QJsonArray &failed);

signals:

    /**
     * @brief 本地人员或识别特征实际变化后，通知原业务刷新清单及图库。
     */
    void personnelChanged();

    /**
     * @brief 将资源清单送往既有人脸质量和特征提取线程。
     * @param token 贯穿请求与异步结果的任务标识。
     * @param faces 待校验的人脸条目，含人员及资源信息。
     */
    void faceValidationRequested(const QString &token, const QJsonArray &faces);

private:

    /**
     * @brief 读取未删除本地人员的公开资料和编辑版本。
     * @param id 目标记录或任务的标识。
     * @return 当前有效本地人员的资料。
     */
    QJsonObject person(qint64 id) const;

    /**
     * @brief 从原本地表汇总人员的卡、二维码、人脸、规则、楼层和实际用量。
     * @param id 目标记录或任务的标识。
     * @param images 为 true 时附带人脸图片资源引用；false 保留人脸元数据，省略供 HTTP 读取图片的路径。
     * @return 当前人员的凭证聚合及按需附带的图片信息。
     * @note 总额度至少覆盖实际剩余和已用之和，兼容旧版仅增加余额的记录；读取不写数据库。
     */
    QJsonObject credential(qint64 id, bool images = true) const;

    /**
     * @brief 按同一 SQL 快照统计人员凭证，并读取实际热点 IP 与媒体分区容量。
     * @return 本地人员、凭证及设备资源统计。
     */
    QJsonObject overview() const;

    /**
     * @brief 报告当前已接入能力、硬件就绪状态和允许编辑的规则字段。
     * @return 当前已接入功能与运行就绪状态。
     */
    QJsonObject capabilities() const;

    /**
     * @brief 按账号归属读取异步任务状态、结果和业务错误。
     * @param id 目标记录或任务的标识。
     * @param owner 已认证 Web 账号主键，用于任务归属校验。
     * @return 所属账号可见的任务状态；不存在或不可见时抛出 WebError。
     */
    QJsonObject job(const QString &id, int owner) const;

    /**
     * @brief 按账号、业务类型和幂等键登记或复用任务，校验请求摘要及资源上限。
     * @param kind 资源或任务类型，用于选择目录、配额和校验规则。
     * @param args 操作对应的结构化参数。
     * @param owner 已认证 Web 账号主键，用于任务归属校验。
     * @return 已持久化的异步操作标识，重复幂等键复用既有任务。
     */
    QString beginJob(const QString &kind, const QJsonObject &args, int owner);

    /**
     * @brief 保存任务完成事实；严格模式下让结果与业务写入在同一事务提交。
     * @param id 目标记录或任务的标识。
     * @param data 本次操作的数据对象。
     * @param code 业务状态码，成功通常为 200。
     * @param message 可返回调用方的结果或错误说明。
     * @param strict 为 true 时要求任务仍处于可完成状态，否则拒绝覆盖。
     */
    void finishJob(const QString &id,
                   const QJsonObject &data,
                   int code,
                   const QString &message,
                   bool strict = false);

    /**
     * @brief 按保留时间清理 Web 暂存图片和已结束任务，不处理原导入目录。
     */
    void collectAssets();

    /**
     * @brief 事务化新增、差量修改或软删除本地人员，保留现有凭证与消费记录。
     * @param operation 白名单业务操作名。
     * @param args 操作对应的结构化参数。
     * @param owner 已认证 Web 账号主键，用于任务归属校验。
     * @return 实际写入后的人员业务结果；失败抛出 WebError。
     */
    QJsonObject mutatePerson(const QString &operation, const QJsonObject &args, int owner);

    /**
     * @brief 处理人脸资源或事务化修改指定凭证、规则和人员楼层。
     * @param operation 白名单业务操作名。
     * @param args 操作对应的结构化参数。
     * @param owner 已认证 Web 账号主键，用于任务归属校验。
     * @return 凭证写入结果或待查询的异步任务。
     * @note credential.update 的响应及幂等回执不附带人脸图片引用；图片仍通过原凭证查询返回。
     * @note 显式充值同时增加总额度及余额，不清空已用量；原幂等操作记录保证相同请求键不重复充值。
     * @note 新提交的增加金额及单价最多两位小数，非零最小 0.01；增加金额 0 表示不充值，启用金额时单价必须大于 0。
     */
    QJsonObject mutateCredential(const QString &operation, const QJsonObject &args, int owner);

    /**
     * @brief 读取公共梯控配置，或保存楼层/模式后通过本地 IO 异步发送。
     * @param operation 白名单业务操作名。
     * @param args 操作对应的结构化参数。
     * @param owner 已认证 Web 账号主键，用于任务归属校验。
     * @return 梯控查询结果或已受理任务信息。
     * @note 楼层和特殊模式读写 online_v1 共享配置；持久控制使用 132 字节 OPEN 帧并叠加模式，响应区分保存、发送和硬件确认。
     */
    QJsonObject elevator(const QString &operation, const QJsonObject &args, int owner);

    WebServiceConfig config_;         /**< 由 Qt 部署环境提供的受控路径和辅助程序。 */
    QHash<QString, bool> faceLeases_; /**< 尚持有人员互斥门的人脸任务。 */
};
