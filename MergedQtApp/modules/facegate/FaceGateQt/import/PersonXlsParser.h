/**
 * @file PersonXlsParser.h
 * @brief 解析 Excel 97-2003 OLE/BIFF8 人员表。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#ifndef PERSON_XLS_PARSER_H
#define PERSON_XLS_PARSER_H

#include "PersonXlsxParser.h"

/** @brief 将 Excel 97-2003 OLE/BIFF8 人员表转换为统一导入记录。 */
class PersonXlsParser
{
public:
    /**
     * @brief 解析指定 XLS 文件。
     * @param filePath  Excel 97-2003 文件路径。
     * @return 统一人员记录、警告以及错误信息。
     */
    static PersonXlsxParseResult parse(const QString &filePath);
};

#endif // PERSON_XLS_PARSER_H
