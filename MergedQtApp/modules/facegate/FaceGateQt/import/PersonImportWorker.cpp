/**
 * @file PersonImportWorker.cpp
 * @brief XLS/XLSX 人员表后台导入任务实现。
 *
 * @author Du lin
 * @date 2026-08-28
 * @note 最近修改：2026-10-23；修改人：Du lin；U 盘人脸保存前复核最新本地与网络图库，冲突项保留失败原因，不改变人员表入库和图片替换规则。
 */

#include "PersonImportWorker.h"

#include "PersonXlsParser.h"
#include "PersonXlsxParser.h"
#include "UsbPersonImportSource.h"
#include "common/sql/dbstore.h"
#include "modules/web/local_import_gate.h"
#include "modules/web/web_face_policy.h"
#include "common/sql/web/web_sql.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

/** @brief 创建持有一次性U盘导入预约的后台任务。 */
PersonImportWorker::PersonImportWorker(const QString &importRequestToken,
                                       QObject *parent)
    : QObject(parent), importRequestToken_(importRequestToken)
{
}

PersonImportWorker::~PersonImportWorker()
{
    cleanupResources();
}

/** @brief 原子设置取消标志，供 U 盘轮询和解析前检查。 */
void PersonImportWorker::cancel()
{
    cancelled_.store(true);
}

/**
 * @brief 复制并解析 U 盘中的人员表，再批量写入本地人员数据库。
 *
 * Excel 先复制到本机临时目录，人脸图片始终读取U盘原路径；
 * 每条结束路径都清理临时副本并释放预约；U盘挂载和拔出由 udisk 服务管理。
 */
