/**
 * @file UsbPersonImportSource.h
 * @brief 检测、挂载并从 U 盘 person 目录复制人员 XLS/XLSX 文件。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#ifndef USB_PERSON_IMPORT_SOURCE_H
#define USB_PERSON_IMPORT_SOURCE_H

#include <QString>
#include <atomic>

/** @brief 从 U 盘复制到本机后的人员表信息。 */
struct UsbPersonImportFile
{
    bool ok = false;             /**< 复制和 U 盘卸载均成功时为 true。 */
    QString temporaryPath;       /**< 本机临时文件路径。 */
    QString sourceFileName;      /**< U 盘中的原始文件名。 */
    QString error;               /**< 查找、复制或卸载失败原因。 */
};

/** @brief 以只读方式读取 U 盘人员表并复制到本机临时目录。 */
class UsbPersonImportSource
{
public:
    /**
     * @brief 在超时前等待人员表，复制完成并卸载 U 盘后返回。
     * @param timeoutMs  最长等待时间，单位 ms。
     * @param cancelled  可选的跨线程取消标志。
     * @return 本机临时文件信息；失败时 error 说明原因。
     */
    static UsbPersonImportFile waitAndCopy(int timeoutMs,
                                           const std::atomic_bool *cancelled = nullptr);
};

#endif // USB_PERSON_IMPORT_SOURCE_H
