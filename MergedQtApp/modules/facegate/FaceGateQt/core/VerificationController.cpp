/**
 * @file VerificationController.cpp
 * @brief 人脸门禁验证状态机的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "VerificationController.h"

#include <algorithm>

namespace {

/** @brief 为检测结果选择人员姓名、编号或默认陌生人文本。 */
QString displayNameForFace(const DetectedFace &face)
{
    if (!face.person.name.isEmpty()) {
        return face.person.name;
    }
    if (!face.person.personNo.isEmpty()) {
        return face.person.personNo;
    }
    return QString();
}

/** @brief 在原始帧副本上绘制人脸框和识别信息。 */
DetectedFace faceWithOverlay(DetectedFace face, const QString &status, const QString &text)
{
    face.overlayStatus = status;
    face.overlayText = text;
    return face;
}

/** @brief 只保留指定人脸并生成验证快照叠加图。 */
QVector<DetectedFace> singleFaceOverlay(const DetectedFace &face, const QString &status, const QString &text)
{
    QVector<DetectedFace> faces;
    faces.push_back(faceWithOverlay(face, status, text));
    return faces;
}

} // namespace

/** @brief 创建处于 Idle 状态的控制器。 */
VerificationController::VerificationController(QObject *parent) : QObject(parent)
{
}

/** @brief 更新采集帧数、阈值、结果保持和重复抑制配置。 */
void VerificationController::configure(const AppConfig &config)
{
    config_ = config;
    livenessEnabled_ = config.livenessEnabled;
}

/** @brief 注入不由控制器拥有的人脸引擎。 */
void VerificationController::setFaceEngine(FaceEngine *engine)
{
    engine_ = engine;
}

/** @brief 注入不由控制器拥有的活体 worker，并连接结果信号。 */
void VerificationController::setLivenessWorker(LivenessWorker *worker)
{
    if (liveness_ == worker) {
        return;
    }
    if (liveness_) {
        disconnect(liveness_, &LivenessWorker::resultReady,
                   this, &VerificationController::onLivenessResult);
    }
    liveness_ = worker;
    if (liveness_) {
        connect(liveness_, &LivenessWorker::resultReady, this, &VerificationController::onLivenessResult);
    }
}

/**
 * @brief 启用或绕过 MiniFASNet 活体检测，但不关闭人脸检测/识别。
 *
 * 关闭活体时，控制器仍会采集帧、选择最佳匹配人脸、
 * 写入日志，并基于身份识别结果开闸。
 */
void VerificationController::setLivenessEnabled(bool enabled)
{
    livenessEnabled_ = enabled;
    enterState(state_, enabled ? "活体检测已启用" : "活体检测已关闭");
}

/** @brief 清空采集和活体计数并返回 Idle。 */
void VerificationController::resetToIdle(const QString &message)
{
    resetCollection();
    resultTimer_.invalidate();
    enterState(VerifyState::Idle, message);
}

/** @return 当前验证状态机状态。 */
VerifyState VerificationController::state() const
{
    return state_;
}

/**
 * @brief 消费一帧拷贝后的摄像头图像，并推进验证状态机。
 *
 * 该帧同时携带 RGB 预览像素和原始连续 YUV420SP 数据。
 * FaceEngine 使用 YUV 数据以保留旧测试程序的 SDK 路径；活体检测
 * 通过后才会继续提取身份特征并进行底库比对。
 */
