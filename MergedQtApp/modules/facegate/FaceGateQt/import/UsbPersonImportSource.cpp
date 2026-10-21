/**
 * @file UsbPersonImportSource.cpp
 * @brief U盘人员导入预约及Excel临时复制实现。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#include "UsbPersonImportSource.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSaveFile>
#include <QStringList>
#include <QThread>
#include <QUuid>

namespace {
const QString kImportControlDirectory = QStringLiteral("/run/face-media");
const QString kImportRequestPath = kImportControlDirectory
        + QStringLiteral("/person-import.request");
const QString kImportReadyPath = kImportControlDirectory
        + QStringLiteral("/person-import.ready");

/** @return 控制文件去除回车后的非空行。 */
QStringList controlFileLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QStringList lines;
    const QList<QByteArray> rawLines = file.readAll().split('\n');
    for (const QByteArray &rawLine : rawLines) {
        const QString line = QString::fromUtf8(rawLine).trimmed();
        if (!line.isEmpty()) lines.append(line);
    }
    return lines;
}

/** @return udisk 服务为指定令牌确认的挂载点。 */
QString readyMountPoint(const QString &requestToken)
{
    const QStringList lines = controlFileLines(kImportReadyPath);
    if (requestToken.isEmpty() || lines.size() < 2 || lines.at(0) != requestToken) {
        return QString();
    }
    return QDir(lines.at(1)).exists() ? QDir(lines.at(1)).absolutePath()
                                      : QString();
}

/** @return U盘 Ycest/person 目录。 */
QString sourcePersonDirectory(const QString &mountPoint)
{
    return QDir(mountPoint).absoluteFilePath(QStringLiteral("Ycest/person"));
}

/** @return Ycest/person 按文件名排序后的第一个非临时 XLS/XLSX。 */
QString firstWorkbook(const QString &mountPoint)
{
    const QDir personDir(sourcePersonDirectory(mountPoint));
    if (!personDir.exists()) return QString();
    const QFileInfoList files = personDir.entryInfoList(
                QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &file : files) {
        const QString suffix = file.suffix();
        const bool supported = suffix.compare(QStringLiteral("xls"), Qt::CaseInsensitive) == 0
                || suffix.compare(QStringLiteral("xlsx"), Qt::CaseInsensitive) == 0;
        if (supported && !file.fileName().startsWith(QStringLiteral("~$"))) {
            return file.absoluteFilePath();
        }
    }
    return QString();
}

/**
 * @brief 将指定挂载目录中列表第一个人员表复制到临时目录。
 * @return 包含来源文件名和临时路径的复制结果。
 */
UsbPersonImportFile copyWorkbook(const QString &mountPoint)
{
    UsbPersonImportFile result;
    result.mountPoint = mountPoint;
    const QString workbook = firstWorkbook(mountPoint);
    if (workbook.isEmpty()) {
        result.error = QStringLiteral("U盘 Ycest/person 目录下未找到 xls 或 xlsx 文件");
        return result;
    }

    const QString tempBase = QStringLiteral("/tmp/facegate_person_import");
    const QString tempDirectory = QDir(tempBase).absoluteFilePath(
                QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))
                + QLatin1Char('_')
                + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(tempDirectory)) {
        result.error = QStringLiteral("无法创建人员导入临时目录");
        return result;
    }
    result.temporaryRoot = tempDirectory;
    const QFileInfo sourceInfo(workbook);
    result.sourceFileName = sourceInfo.fileName();
    result.temporaryPath = QDir(tempDirectory).absoluteFilePath(sourceInfo.fileName());
    if (!QFile::copy(workbook, result.temporaryPath)) {
        result.error = QStringLiteral("复制人员表到临时目录失败");
        QDir(tempDirectory).removeRecursively();
        result.temporaryRoot.clear();
        result.temporaryPath.clear();
        return result;
    }

    const QString imageDirectory = QDir(sourcePersonDirectory(mountPoint))
            .absoluteFilePath(QStringLiteral("img"));
    if (QDir(imageDirectory).exists()) result.sourceImageDirectory = imageDirectory;
    result.ok = true;
    return result;
}

} // namespace

/** @brief 启动时清除遗留预约；不依赖旧PID是否已被其他进程复用。 */
void UsbPersonImportSource::clearImportRequestOnStartup()
{
    // 仅在程序入口、创建页面和导入任务之前调用，避免清除正在使用的预约。
    for (const QString &path : {kImportRequestPath, kImportReadyPath}) {
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            qWarning() << "无法清理遗留人员导入标记:" << path;
        }
    }
}

/** @brief 原子写入人员导入预约，使预约早于U盘插入事件。 */
QString UsbPersonImportSource::beginImportRequest(QString *error)
{
    if (!QDir().mkpath(kImportControlDirectory)) {
        if (error) *error = QStringLiteral("无法创建U盘导入控制目录");
        return QString();
    }

    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QFile::remove(kImportReadyPath);
    QSaveFile request(kImportRequestPath);
    if (!request.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("无法创建人员导入请求：%1").arg(request.errorString());
        return QString();
    }
    const QByteArray content = QByteArray::number(QCoreApplication::applicationPid())
            + '\n' + token.toUtf8() + '\n';
    if (request.write(content) != content.size() || !request.commit()) {
        if (error) *error = QStringLiteral("无法提交人员导入请求：%1").arg(request.errorString());
        return QString();
    }
    return token;
}

/** @brief 仅清理属于本批次的请求和就绪文件。 */
void UsbPersonImportSource::endImportRequest(const QString &requestToken)
{
    if (requestToken.isEmpty()) return;

    const QStringList requestLines = controlFileLines(kImportRequestPath);
    if (requestLines.size() >= 2 && requestLines.at(1) == requestToken) {
        QFile::remove(kImportRequestPath);
    }
    const QStringList readyLines = controlFileLines(kImportReadyPath);
    if (!readyLines.isEmpty() && readyLines.at(0) == requestToken) {
        QFile::remove(kImportReadyPath);
    }
}

/**
 * @brief 轮询 U 盘并把人员表复制到本机，完成后再返回调用方。
 *
 * 只有 udisk watcher 用同一令牌写入 ready 文件后才访问挂载目录。
 * Excel 复制完成后保持U盘挂载，供后续直接读取 img 目录；应用不再卸载U盘。
 */
UsbPersonImportFile UsbPersonImportSource::waitAndCopy(int timeoutMs,
                                                       const std::atomic_bool *cancelled,
                                                       const QString &requestToken)
{
    if (requestToken.trimmed().isEmpty()) {
        UsbPersonImportFile result;
        result.error = QStringLiteral("人员导入请求无效");
        return result;
    }
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (cancelled && cancelled->load()) {
            UsbPersonImportFile cancelledResult;
            cancelledResult.error = QStringLiteral("人员导入已取消");
            return cancelledResult;
        }

        const QString mountPoint = readyMountPoint(requestToken);
        if (!mountPoint.isEmpty() && !firstWorkbook(mountPoint).isEmpty()) {
            return copyWorkbook(mountPoint);
        }
        QThread::msleep(500);
    }

    UsbPersonImportFile result;
    result.error = QStringLiteral(
                "等待U盘超时，请确认 udisk 服务正常，且 /media/usb0/Ycest/person 下存在人员表");
    return result;
}
