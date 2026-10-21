/**
 * @file UsbPersonImportSource.h
 * @brief 预约人员导入会话，等待 udisk 确认后读取 Ycest/person。
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
    bool ok = false;             /**< Excel 临时副本准备成功时为 true。 */
    QString temporaryPath;       /**< 本机临时文件路径。 */
    QString temporaryRoot;       /**< 本次导入使用的唯一临时目录。 */
    QString sourceImageDirectory; /**< U盘原始 img 目录，不复制图片。 */
    QString mountPoint;          /**< 由udisk服务确认、图片处理期间保持有效的挂载点。 */
    QString sourceFileName;      /**< U 盘中的原始文件名。 */
    QString error;               /**< 查找或复制失败原因。 */
};

/** @brief 从 /media/usb0/Ycest/person 读取人员表和 img 人脸目录。 */
class UsbPersonImportSource
{
public:
    /** @brief 仅在应用启动、尚无导入任务时清除上次遗留的预约及确认标记。 */
    static void clearImportRequestOnStartup();

    /**
     * @brief 在显示导入确认框前登记一次人员导入预约。
     * @return 成功时返回一次性令牌，失败时返回空字符串。
     */
    static QString beginImportRequest(QString *error = nullptr);

    /** @brief 结束指定人员导入预约；不会误删其他批次的请求。 */
    static void endImportRequest(const QString &requestToken);

    /**
     * @brief 等待 udisk 服务确认本次插盘专用于人员导入，再复制人员表。
     * @param timeoutMs  最长等待时间，单位 ms。
     * @param cancelled  可选的跨线程取消标志。
     * @param requestToken beginImportRequest 返回的一次性令牌。
     * @return 本机临时文件信息；失败时 error 说明原因。
     */
    static UsbPersonImportFile waitAndCopy(int timeoutMs,
                                           const std::atomic_bool *cancelled,
                                           const QString &requestToken);
};

#endif // USB_PERSON_IMPORT_SOURCE_H