void VerificationController::onFrame(const CameraFrame &frame)
{
    if (!engine_) {
        enterState(VerifyState::Idle, QStringLiteral("缺少 FaceEngine"));
        return;
    }

    const bool holdingResult =
        state_ == VerifyState::Passed ||
        state_ == VerifyState::Failed ||
        state_ == VerifyState::Cooldown;

    if (holdingResult && resultTimer_.isValid() && resultTimer_.elapsed() >= config_.resultHoldMs) {
        resetCollection();
        enterState(VerifyState::Idle, QStringLiteral("等待人脸"));
    }

    /*
     * 主验证链路必须先做检测/质量评估，再做活体检测，活体通过后才做
     * 特征提取和底库相似度比较。这样非活体人员不会进入“人员未注册”
     * 或“验证失败”的身份分类。
     */
    FaceAnalysisResult analysis = engine_->detectFrame(frame);
    emit facesUpdated(analysis.faces);

    if (holdingResult && resultTimer_.isValid() && resultTimer_.elapsed() < config_.resultHoldMs) {
        return;
    }

    if (state_ == VerifyState::LivenessChecking ||
        state_ == VerifyState::SelectBestFrame ||
        state_ == VerifyState::FeatureMatching) {
        return;
    }

    if (analysis.faces.isEmpty()) {
        if (state_ != VerifyState::Idle) {
            resetCollection();
        }
        enterState(VerifyState::Idle, analysis.message.isEmpty() ? QStringLiteral("等待人脸") : analysis.message);
        return;
    }

    // 从当前帧选择质量和检测置信度综合最高的人脸。
    auto bestFaceIt = std::max_element(analysis.faces.begin(), analysis.faces.end(),
        [](const DetectedFace &a, const DetectedFace &b) {
            const float as = (a.qualityValid ? a.quality : 0.0f) + a.detConfidence;
            const float bs = (b.qualityValid ? b.quality : 0.0f) + b.detConfidence;
            return as < bs;
        });

    VerificationSnapshot snapshot;
    snapshot.image = frame.image;
    snapshot.yuv420sp = frame.yuv420sp;
    snapshot.pixelFormat = frame.pixelFormat;
    snapshot.width = frame.width;
    snapshot.height = frame.height;
    snapshot.nv21 = frame.nv21;
    snapshot.face = *bestFaceIt;
    snapshot.frameIndex = frame.frameIndex;

    const bool faceNotUsable =
        !snapshot.face.faceUsable ||
        snapshot.face.detConfidence < config_.faceDetectThreshold ||
        !snapshot.face.qualityValid ||
        snapshot.face.quality < config_.faceQualityThreshold;
    if (faceNotUsable) {
        resetCollection();
        best_ = snapshot;
        best_.face = faceWithOverlay(best_.face, QStringLiteral("failed"), QStringLiteral("验证失败"));
        emit facesUpdated(singleFaceOverlay(best_.face, QStringLiteral("failed"), QStringLiteral("验证失败")));
        finishVerification(false, QStringLiteral("验证失败，请正对摄像头"), QStringLiteral("failed"));
        return;
    }

    if (!livenessEnabled_) {
        resetCollection();
        best_ = snapshot;
        finishIdentityMatching();
        return;
    }

    if (state_ == VerifyState::Idle) {
        resetCollection();
        enterState(VerifyState::CollectingFrames, QStringLiteral("正在采集..."));
    }

    // 累积多帧检测结果，用于后续挑选最稳定的人脸帧做活体检测。
    collected_.push_back(snapshot);
    if (collected_.size() < config_.collectFrameCount) {
        enterState(VerifyState::CollectingFrames,
                   QStringLiteral("正在采集 %1/%2").arg(collected_.size()).arg(config_.collectFrameCount));
        return;
    }

    enterState(VerifyState::SelectBestFrame, QStringLiteral("正在选择最佳人脸帧"));
    best_ = *std::max_element(collected_.begin(), collected_.end(),
        [](const VerificationSnapshot &a, const VerificationSnapshot &b) { return a.score() < b.score(); });

    // 选出最佳帧后先进入活体检测，活体通过后才会进入身份识别。
    startLivenessBatch();
}

/** @brief 累计活体结果并继续下一快照或进入身份匹配。 */
void VerificationController::onLivenessResult(const LivenessResult &result)
{
    if (state_ != VerifyState::LivenessChecking) {
        return;
    }

    if (result.valid && result.passed) {
        ++livenessPassCount_;
    }

    if (nextLivenessIndex_ < collected_.size() && liveness_) {
        liveness_->submit(collected_[nextLivenessIndex_++]);
        return;
    }

    const bool livePassed = livenessPassCount_ >= config_.livenessPassRequired;
    if (!livePassed) {
        best_.face = faceWithOverlay(best_.face, QStringLiteral("liveness_failed"), QStringLiteral("活体失败"));
        emit facesUpdated(singleFaceOverlay(best_.face, QStringLiteral("liveness_failed"), QStringLiteral("活体失败")));
        finishVerification(false, QStringLiteral("活体检测失败"), QStringLiteral("liveness_failed"));
        return;
    }

    finishIdentityMatching();
}

/** @brief 清空候选快照、最佳帧和活体批次计数。 */
void VerificationController::resetCollection()
{
    collected_.clear();
    best_ = VerificationSnapshot();
    nextLivenessIndex_ = 0;
    livenessPassCount_ = 0;
}

