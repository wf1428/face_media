/**
 * @file PersonImportWorker.h
 * @brief 在后台线程串联 U 盘复制、XLS/XLSX 解析和网络人员数据库导入。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#ifndef PERSON_IMPORT_WORKER_H
#define PERSON_IMPORT_WORKER_H

#include <QObject>
#include <QStringList>
#include <atomic>

/**
 * @brief 串联 U 盘复制、表格解析和数据库导入的后台任务对象。
 *
 * U 盘文件先复制到本机临时目录并卸载介质，后续解析和入库不再依赖 U 盘。
 */
class PersonImportWorker : public QObject
{
    Q_OBJECT

public:
    /** @brief 创建尚未启动的人员导入任务。 */
    explicit PersonImportWorker(QObject *parent = nullptr);

    /** @brief 请求任务在下一个安全检查点取消。 */
    void cancel();

public slots:
    /** @brief 等待并复制人员表，解析后批量导入网络人员库。 */
    void run();

signals:
    /** @brief 上报当前导入阶段的用户可读说明。 */
    void progress(const QString &message);

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
    std::atomic_bool cancelled_{false}; /**< 跨线程取消标志。 */
};

#endif // PERSON_IMPORT_WORKER_H
