/**
 * @file UsbPersonExportTarget.h
 * @brief 等待固定U盘挂载点，准备导出目录并在完成后同步、卸载。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#ifndef USB_PERSON_EXPORT_TARGET_H
#define USB_PERSON_EXPORT_TARGET_H

#include <QString>
#include <atomic>

/** @brief 已准备好用于人员导出的固定U盘挂载点。 */
struct UsbPersonExportVolume
{
    bool ok = false;                    /**< Ycest/person 目录可写时为 true。 */
    QString mountPoint;                 /**< 当前挂载目录。 */
    QString personDirectory;            /**< 导出文件写入 Ycest/person 目录。 */
    QString error;                      /**< 准备失败原因。 */
};

/** @brief 负责校验并安全卸载 /media/usb0 人员导出U盘。 */
class UsbPersonExportTarget
{
public:
    /**
     * @brief 在超时前等待可写 U 盘并准备 Ycest/person 目录。
     * @param timeoutMs  最长等待时间，单位 ms。
     * @param cancelled  可选的跨线程取消标志。
     * @return 已准备的卷信息；失败时 error 说明原因。
     */
    static UsbPersonExportVolume waitForWritableVolume(
            int timeoutMs,
            const std::atomic_bool *cancelled = nullptr);

    /**
     * @brief 先同步介质写入，再卸载固定U盘挂载点。
     * @param volume  waitForWritableVolume() 返回的卷信息。
     * @param error  接收同步或卸载失败原因。
     * @return 同步和卸载均成功时返回 true。
     */
    static bool flushAndUnmount(const UsbPersonExportVolume &volume,
                                QString *error = nullptr);
};

#endif // USB_PERSON_EXPORT_TARGET_H
