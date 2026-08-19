/**
 * @file FaceInferenceWorker.cpp
 * @brief 专用推理线程中的人脸引擎与验证状态机门面的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "FaceInferenceWorker.h"

#include "FaceEngine.h"
#include "LivenessWorker.h"
#include "VerificationController.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QThread>

/** @brief 创建尚未分配引擎和控制器的 worker。 */
FaceInferenceWorker::FaceInferenceWorker(QObject *parent)
    : QObject(parent)
{
}

/** @brief 销毁前执行 shutdown()。 */
FaceInferenceWorker::~FaceInferenceWorker()
{
    if (active_ || (engine_ && engine_->isReady())) {
        shutdown();
    }
}

/** @brief 在当前线程延迟创建 FaceEngine 和 VerificationController 并连接信号。 */
void FaceInferenceWorker::ensureObjects()
{
    if (engine_) {
        return;
    }

    // 延迟到工作线程第一次激活时创建，使 QObject 亲缘关系和 SDK
    // 句柄的创建、调用、销毁全部归属于同一个线程。
    engine_ = new FaceEngine(this);
    controller_ = new VerificationController(this);
    controller_->setFaceEngine(engine_);

    connect(engine_, &FaceEngine::engineStatus,
            this, &FaceInferenceWorker::engineStatus);
    connect(controller_, &VerificationController::stateChanged,
            this, &FaceInferenceWorker::stateChanged);
    connect(controller_, &VerificationController::facesUpdated,
            this, &FaceInferenceWorker::facesUpdated);
    connect(controller_, &VerificationController::verificationPassed,
            this, &FaceInferenceWorker::verificationPassed);
    connect(controller_, &VerificationController::verificationFailed,
            this, &FaceInferenceWorker::verificationFailed);
}

/**
 * @brief 初始化/恢复引擎和验证控制器，并注入活体 worker。
 * @note 由 MainWindow 通过 BlockingQueuedConnection 在推理线程执行。
 */
bool FaceInferenceWorker::activate(const AppConfig &config,
                                   LivenessWorker *livenessWorker,
                                   bool livenessEnabled)
{
    ensureObjects();
    livenessWorker_ = livenessWorker;
    controller_->configure(config);
    controller_->setLivenessWorker(livenessWorker_);
    controller_->setLivenessEnabled(livenessEnabled);
    engine_->updateConfig(config);
    engine_->setGallery(gallery_);
    engine_->setNetworkGallery(networkGallery_);

    const bool ready = engine_->isReady() || engine_->initialize(config);
    active_ = ready;
    if (ready) {
        controller_->resetToIdle(QStringLiteral("等待人脸"));
        qInfo() << "[FACE-INFERENCE] worker activated"
                << "thread=" << QThread::currentThreadId();
    }
    return ready;
}

/** @brief 停止验证状态机但保留可再次激活的 worker 对象。 */
void FaceInferenceWorker::deactivate(const QString &message)
{
    active_ = false;
    if (controller_) {
        controller_->resetToIdle(message);
    }
}

/** @brief 释放引擎、控制器和线程内资源。 */
void FaceInferenceWorker::shutdown()
{
    active_ = false;
    if (controller_) {
        controller_->resetToIdle(QStringLiteral("人脸引擎已停止"));
        controller_->setLivenessWorker(nullptr);
    }
    if (engine_) {
        engine_->shutdown();
    }
    qInfo() << "[FACE-INFERENCE] worker shutdown"
            << "thread=" << QThread::currentThreadId();
}

/** @brief 对一帧执行门禁验证；完成后发出 frameProcessed() 解除背压。 */
void FaceInferenceWorker::processVerificationFrame(const CameraFrame &frame)
{
    if (active_ && controller_ && engine_ && engine_->isReady()) {
        controller_->onFrame(frame);
    }
    emit frameProcessed();
}

/** @brief 对管理员预览帧执行检测分析，不推进门禁验证状态机。 */
void FaceInferenceWorker::processAdminFrame(const CameraFrame &frame)
{
    FaceAnalysisResult analysis;
    if (active_ && engine_ && engine_->isReady()) {
        analysis = engine_->detectFrame(frame);
    } else {
        analysis.message = QStringLiteral("FaceEngine 未就绪");
    }

    emit adminAnalysisReady(frame, analysis);
    emit frameProcessed();
}

void FaceInferenceWorker::updateRecognitionGalleries(
        const QVector<FaceRecord> &localRecords,
        const QVector<FaceRecord> &networkRecords)
{
    gallery_ = localRecords;
    networkGallery_ = networkRecords;
    if (engine_) {
        // 同一识别线程事件内完成两份数据替换，下一帧只会看到完整合并快照。
        engine_->setGallery(gallery_);
        engine_->setNetworkGallery(networkGallery_);
    }
}

/** @brief 更新引擎和验证控制器配置。 */
void FaceInferenceWorker::updateConfiguration(const AppConfig &config)
{
    ensureObjects();
    controller_->configure(config);
    engine_->updateConfig(config);
}