void PersonImportWorker::run()
{
    // Web 与 U 盘共用互斥预约；表格按原规则入库，图片通过统一防重后再保存。
    if (!LocalImportGate::acquire()) {
        cleanupResources();
        emit finished(false, 0, 0, 0, 0, {}, {}, QStringLiteral("已有本地人员导入任务，请稍后重试"), QString());
        return;
    }
    localImportLease_ = true;
    emit progress(5, QStringLiteral("正在检测U盘并查找 Ycest/person 目录下的人员表…"));
    const UsbPersonImportFile source = UsbPersonImportSource::waitAndCopy(
                30000, &cancelled_, importRequestToken_);
    if (!source.ok) {
        emit finished(false, 0, 0, 0, 0, {}, {}, source.error, source.sourceFileName);
        return;
    }
    temporaryRoot_ = source.temporaryRoot;
    sourceFileName_ = source.sourceFileName;

    emit progress(20, QStringLiteral("已复制 %1，正在解析人员资料…").arg(source.sourceFileName));
    // 旧版 XLS 使用 OLE/BIFF8 解析器，XLSX 使用 ZIP/XML 解析器，输出统一行模型。
    const bool legacyXls = source.temporaryPath.endsWith(
                QStringLiteral(".xls"), Qt::CaseInsensitive);
    const PersonXlsxParseResult parsed = legacyXls
            ? PersonXlsParser::parse(source.temporaryPath)
            : PersonXlsxParser::parse(source.temporaryPath);
    if (!parsed.ok) {
        QString message = parsed.error;
        if (!parsed.warnings.isEmpty()) {
            message += QStringLiteral("\n文件提示：")
                    + parsed.warnings.join(QStringLiteral("；"));
        }
        const QString cleanupError = cleanupResources();
        if (!cleanupError.isEmpty()) message += QStringLiteral("\n") + cleanupError;
        emit finished(false, 0, 0, 0, 0, {}, {}, message, source.sourceFileName);
        return;
    }
    if (cancelled_.load()) {
        QString message = QStringLiteral("人员导入已取消");
        const QString cleanupError = cleanupResources();
        if (!cleanupError.isEmpty()) message += QStringLiteral("\n") + cleanupError;
        emit finished(false, 0, 0, 0, 0, {}, {}, message, source.sourceFileName);
        return;
    }

    QVector<QJsonObject> records;
    records.reserve(parsed.records.size());
    for (const PersonSpreadsheetRecord &record : parsed.records) records.append(record.data);

    emit progress(45, QStringLiteral("已解析 %1 人，正在核对网络重复人员和卡号并更新本地库…").arg(records.size()));
    const LocalPersonnelImportResult imported = store_.importSpreadsheetPersonnel(records);
    inserted_ = imported.inserted;
    updated_ = imported.updated;
    unchanged_ = imported.unchanged;
    failed_ = imported.failed;
    unchangedNames_ = imported.unchangedNames;
    failedNames_ = imported.failedNames;
    parserWarnings_ = parsed.warnings;
    importError_ = imported.error;

    QHash<QString, LocalImportedPerson> peopleByAccount;
    QHash<QString, QStringList> namesByAccount;
    QSet<QString> ambiguousAccounts;
    for (const LocalImportedPerson &person : imported.importedPeople) {
        const QString account = person.personNo.trimmed();
        if (account.isEmpty()) continue;
        const QString accountKey = account.toCaseFolded();
        namesByAccount[accountKey].append(person.name);
        if (peopleByAccount.contains(accountKey)) ambiguousAccounts.insert(accountKey);
        else peopleByAccount.insert(accountKey, person);
    }
    for (const QString &account : ambiguousAccounts) {
        peopleByAccount.remove(account);
    }

    QJsonArray faceCandidates;
    const QDir imageDirectory(source.sourceImageDirectory);
    const QFileInfoList images = !source.sourceImageDirectory.isEmpty()
            && imageDirectory.exists()
            ? imageDirectory.entryInfoList(QDir::Files | QDir::Readable, QDir::Name)
            : QFileInfoList();
    const QRegularExpression naming(QStringLiteral("^(.+)_([0-9]+)$"));
    QSet<QString> reportedAmbiguousAccounts;
    for (const QFileInfo &image : images) {
        const QRegularExpressionMatch match = naming.match(image.completeBaseName());
        if (!match.hasMatch()) continue;
        const QString account = match.captured(1).trimmed();
        const QString accountKey = account.toCaseFolded();
        if (ambiguousAccounts.contains(accountKey)) {
            if (!reportedAmbiguousAccounts.contains(accountKey)) {
                reportedAmbiguousAccounts.insert(accountKey);
                for (const QString &name : namesByAccount.value(accountKey)) {
                    ++failed_;
                    failedNames_.append(name + QStringLiteral("（人脸图片账号无法唯一匹配）"));
                }
            }
            continue;
        }
        if (!peopleByAccount.contains(accountKey)) continue;
        const LocalImportedPerson person = peopleByAccount.value(accountKey);
        QJsonObject face;
        face.insert(QStringLiteral("localPersonId"), QString::number(person.personId));
        face.insert(QStringLiteral("personNo"), person.personNo);
        face.insert(QStringLiteral("personName"), person.name);
        face.insert(QStringLiteral("phone"), person.phone);
        face.insert(QStringLiteral("imagePath"), image.absoluteFilePath());
        face.insert(QStringLiteral("sourceFileName"), image.fileName());
        faceCandidates.append(face);
    }

    if (cancelled_.load()) {
        importError_ = QStringLiteral("人员导入已取消");
        complete();
        return;
    }
    if (faceCandidates.isEmpty()) {
        complete();
        return;
    }

    pendingToken_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    emit progress(70, QStringLiteral("人员资料已入库，正在校验 %1 张人脸图片并提取特征…")
                  .arg(faceCandidates.size()));
    emit faceValidationRequested(pendingToken_, faceCandidates);
}

/**
 * @brief 复核最新图库后保存本地图片，失败图片不替换旧人脸。
 * @param token 当前待校验任务标识，过期结果忽略。
 * @param validatedFaces 推理线程通过质量和相似度防重的图片。
 * @param failedFaces 推理线程拒绝的图片及原因。
 * @note 人员资料已按原规则入库；图库读取失败时拒绝本次图片，保留已有照片。
 */
