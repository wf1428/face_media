#ifndef LOCAL_PERSONNEL_STORE_H
#define LOCAL_PERSONNEL_STORE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct LocalImportedPerson
{
    qint64 personId = 0;
    QString personNo;
    QString name;
    QString phone;
};

struct LocalPersonnelImportResult
{
    bool ok = false;
    int inserted = 0;
    int updated = 0;
    int unchanged = 0;
    int failed = 0;
    QVector<LocalImportedPerson> importedPeople;
    QStringList unchangedNames;
    QStringList failedNames;
    QString error;
};

/**
 * @brief Excel 离线人员、凭证、权限和人脸特征的独立本地存储。
 *
 * 本类只写 person/face_feature 与 local_* 表，不写 network_* 表，
 * 也不产生任何 MQTT 同步任务。
 */
class LocalPersonnelStore
{
public:
    bool initialize();
    LocalPersonnelImportResult importSpreadsheetPersonnel(
            const QVector<QJsonObject> &records);
    bool replaceValidatedFaces(const QJsonArray &validatedFaces,
                               int *savedCount,
                               QStringList *failedPeople,
                               QString *error);
};

#endif // LOCAL_PERSONNEL_STORE_H
