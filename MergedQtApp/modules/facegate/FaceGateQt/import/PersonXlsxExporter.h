/**
 * @file PersonXlsxExporter.h
 * @brief 从本地、网络或全部人员库生成与导入模板一致的 XLSX 人员表。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#ifndef PERSON_XLSX_EXPORTER_H
#define PERSON_XLSX_EXPORTER_H

#include <QString>

/** @brief 人员导出的数据来源范围。 */
enum class PersonExportKind
{
    Local,   /**< FaceGate 本地离线人员。 */
    Network, /**< 网络人员库中的有效人员。 */
    All      /**< 网络人员与本地人员的合并结果。 */
};

/** @brief 一次 XLSX 人员导出的结果。 */
struct PersonXlsxExportResult
{
    bool ok = false;       /**< 文件成功提交到目标路径时为 true。 */
    int personCount = 0;   /**< 写入的人员记录数量。 */
    QString error;         /**< 失败原因；成功时为空。 */
};

/** @brief 把指定范围的人员数据映射为固定 31 列 XLSX 文件。 */
class PersonXlsxExporter
{
public:
    /**
     * @brief 查询人员数据并原子写入 XLSX 文件。
     * @param kind  需要导出的人员范围。
     * @param filePath  XLSX 目标路径。
     * @return 导出状态、记录数和失败原因。
     */
    static PersonXlsxExportResult exportToFile(PersonExportKind kind,
                                               const QString &filePath);

    /** @return 适合界面显示的人员范围名称。 */
    static QString kindName(PersonExportKind kind);
};

#endif // PERSON_XLSX_EXPORTER_H
