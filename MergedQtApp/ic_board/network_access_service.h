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
    QList<int> floors;
    QByteArray rs485Frame;
};

class NetworkAccessService
{
public:
    bool initialize();
    NetworkAccessResult checkCardFrame(const QByteArray &cardFrame,
                                       const QDateTime &now = QDateTime::currentDateTime());
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