/** @brief 动态切换活体检测开关。 */
void FaceInferenceWorker::setLivenessEnabled(bool enabled)
{
    if (controller_) {
        controller_->setLivenessEnabled(enabled);
    }
}

/** @brief 清空当前验证上下文并发布指定状态消息。 */
void FaceInferenceWorker::resetVerification(const QString &message)
{
    if (controller_) {
        controller_->resetToIdle(message);
    }
}

/** @brief 从录入图片提取特征，并检查是否与现有图库重复。 */
void FaceInferenceWorker::extractEnrollmentFeature(const PersonInfo &person,
                                                   const QString &imagePath,
                                                   float duplicateThreshold)
{
    FaceFeatureData feature;
    FaceRecord similarRecord;
    float duplicateCosine = -1.0f;
    QString errorText;

    if (!active_ || !engine_ || !engine_->isReady()) {
        errorText = QStringLiteral("FaceEngine 未就绪");
        emit enrollmentFeatureReady(person, imagePath, false, errorText, feature,
                                    false, similarRecord, duplicateCosine);
        return;
    }

    if (!engine_->extractFeatureFromImage(imagePath, feature, &errorText)) {
        emit enrollmentFeatureReady(person, imagePath, false, errorText, feature,
                                    false, similarRecord, duplicateCosine);
        return;
    }

    const bool duplicate = engine_->findSimilarFace(
        feature, duplicateThreshold, &similarRecord, &duplicateCosine);
    emit enrollmentFeatureReady(person, imagePath, true, QString(), feature,
                                duplicate, similarRecord, duplicateCosine);
}

void FaceInferenceWorker::validateSyncedFaceImages(
        const QString &token,
        const QString &personId,
        const QJsonArray &faces,
        const AppConfig &config)
{
    QJsonArray validatedFaces;
    QJsonArray failedFaces;
    const QString normalizedPersonId = personId.trimmed();
    if (token.trimmed().isEmpty() || normalizedPersonId.isEmpty()
            || faces.isEmpty()) {
        emit syncedFaceImagesValidated(
                    token, normalizedPersonId, false,
                    QStringLiteral("人脸图像校验参数不完整，请重新录入人脸。"),
                    validatedFaces, failedFaces);
        return;
    }

    ensureObjects();
    engine_->updateConfig(config);
    const bool ready = engine_->isReady() || engine_->initialize(config);
    if (!ready) {
        const QString message = QStringLiteral(
                    "人脸识别服务未就绪，请稍后重新录入人脸。");
        for (const QJsonValue &value : faces) {
            QJsonObject failed = value.toObject();
            failed.insert(QStringLiteral("message"), message);
            failedFaces.append(failed);
        }
        emit syncedFaceImagesValidated(
                    token, normalizedPersonId, false, message,
                    validatedFaces, failedFaces);
        return;
    }

    QString firstFailureMessage;
    for (const QJsonValue &value : faces) {
        const QJsonObject inputFace = value.toObject();
        const QString imagePath = inputFace.value(QStringLiteral("imagePath"))
                .toString().trimmed();
        FaceFeatureData feature;
        QString errorText;
        float detectConfidence = -1.0f;
        if (!engine_->extractFeatureFromImage(
                    imagePath, feature, &errorText, &detectConfidence)) {
            QString message = errorText.trimmed();
            if (message.contains(QStringLiteral("未检测到人脸"))) {
                message = QStringLiteral("未检测到人脸，请重新录入人脸。");
            } else if (message.contains(QStringLiteral("只保留一张人脸"))) {
                message = QStringLiteral("图像中检测到多张人脸，请重新录入单人脸图像。");
            } else if (message.contains(QStringLiteral("图片文件不存在"))) {
                message = QStringLiteral("人脸图像文件不存在，请重新录入人脸。");
            } else if (message.contains(QStringLiteral("HFFaceFeatureExtractTo"))
                       || message.contains(QStringLiteral("HFCreateFaceFeature"))) {
                message = QStringLiteral("人脸特征提取失败，请重新录入人脸。");
            } else if (message.isEmpty()) {
                message = QStringLiteral("人脸图像校验失败，请重新录入人脸。");
            }
            QJsonObject failed = inputFace;
            failed.insert(QStringLiteral("message"), message);
            failedFaces.append(failed);
            if (firstFailureMessage.isEmpty()) {
                firstFailureMessage = message;
            }
            continue;
        }

        QJsonObject face = inputFace;
        face.insert(QStringLiteral("featureBase64"),
                    QString::fromLatin1(feature.blob.toBase64()));
        face.insert(QStringLiteral("modelVersion"), feature.modelVersion);
        face.insert(QStringLiteral("faceQuality"), feature.quality);
        face.insert(QStringLiteral("recognitionQuality"), detectConfidence);
        validatedFaces.append(face);
    }

    emit syncedFaceImagesValidated(
                token, normalizedPersonId, failedFaces.isEmpty(),
                failedFaces.isEmpty()
                    ? QStringLiteral("人脸图像同步成功。")
                    : firstFailureMessage,
                validatedFaces, failedFaces);
}
