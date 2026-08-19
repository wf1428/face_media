/**
 * @file PersonExportWorker.cpp
 * @brief 人员XLSX导出的后台任务实现。
 */

#include "PersonExportWorker.h"

#include "UsbPersonExportTarget.h"

#include <QDateTime>
#include <QDir>

PersonExportWorker::PersonExportWorker(PersonExportKind kind, QObject *parent)
    : QObject(parent),
      kind_(kind)
{
}

void PersonExportWorker::cancel()
{
    cancelled_.store(true);
}

void PersonExportWorker::run()
{
    const QString kindName = PersonXlsxExporter::kindName(kind_);
    emit progress(QStringLiteral("正在检测可写U盘…"));
    const UsbPersonExportVolume volume =
            UsbPersonExportTarget::waitForWritableVolume(30000, &cancelled_);
    if (!volume.ok) {
        emit finished(false, 0, kindName, QString(), volume.error);
        return;
    }

    const QString fileName = QStringLiteral("export_person_%1.xlsx")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss")));
    const QString filePath = QDir(volume.personDirectory).absoluteFilePath(fileName);
    emit progress(QStringLiteral("正在读取%1并生成XLSX…").arg(kindName));

    PersonXlsxExportResult exported;
    if (cancelled_.load()) {
        exported.error = QStringLiteral("人员导出已取消");
    } else {
        exported = PersonXlsxExporter::exportToFile(kind_, filePath);
    }

    emit progress(QStringLiteral("正在同步数据并安全卸载U盘…"));
    QString unmountError;
    const bool unmounted = UsbPersonExportTarget::flushAndUnmount(volume, &unmountError);
    if (!unmounted) {
        const QString message = exported.ok
                ? QStringLiteral("文件已写入，但%1。请勿立即拔出U盘").arg(unmountError)
                : exported.error + QStringLiteral("；") + unmountError;
        emit finished(false, exported.personCount, kindName, fileName, message);
        return;
    }

    if (!exported.ok) {
        emit finished(false, exported.personCount, kindName, fileName, exported.error);
        return;
    }
    emit finished(true,
                  exported.personCount,
                  kindName,
                  fileName,
                  QStringLiteral("%1导出完成，共 %2 人。文件已保存到U盘 person/%3")
                  .arg(kindName).arg(exported.personCount).arg(fileName));
}
