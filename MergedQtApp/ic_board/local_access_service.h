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
    NetworkAccessResult checkQrCode(const QString &qrCode,
                                    const QDateTime &now = QDateTime::currentDateTime());
    NetworkAccessResult checkPassword(const QString &password,
                                      const QDateTime &now = QDateTime::currentDateTime());
    NetworkAccessResult checkFacePerson(qint64 personId,
                                        const QDateTime &now = QDateTime::currentDateTime());
    bool recordSuccessfulAccess(const NetworkAccessResult &result);

private:
    NetworkAccessResult evaluatePerson(qint64 personId, const QString &credential,
                                       const QString &credentialType, const QDateTime &now);
    LocalPersonnelStore personnelStore_;
};

#endif // LOCAL_ACCESS_SERVICE_H
