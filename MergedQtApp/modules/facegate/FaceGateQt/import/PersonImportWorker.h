/**
 * @file PersonImportWorker.h
 * @brief 在后台线程串联 U 盘复制、XLS/XLSX 解析和网络人员数据库导入。
 */

#ifndef PERSON_IMPORT_WORKER_H
#define PERSON_IMPORT_WORKER_H

#include <QObject>
#include <QStringList>
#include <atomic>

class PersonImportWorker : public QObject
{
    Q_OBJECT

public:
    explicit PersonImportWorker(QObject *parent = nullptr);
    void cancel();

public slots:
    void run();

signals:
    void progress(const QString &message);
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
    std::atomic_bool cancelled_{false};
};

#endif // PERSON_IMPORT_WORKER_H
