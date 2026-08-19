/**
 * @file UsbPersonImportSource.h
 * @brief 检测、挂载并从 U 盘 person 目录复制人员 XLS/XLSX 文件。
 */

#ifndef USB_PERSON_IMPORT_SOURCE_H
#define USB_PERSON_IMPORT_SOURCE_H

#include <QString>
#include <atomic>

struct UsbPersonImportFile
{
    bool ok = false;
    QString temporaryPath;
    QString sourceFileName;
    QString error;
};

class UsbPersonImportSource
{
public:
    static UsbPersonImportFile waitAndCopy(int timeoutMs,
                                           const std::atomic_bool *cancelled = nullptr);
};

#endif // USB_PERSON_IMPORT_SOURCE_H
