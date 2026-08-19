/**
 * @file PersonXlsParser.h
 * @brief 解析 Excel 97-2003 OLE/BIFF8 人员表。
 */

#ifndef PERSON_XLS_PARSER_H
#define PERSON_XLS_PARSER_H

#include "PersonXlsxParser.h"

class PersonXlsParser
{
public:
    static PersonXlsxParseResult parse(const QString &filePath);
};

#endif // PERSON_XLS_PARSER_H
