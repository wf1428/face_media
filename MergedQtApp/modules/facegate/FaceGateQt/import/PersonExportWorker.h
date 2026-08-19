/**
 * @file PersonExportWorker.h
 * @brief 在后台线程完成U盘检测、人员查询、XLSX生成和安全卸载。
 */

#ifndef PERSON_EXPORT_WORKER_H
#define PERSON_EXPORT_WORKER_H

#include "PersonXlsxExporter.h"

#include <QObject>
#include <atomic>

class PersonExportWorker : public QObject
{
    Q_OBJECT

public:
    explicit PersonExportWorker(PersonExportKind kind, QObject *parent = nullptr);

public slots:
    void run();
    void cancel();

signals:
    void progress(const QString &message);
    void finished(bool ok,
                  int personCount,
                  const QString &kindName,
                  const QString &fileName,
                  const QString &message);

private:
    PersonExportKind kind_ = PersonExportKind::Local;
    std::atomic_bool cancelled_{false};
};

#endif // PERSON_EXPORT_WORKER_H
