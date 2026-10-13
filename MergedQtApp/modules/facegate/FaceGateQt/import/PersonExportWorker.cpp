/**
 * @file PersonExportWorker.cpp
 * @brief 人员 XLSX 导出的后台任务实现。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#include "PersonExportWorker.h"

#include "UsbPersonExportTarget.h"

#include <QDateTime>
#include <QDir>

/** @brief 保存人员范围；任务实际工作由后台线程调用 run() 开始。 */
PersonExportWorker::PersonExportWorker(PersonExportKind kind, QObject *parent)
    : QObject(parent),
      kind_(kind)
{
}

/** @brief 原子设置取消标志，避免从界面线程直接操作后台资源。 */
void PersonExportWorker::cancel()
{
    cancelled_.store(true);
}

/**
 * @brief 完成 U 盘准备、人员查询、XLSX 写入以及介质同步卸载。
 *
 * 无论导出成功还是失败，只要卷已准备完成都会执行同步和卸载，
 * 防止调用方在后台仍持有挂载点时提示用户拔出 U 盘。
 */
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

    // 文件写入结果不能跳过介质收尾；失败路径同样需要释放挂载资源。
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
