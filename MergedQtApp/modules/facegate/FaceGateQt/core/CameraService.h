/**
 * @file CameraService.h
 * @brief 摄像头采集线程服务。
 *
 * @author Dulin
 * @date 2026-07-25
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
 * 采集线程通过后端等待、出队、RGA 标准化并重新入队缓冲区；最新帧以深拷贝形式
 * 保存在互斥锁下，UI/推理线程通过 takeLatestFrame() 取得副本。
 */
class CameraService : public QObject {
    Q_OBJECT

public:
    /** @brief 创建尚未启动的采集服务。 */
    explicit CameraService(QObject *parent = nullptr);

    /** @brief 销毁前停止并等待采集线程退出。 */
    ~CameraService() override;

    /** @brief 使用兼容参数构造 MIPI 配置并启动采集。 */
    bool start(const QString &devicePath, int width, int height, const QString &pixelFormat);

    /** @brief 按完整摄像头配置启动采集线程。 */
    bool start(const CameraProfile &profile);

    /** @brief 请求采集线程停止并等待退出。 */
    void stop();

    /** @return 采集线程运行标志。 */
    bool isRunning() const;

    /**
     * @brief 取出最近一帧的副本并清除待投递标志。
     * @return 当前存在尚未领取的最新帧时返回 true。
     */
    bool takeLatestFrame(CameraFrame *frame);

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

private:
    CameraProfile profile_;                 /**< 当前采集配置。 */
    std::atomic_bool running_{false};       /**< 跨线程停止标志。 */
    std::thread captureThread_;             /**< 阻塞等待摄像头的后台线程。 */
    std::mutex frameMutex_;                 /**< 保护 latestFrame_ 和投递标志。 */
    CameraFrame latestFrame_;               /**< 最近完成标准化和预览转换的深拷贝帧。 */
    bool frameDeliveryPending_ = false;     /**< 是否已有未被消费者领取的帧。 */
};

#endif
