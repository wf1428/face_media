/**
 * @file PersonImportWorker.cpp
 * @brief XLS/XLSX 人员表后台导入任务实现。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#include "PersonImportWorker.h"

#include "PersonXlsParser.h"
#include "PersonXlsxParser.h"
#include "UsbPersonImportSource.h"
#include "common/sql/dbstore.h"
#include "common/sql/network_personnel_store.h"

#include <QFile>

/** @brief 创建尚未启动的后台导入对象。 */
PersonImportWorker::PersonImportWorker(QObject *parent)
    : QObject(parent)
{
}

/** @brief 原子设置取消标志，供 U 盘轮询和解析前检查。 */
void PersonImportWorker::cancel()
{
    cancelled_.store(true);
}

/**
 * @brief 复制并解析 U 盘中的人员表，再批量写入网络人员数据库。
 *
 * 源文件先复制到本机临时目录，确保 U 盘卸载后解析和入库仍可继续；
 * 每条退出路径都删除临时副本，避免长期占用设备存储空间。
 */
void PersonImportWorker::run()
{
    emit progress(QStringLiteral("正在检测U盘并查找 person 目录下的人员表…"));
    const UsbPersonImportFile source = UsbPersonImportSource::waitAndCopy(30000, &cancelled_);
    if (!source.ok) {
        emit finished(false, 0, 0, 0, 0, {}, {}, source.error, source.sourceFileName);
        return;
    }

    emit progress(QStringLiteral("已复制 %1，正在解析人员资料…").arg(source.sourceFileName));
    // 旧版 XLS 使用 OLE/BIFF8 解析器，XLSX 使用 ZIP/XML 解析器，输出统一行模型。
    const bool legacyXls = source.temporaryPath.endsWith(
                QStringLiteral(".xls"), Qt::CaseInsensitive);
    const PersonXlsxParseResult parsed = legacyXls
            ? PersonXlsParser::parse(source.temporaryPath)
            : PersonXlsxParser::parse(source.temporaryPath);
    if (!parsed.ok) {
        QFile::remove(source.temporaryPath);
        emit finished(false, 0, 0, 0, 0, {}, {}, parsed.error, source.sourceFileName);
        return;
    }
    if (cancelled_.load()) {
        QFile::remove(source.temporaryPath);
        emit finished(false, 0, 0, 0, 0, {}, {}, QStringLiteral("人员导入已取消"), source.sourceFileName);
        return;
    }

    QVector<QJsonObject> records;
    records.reserve(parsed.records.size());
    for (const PersonSpreadsheetRecord &record : parsed.records) records.append(record.data);

    emit progress(QStringLiteral("已解析 %1 人，正在核对并更新数据库…").arg(records.size()));
    // 入库完成后关闭线程关联的数据库连接，再删除本地临时文件。
    NetworkPersonnelStore store;
    const NetworkPersonnelImportResult imported = store.importSpreadsheetPersonnel(records);
    DbStore::close();
    QFile::remove(source.temporaryPath);

    QString message = QStringLiteral("人员资料导入完成：新增 %1 人，更新 %2 人，未变化 %3 人，失败 %4 人")
            .arg(imported.inserted).arg(imported.updated)
            .arg(imported.unchanged).arg(imported.failed);
    if (!parsed.warnings.isEmpty()) {
        message += QStringLiteral("\n文件提示：") + parsed.warnings.join(QStringLiteral("；"));
    }
    if (!imported.error.isEmpty()) message += QStringLiteral("\n") + imported.error;
    emit finished(imported.ok,
                  imported.inserted,
                  imported.updated,
                  imported.unchanged,
                  imported.failed,
                  imported.unchangedNames,
                  imported.failedNames,
                  message,
                  source.sourceFileName);
}
