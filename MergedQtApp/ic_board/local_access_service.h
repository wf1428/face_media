#ifndef LOCAL_ACCESS_SERVICE_H
#define LOCAL_ACCESS_SERVICE_H

#include "ic_board/network_access_service.h"
#include "common/sql/local_personnel_store.h"

/**
 * @brief 离线本地人员的凭证、规则、楼层和用量判定。
 *
 * 仅读写 person/face_feature/local_* 表，不产生任何 MQTT 事件。
 */
class LocalAccessService
{
public:
    bool initialize();
    NetworkAccessResult checkCardFrame(const QByteArray &cardFrame,
                                       const QDateTime &now = QDateTime::currentDateTime());
    /** @brief 使用读头提供的纯卡号校验本地人员权限。 */
    NetworkAccessResult checkCardId(const QString &cardId,
                                    const QDateTime &now = QDateTime::currentDateTime());
    /** @return 本地人员卡表中存在该卡号（含当前停用记录）时返回 true。 */
    bool hasCardId(const QString &cardId);
    /** @return 持有该物理卡号的全部本地人员；同一人员的多种卡号写法只返回一次。 */
    QList<CardHolderInfo> cardHolders(const QString &cardId);
    NetworkAccessResult checkQrCode(const QString &qrCode,
                                    const QDateTime &now = QDateTime::currentDateTime());
    NetworkAccessResult checkPassword(const QString &password,
                                      const QDateTime &now = QDateTime::currentDateTime());
    NetworkAccessResult checkFacePerson(qint64 personId,
                                        const QDateTime &now = QDateTime::currentDateTime());
    bool recordSuccessfulAccess(const NetworkAccessResult &result);

private:
    /** @brief 跨天时把远程呼梯当天剩余次数重置为每日额度，不累计。 */
    bool refreshRemoteCallDailyQuota(qint64 personId, const QDate &date);
    NetworkAccessResult evaluatePerson(qint64 personId, const QString &credential,
                                       const QString &credentialType, const QDateTime &now);
    LocalPersonnelStore personnelStore_;
};

#endif // LOCAL_ACCESS_SERVICE_H
