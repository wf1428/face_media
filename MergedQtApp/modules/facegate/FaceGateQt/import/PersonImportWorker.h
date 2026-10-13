/**
 * @file PersonImportWorker.h
 * @brief 在后台线程串联 U 盘复制、XLS/XLSX 解析、本地人员和人脸导入。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#ifndef PERSON_IMPORT_WORKER_H
#define PERSON_IMPORT_WORKER_H

#include <QObject>
#include <QJsonArray>
#include <QStringList>
#include <atomic>

#include "common/sql/local_personnel_store.h"

/**
 * @brief 串联 U 盘复制、表格解析和数据库导入的后台任务对象。
 *
 * Excel 先复制到本机临时目录，人脸图片直接从U盘读取；
 * U盘生命周期由 udisk 服务统一管理，任务结束时仅释放本次导入预约。
 */
class PersonImportWorker : public QObject
{
    Q_OBJECT

public:
    /** @brief 创建尚未启动的人员导入任务。 */
    explicit PersonImportWorker(const QString &importRequestToken,
                                QObject *parent = nullptr);

    /** @brief 销毁前清理Excel临时副本并释放人员导入预约。 */
    ~PersonImportWorker() override;

    /** @brief 请求任务在下一个安全检查点取消。 */
    void cancel();

public slots:
    /** @brief 等待并复制人员表，解析后批量导入本地人员库。 */
    void run();

    /** @brief 接收推理线程的图片质量与特征校验结果并完成入库。 */
    void applyFaceValidation(const QString &token,
                             const QJsonArray &validatedFaces,
                             const QJsonArray &failedFaces);

signals:
    /** @brief 上报当前导入阶段的进度百分比和用户可读说明。 */
    void progress(int percent, const QString &message);

    /** @brief 请求人脸推理线程校验U盘图片。 */
    void faceValidationRequested(const QString &token, const QJsonArray &faces);

    /**
     * @brief 上报导入统计、跳过/失败名单和来源文件。
     *
     * @param ok  整体导入是否成功。
     * @param inserted  新增人员数。
     * @param updated  更新人员数。
     * @param unchanged  数据未变化的人员数。
     * @param failed  导入失败人员数。
     * @param unchangedNames  未发生变化的人员名称。
     * @param failedNames  导入失败的人员名称。
     * @param message  汇总说明或失败原因。
     * @param sourceFileName  U 盘中的原始文件名。
     */
    void finished(bool ok,
                  int inserted,
                  int updated,
                  int unchanged,
                  int failed,
                  const QStringList &unchangedNames,
                  const QStringList &failedNames,
                  const QString &message,
                  const QString &sourceFileName);

private:
    void complete(const QString &extraMessage = QString());
    QString cleanupResources();

    std::atomic_bool cancelled_{false}; /**< 跨线程取消标志。 */
    QString pendingToken_;
    QString importRequestToken_;
    QString temporaryRoot_;
    QString sourceFileName_;
    int inserted_ = 0;
    int updated_ = 0;
    int unchanged_ = 0;
    int failed_ = 0;
    int savedFaces_ = 0;
    QStringList unchangedNames_;
    QStringList failedNames_;
    QStringList parserWarnings_;
    QString importError_;
    LocalPersonnelStore store_;
    bool localImportLease_ = false; /**< 仅协调新增 Web 与 U 盘的并发导入。 */
};

#endif // PERSON_IMPORT_WORKER_H
