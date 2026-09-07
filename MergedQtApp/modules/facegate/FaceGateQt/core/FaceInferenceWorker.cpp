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

#include <cmath>

namespace {

/** @return 与本地重复录入判断一致的 float 特征余弦相似度。 */
float featureCosine(const QByteArray &left, const QByteArray &right)
{
    if (left.isEmpty() || left.size() != right.size()
            || left.size() % static_cast<int>(sizeof(float)) != 0) {
        return -1.0f;
    }
    const int count = left.size() / static_cast<int>(sizeof(float));
    const float *l = reinterpret_cast<const float *>(left.constData());
    const float *r = reinterpret_cast<const float *>(right.constData());
    double dot = 0.0;
    double leftNorm = 0.0;
    double rightNorm = 0.0;
    for (int index = 0; index < count; ++index) {
        dot += static_cast<double>(l[index]) * static_cast<double>(r[index]);
        leftNorm += static_cast<double>(l[index]) * static_cast<double>(l[index]);
        rightNorm += static_cast<double>(r[index]) * static_cast<double>(r[index]);
    }
    if (leftNorm <= 0.0 || rightNorm <= 0.0) {
        return -1.0f;
    }
    return static_cast<float>(dot / (std::sqrt(leftNorm) * std::sqrt(rightNorm)));
}

/** @brief 向 JSON 字符串数组追加非空且尚不存在的值。 */
void appendUniqueString(QJsonArray *values, const QString &value)
{
    if (!values || value.trimmed().isEmpty()) return;
    for (const QJsonValue &existing : *values) {
        if (existing.toString() == value) return;
    }
    values->append(value);
}

} // namespace

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

/**
 * @brief 在同一推理线程事件中替换本地和网络图库。
 *
 * 两份图库连续写入后再处理下一帧，避免识别看到只更新一半的合并快照。
 */
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

/**
 * @brief 对同步到本地的网络人员人脸图像逐张提取特征并校验重复项。
 *
 * 仅将图像可读、特征有效且人员内不重复的结果加入有效集合；失败项保留
 * 原始标识和原因，供同步层决定回报或重新拉取。
 */
void FaceInferenceWorker::validateSyncedFaceImages(
        const QString &token,
        const QString &personId,
        const QJsonArray &faces,
        const AppConfig &config)
{
    struct Candidate {
        QJsonObject face;
        QByteArray feature;
    };

    QJsonArray validatedFaces;
    QJsonArray failedFaces;
    QVector<Candidate> extractedCandidates;
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
        Candidate candidate;
        candidate.face = face;
        candidate.feature = feature.blob;
        extractedCandidates.append(candidate);
    }

    const float duplicateThreshold = qBound(
                0.0f, config.duplicateFaceCosineThreshold, 1.0f);
    QVector<Candidate> acceptedCandidates;
    for (Candidate candidate : extractedCandidates) {
        const QString currentHash = candidate.face.value(
                    QStringLiteral("faceHash")).toString().trimmed();

        // 不同网络 personId 之间禁止保存相似人脸；当前 personId 自己的旧人脸
        // 由本次最新下发的人脸替换。
        QJsonArray replacedNetworkHashes;
        QString conflictPersonId;
        QString conflictPersonName;
        float conflictCosine = -1.0f;
        for (const FaceRecord &record : networkGallery_) {
            const float cosine = featureCosine(
                        record.feature.blob, candidate.feature);
            if (cosine < duplicateThreshold) continue;

            const QString existingPersonId = record.person.personNo.trimmed();
            if (existingPersonId == normalizedPersonId) {
                if (record.faceHash.trimmed() != currentHash) {
                    appendUniqueString(&replacedNetworkHashes,
                                       record.faceHash.trimmed());
                }
            } else if (cosine > conflictCosine) {
                conflictCosine = cosine;
                conflictPersonId = existingPersonId;
                conflictPersonName = record.person.name.trimmed();
            }
        }
        if (!conflictPersonId.isEmpty()) {
            if (conflictPersonName.isEmpty()
                    || conflictPersonName == conflictPersonId) {
                conflictPersonName = QStringLiteral("其他网络人员");
            }
            QJsonObject failed = candidate.face;
            failed.insert(
                        QStringLiteral("message"),
                        QStringLiteral("人脸与网络人员“%1”重复，相似度=%2。")
                        .arg(conflictPersonName)
                        .arg(conflictCosine, 0, 'f', 3));
            failedFaces.append(failed);
            if (firstFailureMessage.isEmpty()) {
                firstFailureMessage = failed.value(
                            QStringLiteral("message")).toString();
            }
            continue;
        }

        // 同一次响应中按顺序处理；后出现的相似人脸视为最新项，替换之前项。
        for (int index = acceptedCandidates.size() - 1; index >= 0; --index) {
            const float cosine = featureCosine(
                        acceptedCandidates.at(index).feature,
                        candidate.feature);
            if (cosine < duplicateThreshold) continue;

            const QJsonObject previousFace = acceptedCandidates.at(index).face;
            const QString previousHash = previousFace.value(
                        QStringLiteral("faceHash")).toString().trimmed();
            if (previousHash != currentHash) {
                appendUniqueString(&replacedNetworkHashes, previousHash);
            }
            const QJsonArray previousReplacements = previousFace.value(
                        QStringLiteral("replaceNetworkFaceHashes")).toArray();
            for (const QJsonValue &value : previousReplacements) {
                appendUniqueString(&replacedNetworkHashes, value.toString());
            }
            acceptedCandidates.removeAt(index);
        }

        if (!replacedNetworkHashes.isEmpty()) {
            candidate.face.insert(QStringLiteral("replaceNetworkFaceHashes"),
                                  replacedNetworkHashes);
        }

        // 与本地人员相似时不拒绝网络同步，用最新网络特征替换相似度最高的
        // 本地特征；数据库提交后会统一重载本地与网络图库。
        qint64 localFeatureId = 0;
        float localBestCosine = -1.0f;
        QString localPersonNo;
        for (const FaceRecord &record : gallery_) {
            const float cosine = featureCosine(
                        record.feature.blob, candidate.feature);
            if (cosine >= duplicateThreshold && cosine > localBestCosine) {
                localFeatureId = record.featureId;
                localBestCosine = cosine;
                localPersonNo = record.person.personNo;
            }
        }
        if (localFeatureId > 0) {
            candidate.face.insert(QStringLiteral("replaceLocalFeatureId"),
                                  QString::number(localFeatureId));
            candidate.face.insert(QStringLiteral("replaceLocalPersonNo"),
                                  localPersonNo);
            candidate.face.insert(QStringLiteral("replaceLocalCosine"),
                                  localBestCosine);
        }
        candidate.face.insert(QStringLiteral("duplicateThreshold"),
                              duplicateThreshold);
        acceptedCandidates.append(candidate);
    }

    for (const Candidate &candidate : acceptedCandidates) {
        validatedFaces.append(candidate.face);
    }

    emit syncedFaceImagesValidated(
                token, normalizedPersonId, failedFaces.isEmpty(),
                failedFaces.isEmpty()
                    ? QStringLiteral("人脸图像同步成功。")
                    : firstFailureMessage,
                validatedFaces, failedFaces);
}