void PersonImportWorker::applyFaceValidation(const QString &token,
                                             const QJsonArray &validatedFaces,
                                             const QJsonArray &failedFaces)
{
    if (token != pendingToken_ || pendingToken_.isEmpty()) return;
    pendingToken_.clear();
    emit progress(85, QStringLiteral("人脸图片校验完成，正在保存导入结果…"));
    QJsonArray verified, rejected = failedFaces;
    try {
        QVector<FaceRecord> local, network;
        WebFacePolicy::currentGalleries(&local, &network);
        const float threshold = validatedFaces.isEmpty() ? 1.0f
                : float(validatedFaces.first().toObject().value("duplicateThreshold").toDouble());
        WebFacePolicy::filter(validatedFaces, local, network, threshold, &verified, &rejected);
    } catch (const WebError &e) {
        for (const auto &value : validatedFaces) {
            auto face = value.toObject();
            face.insert("message", e.message);
            rejected.append(face);
        }
    }
    QHash<QString, QStringList> faceErrorsByPerson;
    for (const QJsonValue &value : rejected) {
        const QJsonObject face = value.toObject();
        const QString name = face.value(QStringLiteral("personName")).toString().trimmed();
        const QString personLabel = name.isEmpty()
                ? face.value(QStringLiteral("phone")).toString() : name;
        QString reason = face.value(QStringLiteral("message")).toString().trimmed();
        if (reason.isEmpty()) reason = QStringLiteral("质量校验未通过");
        if (!faceErrorsByPerson[personLabel].contains(reason)) {
            faceErrorsByPerson[personLabel].append(reason);
        }
    }
    for (auto it = faceErrorsByPerson.constBegin(); it != faceErrorsByPerson.constEnd(); ++it) {
        ++failed_;
        failedNames_.append(it.key() + QStringLiteral("（人脸图片未通过：")
                            + it.value().join(QStringLiteral("；")) + QChar(0xff09));
    }

    QStringList storageFailures;
    QString storageError;
    if (!store_.replaceValidatedFaces(verified, &savedFaces_,
                                      &storageFailures, &storageError)) {
        failed_ += storageFailures.size();
        failedNames_.append(storageFailures);
        if (!storageError.isEmpty()) importError_ = storageError;
    }
    complete();
}

QString PersonImportWorker::cleanupResources()
{
    if (localImportLease_) {
        localImportLease_ = false;
        LocalImportGate::release();
    }
    if (!temporaryRoot_.isEmpty()) QDir(temporaryRoot_).removeRecursively();
    temporaryRoot_.clear();
    UsbPersonImportSource::endImportRequest(importRequestToken_);
    importRequestToken_.clear();
    return QString();
}

void PersonImportWorker::complete(const QString &extraMessage)
{
    emit progress(100, QStringLiteral("正在完成导入…"));
    DbStore::close();
    const QString cleanupError = cleanupResources();
    failedNames_.removeDuplicates();
    failed_ = failedNames_.size();
    QString message = QStringLiteral("本地人员导入完成：新增 %1 人，覆盖 %2 人，人脸入库 %3 张，失败 %4 项")
            .arg(inserted_).arg(updated_).arg(savedFaces_).arg(failed_);
    if (!parserWarnings_.isEmpty()) {
        message += QStringLiteral("\n文件提示：") + parserWarnings_.join(QStringLiteral("；"));
    }
    if (!importError_.isEmpty()) message += QStringLiteral("\n") + importError_;
    if (!cleanupError.isEmpty()) message += QStringLiteral("\n") + cleanupError;
    if (!extraMessage.isEmpty()) message += QStringLiteral("\n") + extraMessage;
    emit finished(importError_.isEmpty() && cleanupError.isEmpty() && failed_ == 0,
                  inserted_, updated_, unchanged_, failed_, unchangedNames_,
                  failedNames_, message, sourceFileName_);
}
