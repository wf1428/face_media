/**
 * @file LivenessWorker.h
 * @brief MiniFASNet 双模型活体检测后台线程。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef LIVENESS_WORKER_H
#define LIVENESS_WORKER_H

#include <QObject>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "AppConfig.h"
#include "VerificationTypes.h"

/**
 * @brief MiniFASNet 双模型活体检测后台线程。
 *
 * submit() 采用“只保留最新请求”语义：worker 忙碌时新快照覆盖尚未处理的旧快照，
 * 防止实时摄像头输入造成无界队列和结果延迟。
 */
class LivenessWorker : public QObject {
    Q_OBJECT

public:
    /** @brief 创建尚未加载 RKNN 模型的 worker。 */
    explicit LivenessWorker(QObject *parent = nullptr);

    /** @brief 销毁前请求线程停止并等待退出。 */
    ~LivenessWorker() override;

    /** @brief 加载两个活体模型并启动工作线程。 */
    bool start(const AppConfig &config);

    /** @brief 更新活体阈值等运行配置。 */
    void updateConfig(const AppConfig &config);

    /** @brief 唤醒并停止工作线程，释放模型资源。 */
    void stop();

    /** @return 两个模型和工作线程已经就绪时返回 true。 */
    bool isReady() const;

    /** @brief 提交验证快照；已有待处理请求时直接替换。 */
    void submit(const VerificationSnapshot &snapshot);

signals:
    /** @brief 输出一张快照的双模型活体结果。 */
    void resultReady(const LivenessResult &result);

    /** @brief 上报模型加载、线程和推理状态。 */
    void workerStatus(const QString &message);

private:
    /** @brief 等待最新请求，执行推理并将结果投递回 Qt 事件系统。 */
    void workerLoop();

private:
    /** @brief RKNN 模型句柄与预处理实现。 */
    struct Impl;

    AppConfig config_;                  /**< 模型路径和通过阈值。 */
    std::unique_ptr<Impl> impl_;        /**< 双模型资源所有者。 */
    mutable std::mutex mutex_;          /**< 保护配置、请求和状态标志。 */
    std::condition_variable cv_;        /**< 新请求或停止请求的唤醒条件。 */
    VerificationSnapshot pending_;     /**< 仅保留的最新待处理快照。 */
    bool hasPending_ = false;           /**< pending_ 是否有效。 */
    bool stopRequested_ = false;        /**< 工作线程退出标志。 */
    bool ready_ = false;                /**< 模型和线程可接受任务标志。 */
    std::thread worker_;                /**< 阻塞执行 RKNN 推理的后台线程。 */
};

#endif
