/**
 * @file network_personnel_store.h
 * @brief MQTT 网络人员聚合数据的 SQLite 持久化与哈希幂等处理。
 */

#ifndef NETWORK_PERSONNEL_STORE_H
#define NETWORK_PERSONNEL_STORE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

/** @brief 一次网络人员同步请求的持久化处理结果。 */
struct NetworkPersonnelSyncResult
{
    bool ok = false;
    bool duplicate = false;
    QJsonObject data;
};

/** @brief 平台下发并经设备校验后的在线二维码楼层授权快照。 */
struct NetworkQrAuthorization
{
    QString qrCode;
    QString personId;
    QString sourceType;
    QString invitationId;
    QString floorHex;
    QStringList floors;
};

/** @brief qr.deductResult 的本地持久化及扣次结果。 */
struct NetworkQrDeductApplyResult
{
    bool ok = false;
    bool duplicate = false;
    bool countApplied = false;
    bool skipped = false;
    QString scanId;
    QString sourceType;
    QString error;
};

/** @brief 刷卡或人脸 deductResult 的本地同步结果。 */
struct NetworkAccessDeductApplyResult
{
    bool ok = false;
    bool duplicate = false;
    bool countApplied = false;
    bool amountApplied = false;
    bool skipped = false;
    qlonglong remainingCount = -1;
    qlonglong usedCount = -1;
    double remainingAmount = -1.0;
    double usedAmount = -1.0;
    QString error;
};

/** @brief 一笔远程呼梯事务在 RS485 完成后的持久化上下文。 */
struct NetworkRemoteCallContext
{
    bool ok = false;
    bool duplicate = false;
    bool localDeducted = false;
    QString requestId;
    QString personId;
    QString source;
    int floor = 0;
    qlonglong remainingCount = -1;
    QString error;
};

/** @brief remoteCall.deductResult 的本地应用结果。 */
struct NetworkRemoteCallDeductApplyResult
{
    bool ok = false;
    bool duplicate = false;
    bool remoteCountApplied = false;
    bool passCountApplied = false;
    bool skipped = false;
    QString requestId;
    qlonglong remainingCount = -1;
    qlonglong usedCount = -1;
    qlonglong passRemainingCount = -1;
    qlonglong passUsedCount = -1;
    QString error;
};

/** @brief 一次本地 XLSX 网络人员导入的汇总结果。 */
struct NetworkPersonnelImportResult
{
    bool ok = false;
    int inserted = 0;
    int updated = 0;
    int unchanged = 0;
    int failed = 0;
    QStringList unchangedNames;
    QStringList failedNames;
    QString error;
};

/**
 * @brief 在统一门禁数据库中保存网络人员、人脸元数据、IC 卡、二维码和权限。
 *
 * 本类只操作 network_* 表，不修改 FaceGate 原有的离线 person/face_feature 表。
 */
class NetworkPersonnelStore
{
public:
    /** @brief 打开统一 SQLite 并创建网络人员相关表。 */
    bool initialize();

    /** @brief 按 personHash 幂等地整体替换一个网络人员聚合。 */
    NetworkPersonnelSyncResult applyFullPersonnel(const QJsonObject &envelope);

    /** @brief 对比服务端 personId->personHash 清单和本地网络人员清单。 */
    NetworkPersonnelSyncResult checkAllPersonHashes(const QJsonObject &envelope);

    /** @brief 按 deleteType 删除人员或 items 中指定的凭证。 */
    NetworkPersonnelSyncResult deletePersonnel(const QJsonObject &envelope);

    QJsonArray networkPersonnelList() const;

    /**
     * @brief 按账号和既有凭证匹配 XLSX 人员，保存基础信息及完整权限快照。
     *
     * 已存在人员的 person_hash 与 network_face 数据保持不变。
     */
    NetworkPersonnelImportResult importSpreadsheetPersonnel(
            const QVector<QJsonObject> &records);
    QJsonArray pendingFaceImageRequests() const;
    /** @return 指定人员尚未完整落盘、落库和提取特征的人脸补拉参数。 */
    QJsonObject pendingFaceImageRequest(const QString &personId) const;
    NetworkPersonnelSyncResult applyFaceImages(const QJsonObject &envelope);

    /** @brief 保存通过图片的特征，并只清理校验失败的图片内容。 */
    bool finalizeFaceImageValidation(const QString &personId,
                                     const QJsonArray &validatedFaces,
                                     const QJsonArray &failedFaces,
                                     bool *allFacesReady = nullptr,
                                     QString *error = nullptr,
                                     QJsonArray *commitFailedFaces = nullptr,
                                     QJsonArray *commitFaceNotices = nullptr);

    /** @brief 建立一笔 online_v1 二维码扫码事务。 */
    bool beginQrAccessTransaction(const QString &scanId,
                                  const QString &deviceId,
                                  const QString &qrCode,
                                  QString *error = nullptr);