/** @brief 设置状态并发出 stateChanged()。 */
void VerificationController::enterState(VerifyState state, const QString &message)
{
    if (state_ == state && message.isEmpty()) {
        return;
    }
    state_ = state;
    emit stateChanged(state_, message);
}

/** @brief 从最佳候选开始逐张提交活体检测。 */
void VerificationController::startLivenessBatch()
{
    if (!livenessEnabled_) {
        livenessPassCount_ = config_.livenessPassRequired;
        finishVerification(true, "验证通过");
        return;
    }

    if (!liveness_ || !liveness_->isReady()) {
        finishVerification(false, QStringLiteral("活体工作线程未就绪"), QStringLiteral("liveness_failed"));
        return;
    }

    nextLivenessIndex_ = 0;
    livenessPassCount_ = 0;
    enterState(VerifyState::LivenessChecking, "正在检测活体");
    liveness_->submit(collected_[nextLivenessIndex_++]);
}

/** @brief 对最佳快照执行图库身份匹配。 */
void VerificationController::finishIdentityMatching()
{
    enterState(VerifyState::FeatureMatching, QStringLiteral("正在识别身份"));

    if (!engine_) {
        finishVerification(false, QStringLiteral("验证失败，请正对摄像头"), QStringLiteral("failed"));
        return;
    }

    DetectedFace matchedFace;
    QString errorText;
    const bool matched = engine_->matchSnapshot(best_, &matchedFace, &errorText);
    best_.face = matchedFace;

    if (matched) {
        const QString text = displayNameForFace(matchedFace).isEmpty()
            ? QStringLiteral("验证通过")
            : displayNameForFace(matchedFace);
        best_.face = faceWithOverlay(matchedFace, QStringLiteral("passed"), text);
        emit facesUpdated(singleFaceOverlay(best_.face, QStringLiteral("passed"), text));
        finishVerification(true, QStringLiteral("验证通过"), QStringLiteral("passed"));
        return;
    }

    const bool faceNotUsable =
        !matchedFace.faceUsable ||
        matchedFace.detConfidence < config_.faceDetectThreshold ||
        !matchedFace.qualityValid ||
        matchedFace.quality < config_.faceQualityThreshold;

    const float similarButBelowThreshold = qMax(0.0f, config_.faceCosineThreshold - 0.20f);
    const bool hasSimilarCandidate =
        !matchedFace.galleryEmpty &&
        matchedFace.bestCandidateValid &&
        matchedFace.cosine >= similarButBelowThreshold;

    if (faceNotUsable || hasSimilarCandidate) {
        best_.face = faceWithOverlay(matchedFace, QStringLiteral("failed"), QStringLiteral("验证失败"));
        emit facesUpdated(singleFaceOverlay(best_.face, QStringLiteral("failed"), QStringLiteral("验证失败")));
        finishVerification(false, QStringLiteral("验证失败，请正对摄像头"), QStringLiteral("failed"));
        return;
    }

    Q_UNUSED(errorText)
    best_.face = faceWithOverlay(matchedFace, QStringLiteral("stranger"), QStringLiteral("未录入人员"));
    emit facesUpdated(singleFaceOverlay(best_.face, QStringLiteral("stranger"), QStringLiteral("未录入人员")));
    finishVerification(false, QStringLiteral("未录入人员"), QStringLiteral("stranger"));
}

/** @brief 生成最终日志、发布结果并进入结果保持状态。 */
void VerificationController::finishVerification(bool passed, const QString &message, const QString &resultOverride)
{
    // 生成验证日志，保持 result 字段为英文枚举以兼容数据库查询。
    VerifyLog log;
    log.personId = best_.face.person.id;
    log.personNo = best_.face.person.personNo;
    log.faceHash = best_.face.faceHash;
    log.nameSnapshot = best_.face.person.name;
    log.cosine = best_.face.cosine;
    log.liveScore = static_cast<float>(livenessPassCount_) /
                    static_cast<float>(std::max(1, config_.livenessPassRequired));
    log.result = passed ? QStringLiteral("passed") : (resultOverride.isEmpty() ? QStringLiteral("failed") : resultOverride);
    if (!passed) {
        log.failReason = message;
        if (resultOverride.isEmpty() && message.contains(QStringLiteral("活体"))) {
            log.result = QStringLiteral("liveness_failed");
        }
    }

    resultTimer_.restart();
    if (passed) {
        enterState(VerifyState::Passed, message);
        emit verificationPassed(log);
    } else {
        enterState(VerifyState::Failed, message);
        emit verificationFailed(log);
    }
}
