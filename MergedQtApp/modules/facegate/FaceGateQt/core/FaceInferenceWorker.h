/**
 * @file FaceInferenceWorker.h
 * @brief 专用推理线程中的人脸引擎与验证状态机门面。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef FACE_INFERENCE_WORKER_H
#define FACE_INFERENCE_WORKER_H

#include <QJsonArray>
#include <QObject>

#include "AppConfig.h"
#include "VerificationTypes.h"

class FaceEngine;
class LivenessWorker;
class VerificationController;

/**
 * @brief 专用推理线程中的人脸引擎与验证状态机门面。
 *
 * 生命周期方法由主窗口阻塞排队调用，帧处理槽串行执行，确保 InspireFace 会话
 * 和 VerificationController 不被多个线程并发访问。
 */
class FaceInferenceWorker : public QObject {
    Q_OBJECT

public:
    /** @brief 创建尚未分配引擎和控制器的 worker。 */
    explicit FaceInferenceWorker(QObject *parent = nullptr);

    /** @brief 销毁前执行 shutdown()。 */
    ~FaceInferenceWorker() override;

    /**
     * @brief 初始化/恢复引擎和验证控制器，并注入活体 worker。
     * @note 由 MainWindow 通过 BlockingQueuedConnection 在推理线程执行。
     */
    bool activate(const AppConfig &config, LivenessWorker *livenessWorker, bool livenessEnabled);

    /** @brief 停止验证状态机但保留可再次激活的 worker 对象。 */
    void deactivate(const QString &message);

    /** @brief 释放引擎、控制器和线程内资源。 */
    void shutdown();

public slots:
    /** @brief 对一帧执行门禁验证；完成后发出 frameProcessed() 解除背压。 */
    void processVerificationFrame(const CameraFrame &frame);

    /** @brief 对管理员预览帧执行检测分析，不推进门禁验证状态机。 */
    void processAdminFrame(const CameraFrame &frame);

    /** @brief 同时替换本地与网络图库，避免识别帧看到半更新快照。 */
    void updateRecognitionGalleries(const QVector<FaceRecord> &localRecords,
                                    const QVector<FaceRecord> &networkRecords);

    /** @brief 更新引擎和验证控制器配置。 */
    void updateConfiguration(const AppConfig &config);

    /** @brief 动态切换活体检测开关。 */
    void setLivenessEnabled(bool enabled);

    /** @brief 清空当前验证上下文并发布指定状态消息。 */
    void resetVerification(const QString &message);

    /** @brief 从录入图片提取特征，并检查是否与现有图库重复。 */
    void extractEnrollmentFeature(const PersonInfo &person,
                                  const QString &imagePath,
                                  float duplicateThreshold);

    /** @brief 对服务器同步图片执行单人脸、检测置信度、质量和特征提取校验，不做活体检测。 */
    void validateSyncedFaceImages(const QString &token,
                                  const QString &personId,
                                  const QJsonArray &faces,
                                  const AppConfig &config);

signals:
    /** @brief 转发人脸引擎状态。 */
    void engineStatus(const QString &message);

    /** @brief 发布验证状态机状态和面向用户的消息。 */
    void stateChanged(VerifyState state, const QString &message);

    /** @brief 发布当前检测到的人脸列表。 */
    void facesUpdated(const QVector<DetectedFace> &faces);

    /** @brief 发布一次验证通过日志。 */
    void verificationPassed(const VerifyLog &log);

    /** @brief 发布一次验证失败日志。 */
    void verificationFailed(const VerifyLog &log);

    /** @brief 表示当前帧处理结束，允许主窗口投递下一帧。 */
    void frameProcessed();

    /** @brief 返回管理员页面所需的帧和检测分析。 */
    void adminAnalysisReady(const CameraFrame &frame, const FaceAnalysisResult &analysis);

    /** @brief 返回录入图片特征提取及重复人脸检查结果。 */
    void enrollmentFeatureReady(const PersonInfo &person,
                                const QString &imagePath,
                                bool ok,
                                const QString &errorText,
                                const FaceFeatureData &feature,
                                bool duplicate,
                                const FaceRecord &similarRecord,
                                float duplicateCosine);

    /** @brief 返回服务器同步图片的批量人脸质量校验结果。 */
    void syncedFaceImagesValidated(const QString &token,
                                   const QString &personId,
                                   bool ok,
                                   const QString &message,
                                   const QJsonArray &validatedFaces,
                                   const QJsonArray &failedFaces);

private:
    /** @brief 在当前线程延迟创建 FaceEngine 和 VerificationController 并连接信号。 */
    void ensureObjects();

private:
    FaceEngine *engine_ = nullptr;                  /**< 推理线程内拥有的人脸引擎。 */
    VerificationController *controller_ = nullptr; /**< 推理线程内拥有的验证状态机。 */
    LivenessWorker *livenessWorker_ = nullptr;      /**< 外部活体 worker，不拥有。 */
    QVector<FaceRecord> gallery_;                   /**< 本地离线图库快照。 */
    QVector<FaceRecord> networkGallery_;            /**< 网络人员图库快照。 */
    bool active_ = false;                           /**< 是否接受验证帧。 */
};

#endif
