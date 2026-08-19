/**
 * @file UsbPersonExportTarget.h
 * @brief 检测并以可写方式挂载人员导出U盘，导出结束后负责同步和卸载。
 */

#ifndef USB_PERSON_EXPORT_TARGET_H
#define USB_PERSON_EXPORT_TARGET_H

#include <QString>
#include <atomic>

struct UsbPersonExportVolume
{
    bool ok = false;
    QString device;
    QString mountPoint;
    QString personDirectory;
    bool mountedByApplication = false;
    QString error;
};

class UsbPersonExportTarget
{
public:
    static UsbPersonExportVolume waitForWritableVolume(
            int timeoutMs,
            const std::atomic_bool *cancelled = nullptr);

    static bool flushAndUnmount(const UsbPersonExportVolume &volume,
                                QString *error = nullptr);
};

#endif // USB_PERSON_EXPORT_TARGET_H
