/**
 * @file PersonXlsxExporter.h
 * @brief 从本地、网络或全部人员库生成与导入模板一致的 XLSX 人员表。
 */

#ifndef PERSON_XLSX_EXPORTER_H
#define PERSON_XLSX_EXPORTER_H

#include <QString>

enum class PersonExportKind
{
    Local,
    Network,
    All
};

struct PersonXlsxExportResult
{
    bool ok = false;
    int personCount = 0;
    QString error;
};

class PersonXlsxExporter
{
public:
    static PersonXlsxExportResult exportToFile(PersonExportKind kind,
                                               const QString &filePath);
    static QString kindName(PersonExportKind kind);
};

#endif // PERSON_XLSX_EXPORTER_H
