/**
 * @file PersonXlsxParser.h
 * @brief 解析网络人员导出的 XLSX 文件，并提供 XLS/XLSX 共用的人员行映射。
 */

#ifndef PERSON_XLSX_PARSER_H
#define PERSON_XLSX_PARSER_H

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct PersonSpreadsheetRecord
{
    QString name;
    QString account;
    int sourceRow = 0;
    QJsonObject data;
};

struct PersonXlsxParseResult
{
    bool ok = false;
    QVector<PersonSpreadsheetRecord> records;
    QStringList warnings;
    QString error;
};

class PersonXlsxParser
{
public:
    static PersonXlsxParseResult parse(const QString &filePath);

    /** @brief 将任一 Excel 读取器产生的二维文本行转换成统一人员记录。 */
    static PersonXlsxParseResult parseRows(const QVector<QStringList> &rows,
                                           const QString &formatName);
};

#endif // PERSON_XLSX_PARSER_H
