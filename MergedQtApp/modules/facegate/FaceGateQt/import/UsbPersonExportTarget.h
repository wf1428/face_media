/**
 * @file UsbPersonExportTarget.h
 * @brief 检测并以可写方式挂载人员导出 U 盘，导出结束后负责同步和卸载。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#ifndef USB_PERSON_EXPORT_TARGET_H
#define USB_PERSON_EXPORT_TARGET_H

#include <QString>
#include <atomic>

/** @brief 已准备好用于人员导出的 U 盘卷及其挂载责任。 */
struct UsbPersonExportVolume
{
    bool ok = false;                    /**< person 目录可写时为 true。 */
    QString device;                     /**< U 盘块设备节点。 */
    QString mountPoint;                 /**< 当前挂载目录。 */
    QString personDirectory;            /**< 导出文件写入目录。 */
    bool mountedByApplication = false;  /**< 是否由本次任务创建挂载。 */
    QString error;                      /**< 准备失败原因。 */
};

/** @brief 负责查找、挂载、校验并安全卸载人员导出 U 盘。 */
class UsbPersonExportTarget
{
public:
    /**
     * @brief 在超时前等待可写 U 盘并准备 person 目录。
     * @param timeoutMs  最长等待时间，单位 ms。
     * @param cancelled  可选的跨线程取消标志。
     * @return 已准备的卷信息；失败时 error 说明原因。
     */
    static UsbPersonExportVolume waitForWritableVolume(
            int timeoutMs,
            const std::atomic_bool *cancelled = nullptr);

    /**
     * @brief 先同步介质写入，再卸载 U 盘并清理应用创建的挂载点。
     * @param volume  waitForWritableVolume() 返回的卷信息。
     * @param error  接收同步或卸载失败原因。
     * @return 同步和卸载均成功时返回 true。
     */
    static bool flushAndUnmount(const UsbPersonExportVolume &volume,
                                QString *error = nullptr);
};

#endif // USB_PERSON_EXPORT_TARGET_H
