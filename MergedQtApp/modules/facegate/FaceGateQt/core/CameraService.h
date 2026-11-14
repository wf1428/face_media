/**
 * @file CameraService.h
 * @brief 摄像头采集线程服务。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-05；修改人：Du lin；增加独立截图邮箱、按需图像转换及异步启停协调；识别邮箱保持独立，正常停用不等待采集退出。
 */

#ifndef CAMERA_SERVICE_H
#define CAMERA_SERVICE_H

#include <QObject>
#include <QImage>
#include <QString>

#include "CameraProfile.h"
#include "VerificationTypes.h"

#include <atomic>
#include <mutex>
#include <thread>

/**
 * @brief 摄像头采集线程服务。
 *
 * 采集线程通过后端等待、出队、RGA转换并归还缓冲；识别帧与截图均拥有独立像素寿命。
 * 识别邮箱保持原单消费者交接；截图使用独立try_lock邮箱，不领取识别帧。
 * 仅截图时不产生识别YUV和录入预览；正常需求切换不等待后端关闭，最终析构才join。
 */
class CameraService : public QObject {
    Q_OBJECT

public:
    /** @brief 创建尚未启动的采集服务。 */
    explicit CameraService(QObject *parent = nullptr);

    /** @brief 销毁前停止并等待采集线程退出。 */
    ~CameraService() override;

    /** @brief 使用兼容参数构造MIPI配置并启动采集线程。
     * @param[in] devicePath V4L2设备节点。
     * @param[in] width 请求宽度，单位像素；后端协商实际尺寸。
     * @param[in] height 请求高度，单位像素。
     * @param[in] pixelFormat NV12/NV21名称，按原后端规则处理。
     * @return true为已运行或已创建采集线程；打开失败异步通过cameraError报告。
     * @note 仅对象线程；常规切换使用requestRunning避免等待旧线程。
     */
    bool start(const QString &devicePath, int width, int height, const QString &pixelFormat);

    /** @brief 按完整配置创建采集线程；已运行时不改变设备参数。
     * @param[in] profile 自有配置快照；实际设备打开在采集线程。
     * @return true表示已运行或线程已创建，不代表硬件打开成功。
     * @note 仅对象线程；直接调用可能等待旧线程，正常需求切换应使用requestRunning。
     */
    bool start(const CameraProfile &profile);

    /** @brief 最终关闭时停止并等待线程退出，清理两类邮箱；正常切换使用requestRunning避免等待。 */
    void stop();

    /** @return 采集线程运行标志。 */
    bool isRunning() const;

    /**
     * @brief 取出最近一帧的副本并清除待投递标志。
     * @return 当前存在尚未领取的最新帧时返回 true。
     */
    bool takeLatestFrame(CameraFrame *frame);

    /** @brief 更新 UI/后台采集并集需求，正常停止不 join；仅对象线程调用。
     * @param[in] profile 下次启动使用的原相机配置，不热切换正在运行的相机。
     * @param[in] wanted true 请求采集；false 立即禁止采集循环并异步释放设备。
     * @param[in] recognition true 保留原识别帧转换/通知；false 不生成识别或录入预览。
     */
    void requestRunning(const CameraProfile &profile,bool wanted,bool recognition);
    /** @return 对象线程中最近一次请求是否需要相机，不代表硬件已打开成功。 */
    bool requestedRunning() const { return desiredRunning_; }
    /** @brief 更新独立截图通道；仅对象线程调用，false 清空待领帧并取消取帧请求。
     * @param[in] enabled true 允许按需产生截图帧；不影响识别邮箱。
     */
    void setScreenshotEnabled(bool enabled);
    /** @brief 有界领取一张截图，无帧时请求后续一张；不阻塞采集/识别，不返回驱动借用地址。
     * @param[out] image 已取得结果时写入自有RGB；空图表示截图转换失败，未就绪保持原值。
     * @param[out] ts 成功时写入采集 epoch 毫秒，失败保持原值。
     * @return true得到独立采集结果（可为空图失败）；false未启用、锁忙、参数为空或尚无结果。
     */
    bool takeScreenshot(QImage *image,qint64 *ts);

signals:
    /** @brief 最新帧从“无待领取”变为“有待领取”时通知消费者。 */
    void frameAvailable();

    /** @brief 上报采集打开或运行错误。 */
    void cameraError(const QString &message);

    /** @brief 上报后端格式、内存模式和非致命状态。 */
    void cameraStatus(const QString &message);

private:
    /** @brief 采集线程主循环，确保每个出队缓冲区最终重新入队。 */
    void captureLoop();
    /** @brief 对象线程协调快速停止/重启；仅已确认退出的线程才 join，等待期间用单个定时器续办。 */
    void reconcileCapture();

private:
    CameraProfile desiredProfile_; /**< 对象线程保存的下次启动配置。 */
    bool desiredRunning_=false,reconcilePending_=false; /**< 对象线程的需求及唯一重启检查标志。 */
    std::atomic_bool captureExited_{true}; /**< 采集入口所有返回路径置 true，允许对象线程无硬件等待地回收线程。 */
    std::atomic_bool recognitionEnabled_{true},screenshotEnabled_{false},screenshotRequested_{false}; /**< 独立消费者开关；截图仅在请求新帧时转换。 */
    std::mutex screenshotMutex_; /**< 只保护截图值；生产/消费均 try_lock，编码不持锁。 */
    QImage screenshotImage_; /**< 采集线程生成的自有 RGB 图像，不借用 V4L2/GStreamer 内存。 */
    qint64 screenshotTs_=0; /**< 截图采集epoch毫秒；0为无结果，正数且空图表示转换失败。 */
    CameraProfile profile_;                 /**< 当前采集配置。 */
    std::atomic_bool running_{false};       /**< 跨线程停止标志。 */
    std::thread captureThread_;             /**< 阻塞等待摄像头的后台线程。 */
    std::mutex frameMutex_;                 /**< 保护 latestFrame_ 和投递标志。 */
    CameraFrame latestFrame_;               /**< 最近完成标准化和预览转换的深拷贝帧。 */
    bool frameDeliveryPending_ = false;     /**< 是否已有未被消费者领取的帧。 */
};

#endif
