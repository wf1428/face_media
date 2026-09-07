/**
 * @file PersonExportWorker.h
 * @brief 在后台线程完成 U 盘检测、人员查询、XLSX 生成和安全卸载。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#ifndef PERSON_EXPORT_WORKER_H
#define PERSON_EXPORT_WORKER_H

#include "PersonXlsxExporter.h"

#include <QObject>
#include <atomic>

/**
 * @brief 串联人员导出全流程的后台任务对象。
 *
 * 该对象应移动到专用线程中运行。cancel() 只设置原子取消标志，
 * U 盘轮询和正式导出阶段会在安全检查点读取该标志。
 */
class PersonExportWorker : public QObject
{
    Q_OBJECT

public:
    /** @brief 创建指定人员范围的导出任务。 */
    explicit PersonExportWorker(PersonExportKind kind, QObject *parent = nullptr);

public slots:
    /** @brief 等待可写 U 盘、生成 XLSX，并在完成前同步和卸载介质。 */
    void run();

    /** @brief 请求任务在下一个安全检查点取消。 */
    void cancel();

signals:
    /** @brief 上报当前导出阶段的用户可读说明。 */
    void progress(const QString &message);

    /**
     * @brief 上报导出结果以及已处理的人员数量和文件名。
     *
     * @param ok  文件写入、同步和卸载均成功时为 true。
     * @param personCount  实际写入人员记录数。
     * @param kindName  导出的人员范围名称。
     * @param fileName  U 盘 person 目录中的输出文件名。
     * @param message  完成说明或失败原因。
     */
    void finished(bool ok,
                  int personCount,
                  const QString &kindName,
                  const QString &fileName,
                  const QString &message);

private:
    PersonExportKind kind_ = PersonExportKind::Local; /**< 本次任务导出的人员范围。 */
    std::atomic_bool cancelled_{false}; /**< 跨线程取消标志。 */
};

#endif // PERSON_EXPORT_WORKER_H
