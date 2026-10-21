/**
 * @file UsbPersonExportTarget.h
 * @brief 预约人员导出U盘，等待脚本确认并在完成后同步、卸载。
 *
 * @author Dulin
 * @date 2026-08-28
 */

#ifndef USB_PERSON_EXPORT_TARGET_H
#define USB_PERSON_EXPORT_TARGET_H

#include <QString>
#include <atomic>

/** @brief 脚本已确认用于人员导出的U盘挂载点。 */
struct UsbPersonExportVolume
{
    bool ok = false;                    /**< Ycest/person 目录可写时为 true。 */
    QString mountPoint;                 /**< 当前挂载目录。 */
    QString personDirectory;            /**< 导出文件写入 Ycest/person 目录。 */
    QString error;                      /**< 准备失败原因。 */
};

/** @brief 与 udisk watcher 协调人员导出预约、挂载确认和安全卸载。 */
class UsbPersonExportTarget
{
public:
    /** @brief 应用启动、尚无导出任务时清除上次异常退出的导出标记。 */
    static void clearExportRequestOnStartup();
    /** @brief 在显示插盘提示前预约导出，避免脚本自动复制UI和视频。 */
    static QString beginExportRequest(QString *error = nullptr);
    /** @brief 仅释放当前令牌的导出预约和脚本确认文件。 */
    static void endExportRequest(const QString &requestToken);
    /**
     * @brief 在超时前等待可写 U 盘并准备 Ycest/person 目录。
     * @param timeoutMs  最长等待时间，单位 ms。
     * @param cancelled  可选的跨线程取消标志。
     * @param requestToken  显示插盘提示前取得的预约令牌。
     * @return 已准备的卷信息；失败时 error 说明原因。
     */
    static UsbPersonExportVolume waitForWritableVolume(
            int timeoutMs,
            const std::atomic_bool *cancelled,
            const QString &requestToken);

    /**
     * @brief 先同步介质写入，再卸载脚本确认的U盘挂载点。
     * @param volume  waitForWritableVolume() 返回的卷信息。
     * @param error  接收同步或卸载失败原因。
     * @return 同步和卸载均成功时返回 true。
     */
    static bool flushAndUnmount(const UsbPersonExportVolume &volume,
                                QString *error = nullptr);
};

#endif // USB_PERSON_EXPORT_TARGET_H
