/**
 * @file network_access_service.h
 * @brief online_v1 网络人员本地凭证、规则和楼层权限判定。
 */
#ifndef NETWORK_ACCESS_SERVICE_H
#define NETWORK_ACCESS_SERVICE_H

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

#include "common/sql/network_personnel_store.h"

struct NetworkAccessResult
{
    bool pass = false;
    int code = 0;
    QString reason;
    QString personId;
    QString credential;
    QString credentialType;
    qint64 localPersonId = 0; /**< 本地离线人员数据库主键；网络人员始终为0。 */
    QList<int> floors;
    QByteArray rs485Frame;
};

struct CardHolderInfo
{
    QString personId;
    QString name;
};

class NetworkAccessService
{
public:
    bool initialize();
    NetworkAccessResult checkCardFrame(const QByteArray &cardFrame,
                                       const QDateTime &now = QDateTime::currentDateTime());
    /** @brief 使用读头提供的纯卡号校验网络人员权限。 */
    NetworkAccessResult checkCardId(const QString &cardId,
                                    const QDateTime &now = QDateTime::currentDateTime());
    /** @return 网络人员卡表中存在该卡号（含当前停用记录）时返回 true。 */
    bool hasCardId(const QString &cardId);
    /** @return 持有该物理卡号的全部网络人员；同一人员的多种卡号写法只返回一次。 */
    QList<CardHolderInfo> cardHolders(const QString &cardId);
    NetworkAccessResult checkQrCode(const QString &qrCode,
                                    const QDateTime &now = QDateTime::currentDateTime());
    NetworkAccessResult checkPassword(const QString &password,
                                      const QDateTime &now = QDateTime::currentDateTime());
    NetworkAccessResult checkFacePerson(const QString &personId,
                                        const QDateTime &now = QDateTime::currentDateTime());
    bool recordSuccessfulAccess(const NetworkAccessResult &result);

private:
    NetworkAccessResult evaluatePerson(const QString &personId,
                                       const QString &credential,
                                       const QString &credentialType,
                                       const QDateTime &now);
    NetworkPersonnelStore personnelStore_;
};

#endif
