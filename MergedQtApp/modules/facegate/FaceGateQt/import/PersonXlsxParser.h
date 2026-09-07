/**
 * @file PersonXlsxParser.h
 * @brief 解析网络人员导出的 XLSX 文件，并提供 XLS/XLSX 共用的人员行映射。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#ifndef PERSON_XLSX_PARSER_H
#define PERSON_XLSX_PARSER_H

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

/** @brief 从人员表一行归一化得到的待导入记录。 */
struct PersonSpreadsheetRecord
{
    QString name;      /**< 人员姓名。 */
    QString account;   /**< 文件内应唯一的人员账号。 */
    int sourceRow = 0; /**< Excel 原始行号，从 1 开始。 */
    QJsonObject data;  /**< 供网络人员库导入的统一 JSON 数据。 */
};

/** @brief XLS/XLSX 人员表解析结果。 */
struct PersonXlsxParseResult
{
    bool ok = false;                            /**< 至少得到一条有效记录时为 true。 */
    QVector<PersonSpreadsheetRecord> records;   /**< 通过校验的人员记录。 */
    QStringList warnings;                       /**< 被跳过行等非致命提示。 */
    QString error;                              /**< 阻止导入的失败原因。 */
};

/** @brief 解包 XLSX 并把工作表行转换为统一人员记录。 */
class PersonXlsxParser
{
public:
    /**
     * @brief 解析指定 XLSX 文件。
     * @param filePath  XLSX 文件路径。
     * @return 统一人员记录、警告以及错误信息。
     */
    static PersonXlsxParseResult parse(const QString &filePath);

    /**
     * @brief 将任一 Excel 读取器产生的二维文本行转换成统一人员记录。
     * @param rows  第一行为表头的二维文本数据。
     * @param formatName  用于错误提示的来源格式名。
     * @return 通过表头和逐行校验后的解析结果。
     */
    static PersonXlsxParseResult parseRows(const QVector<QStringList> &rows,
                                           const QString &formatName);
};

#endif // PERSON_XLSX_PARSER_H