    /** @brief 保存 qr.scanResult，供故障追踪和消息去重。 */
    bool recordQrScanResult(const QString &scanId,
                            const QString &messageId,
                            int code,
                            bool success,
                            const QString &reason,
                            const QJsonObject &authorizationData,
                            bool *duplicate = nullptr,
                            QString *error = nullptr);

    /**
     * @brief 保存 qr.floorControl 和独立二维码授权缓存。
     * @param changed 授权楼层或归属信息实际变化时写 true。
     */
    bool saveQrFloorControl(const QString &scanId,
                            const QString &messageId,
                            const NetworkQrAuthorization &authorization,
                            bool *duplicate = nullptr,
                            bool *changed = nullptr,
                            QString *error = nullptr);

    /** @brief 记录本次二维码楼层帧的实际 RS485 发送结果。 */
    bool recordQrRs485Result(const QString &scanId,
                             bool success,
                             const QString &reason,
                             QString *error = nullptr);

    /** @brief 记录 qr.accessResult 是否已经提交给 mqttd。 */
    bool recordQrAccessResult(const QString &scanId,
                              const QString &messageId,
                              bool success,
                              bool submitted,
                              QString *error = nullptr);

    /** @brief 按二维码来源处理平台扣次结果，并校验人员与扫码事务归属。 */
    NetworkQrDeductApplyResult applyQrDeductResult(const QString &messageId,
                                                   const QString &qrCode,
                                                   const QString &sourceType,
                                                   const QString &personId,
                                                   int code,
                                                   bool deducted,
                                                   const QString &message,
                                                   const QVariant &remainingCount,
                                                   const QVariant &usedCount);

    /** @brief 按平台权威次数/金额同步刷卡或人脸扣减结果，并按消息 ID 去重。 */
    NetworkAccessDeductApplyResult applyAccessDeductResult(
            const QString &messageId,
            const QString &method,
            const QString &personId,
            int code,
            bool deducted,
            const QString &message,
            const QVariant &remainingCount,
            const QVariant &usedCount,
            const QVariant &remainingAmount,
            const QVariant &usedAmount);

    /** @brief 累加 MQTT 断线期间一次实际成功的本地通行。 */
    bool recordOfflineAccessResult(const QString &method,
                                   const QString &personId,
                                   bool success,
                                   QString *error = nullptr);

    /** @brief 读取指定 accessResult method 尚未批量上报的聚合记录。 */
    QJsonArray pendingOfflineAccessResults(const QString &method,
                                           QString *error = nullptr);

    /** @brief 批量消息提交成功后按快照次数消费记录，并保留期间新增次数。 */
    bool consumeOfflineAccessResults(const QString &method,
                                     const QJsonArray &records,
                                     QString *error = nullptr);

    /** @brief 建立远程呼梯事务；同一请求ID及同人员同楼层待处理事务均会去重。 */
    NetworkRemoteCallContext beginRemoteCallTransaction(
            const QString &requestId,
            const QString &personId,
            int floor,
            const QString &source);

    /** @brief 保存RS485结果；成功时按远程呼梯规则预扣一次并设置待确认标志。 */
    NetworkRemoteCallContext recordRemoteCallRs485Result(
            const QString &requestId,
            bool success,
            const QString &reason);

    /** @brief 保存 remoteCall.accessResult 的消息ID和提交状态。 */
    bool recordRemoteCallAccessResult(const QString &requestId,
                                      const QString &accessResultId,
                                      bool success,
                                      bool submitted,
                                      QString *error = nullptr);

    /** @brief 应用平台远程呼梯扣次结果，校准计数并清除本地预扣标志。 */
    NetworkRemoteCallDeductApplyResult applyRemoteCallDeductResult(
            const QString &messageId,
            const QString &personId,
            int floor,
            int code,
            bool deducted,
            const QString &message,
            const QVariant &remainingCount,
            const QVariant &usedCount,
            const QVariant &passRemainingCount,
            const QVariant &passUsedCount);

    /** @brief 把未完成二维码事务标记为失败或超时。 */
    bool finishQrAccessTransaction(const QString &scanId,
                                   const QString &state,
                                   const QString &reason,
                                   QString *error = nullptr);

private:
    /** @return 请求 JSON 的稳定诊断摘要，用于识别同 ID 不同内容。 */
    QString payloadHash(const QJsonObject &envelope) const;

    /** @return 已处理过相同消息时返回 true，并恢复原结果。 */
    bool loadCachedResult(const QJsonObject &envelope,
                          const QString &hash,
                          NetworkPersonnelSyncResult *result) const;

    /** @brief 把处理结果与业务事务一起写入幂等收件箱。 */
    bool recordResult(const QJsonObject &envelope,
                      const QString &personId,
                      const QString &hash,
                      const NetworkPersonnelSyncResult &result) const;

    /** @return 构造统一成功或失败 data。 */
    NetworkPersonnelSyncResult makeResult(bool ok,
                                          const QString &code,
                                          const QString &message,
                                          const QString &personId = QString()) const;

    bool initialized_ = false;
};

#endif // NETWORK_PERSONNEL_STORE_H
