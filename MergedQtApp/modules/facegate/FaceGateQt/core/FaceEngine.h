/**
 * @file FaceEngine.h
 * @brief InspireFace 检测、质量、特征提取和图库匹配封装。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef FACE_ENGINE_H
#define FACE_ENGINE_H

#include <QObject>

#include <memory>

#include "AppConfig.h"
#include "VerificationTypes.h"

/**
 * @brief InspireFace 检测、质量、特征提取和图库匹配封装。
 *
 * SDK 句柄隐藏在 Impl 中并只应从推理线程串行访问；图库保存为内存快照，
 * 匹配阈值和模型路径来自 AppConfig。
 */
class FaceEngine : public QObject {
    Q_OBJECT

public:
    /** @brief 创建尚未初始化 SDK 的引擎对象。 */
    explicit FaceEngine(QObject *parent = nullptr);

    /** @brief 销毁前释放 InspireFace 资源。 */
    ~FaceEngine() override;

    /** @brief 加载资源包并创建检测、识别会话。 */
    bool initialize(const AppConfig &config);

    /** @brief 更新不需要重建 SDK 会话的运行阈值配置。 */
    void updateConfig(const AppConfig &config);

    /** @brief 释放全部 SDK 会话和资源。 */
    void shutdown();

    /** @return SDK 初始化完成且会话可用时返回 true。 */
    bool isReady() const;

    /** @brief 原子替换用于 1:N 比对的本地离线人脸图库快照。 */
    void setGallery(const QVector<FaceRecord> &records);

    /** @brief 原子替换用于 1:N 比对的网络人员人脸图库快照。 */
    void setNetworkGallery(const QVector<FaceRecord> &records);

    /** @brief 对一帧执行检测、质量、特征提取和图库匹配。 */
    FaceAnalysisResult analyzeFrame(const CameraFrame &frame);

    /** @brief 仅执行人脸检测和基础属性分析。 */
    FaceAnalysisResult detectFrame(const CameraFrame &frame);

    /** @brief 在验证快照的多帧候选中查找可匹配人脸。 */
    bool matchSnapshot(const VerificationSnapshot &snapshot, DetectedFace *matchedFace, QString *errorText);

    /** @brief 从静态图片检测单人脸并提取特征。 */
    bool extractFeatureFromImage(const QString &imagePath,
                                 FaceFeatureData &feature,
                                 QString *errorText,
                                 float *detectConfidence = nullptr);

    /** @brief 在本地离线图库中查找余弦相似度达到 threshold 的最佳记录。 */
    bool findSimilarFace(const FaceFeatureData &feature, float threshold, FaceRecord *matchedRecord, float *bestCosine) const;

signals:
    /** @brief 上报 SDK 初始化、关闭和运行状态。 */
    void engineStatus(const QString &message);

private:
    /** @brief InspireFace SDK 句柄和实现细节。 */
    struct Impl;

    /** @brief 重建供每次识别统一遍历的本地+网络图库。 */
    void rebuildRecognitionGallery();

    AppConfig config_;                /**< 当前模型路径和识别阈值。 */
    QVector<FaceRecord> localGallery_;   /**< FaceGate 本地离线图库。 */
    QVector<FaceRecord> networkGallery_; /**< MQTT 网络人员图库。 */
    QVector<FaceRecord> gallery_;        /**< 本地和网络图库合并后的识别快照。 */
    std::unique_ptr<Impl> impl_;      /**< SDK 资源所有者。 */
    bool ready_ = false;              /**< SDK 会话可用标志。 */
};

#endif
