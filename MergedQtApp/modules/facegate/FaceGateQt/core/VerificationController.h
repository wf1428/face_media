/**
 * @file VerificationController.h
 * @brief 人脸门禁验证状态机。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef VERIFICATION_CONTROLLER_H
#define VERIFICATION_CONTROLLER_H

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>

#include "AppConfig.h"
#include "FaceEngine.h"
#include "LivenessWorker.h"
#include "VerificationTypes.h"

/**
 * @brief 人脸门禁验证状态机。
 *
 * 流程为待机、采集多帧、选择最佳帧、可选活体批次、身份匹配、结果保持和冷却。
 * 控制器不拥有 FaceEngine 或 LivenessWorker，且应在推理线程串行调用。
 */
class VerificationController : public QObject {
    Q_OBJECT

public:
    /** @brief 创建处于 Idle 状态的控制器。 */
    explicit VerificationController(QObject *parent = nullptr);

    /** @brief 更新采集帧数、阈值、结果保持和重复抑制配置。 */
    void configure(const AppConfig &config);

    /** @brief 注入不由控制器拥有的人脸引擎。 */
    void setFaceEngine(FaceEngine *engine);

    /** @brief 注入不由控制器拥有的活体 worker，并连接结果信号。 */
    void setLivenessWorker(LivenessWorker *worker);

    /** @brief 动态启用或跳过活体阶段。 */
    void setLivenessEnabled(bool enabled);

    /** @brief 清空采集和活体计数并返回 Idle。 */
    void resetToIdle(const QString &message = QString());

    /** @return 当前验证状态机状态。 */
    VerifyState state() const;

public slots:
    /** @brief 消费一帧，根据当前状态执行检测、采集或冷却判断。 */
    void onFrame(const CameraFrame &frame);

    /** @brief 累计活体结果并继续下一快照或进入身份匹配。 */
    void onLivenessResult(const LivenessResult &result);

signals:
    /** @brief 状态变化时发布状态和界面消息。 */
    void stateChanged(VerifyState state, const QString &message);

    /** @brief 发布当前帧的人脸框和匹配信息。 */
    void facesUpdated(const QVector<DetectedFace> &faces);

    /** @brief 发布一次最终通过日志。 */
    void verificationPassed(const VerifyLog &log);

    /** @brief 发布一次最终失败日志。 */
    void verificationFailed(const VerifyLog &log);

private:
    /** @brief 清空候选快照、最佳帧和活体批次计数。 */
    void resetCollection();

    /** @brief 为当前在场人脸启动一个新的限时识别窗口。 */
    void beginRecognitionAttempt();

    /** @brief 清空当前识别窗口累计的失败证据。 */
    void clearFailureEvidence();

    /** @brief 保存识别窗口内最有意义的失败结果，供超时时一次性发布。 */
    void rememberFailure(const VerificationSnapshot &snapshot,
                         const QString &message,
                         const QString &result,
                         int priority);

    /** @brief 放弃当前子流程并在同一个识别窗口内重新采集。 */
    void retryWithinRecognitionWindow(const QString &message);

    /** @brief 当前识别窗口到期后发布且只发布一次最终失败。 */
    void finishRecognitionTimeout();

    /** @return 当前 4 秒识别窗口是否已经到期。 */
    bool recognitionWindowExpired() const;

    /** @brief 设置状态并发出 stateChanged()。 */
    void enterState(VerifyState state, const QString &message);

    /** @brief 从最佳候选开始逐张提交活体检测。 */
    void startLivenessBatch();

    /** @brief 对最佳快照执行图库身份匹配。 */
    void finishIdentityMatching();

    /** @brief 生成最终日志、发布结果并进入结果保持状态。 */
    void finishVerification(bool passed, const QString &message, const QString &resultOverride = QString());

private:
    AppConfig config_;                   /**< 当前验证阈值和时序配置。 */
    FaceEngine *engine_ = nullptr;       /**< 人脸引擎，不拥有。 */
    LivenessWorker *liveness_ = nullptr; /**< 活体 worker，不拥有。 */
    bool livenessEnabled_ = true;        /**< 当前是否执行活体阶段。 */
    VerifyState state_ = VerifyState::Idle; /**< 当前状态。 */
    QVector<VerificationSnapshot> collected_; /**< 本轮采集的候选快照。 */
    VerificationSnapshot best_;         /**< 按 score() 选出的最佳快照。 */
    int nextLivenessIndex_ = 0;          /**< 下一张待活体检测候选索引。 */
    int livenessPassCount_ = 0;          /**< 本轮活体通过帧数。 */
    QElapsedTimer resultTimer_;          /**< 结果保持及冷却计时。 */
    QElapsedTimer recognitionTimer_;     /**< 当前单次识别窗口计时。 */
    VerificationSnapshot pendingFailureSnapshot_; /**< 超时失败使用的最佳证据帧。 */
    QString pendingFailureMessage_;      /**< 超时时发布的失败原因。 */
    QString pendingFailureResult_;       /**< 超时时发布的稳定失败结果码。 */
    int pendingFailurePriority_ = 0;     /**< 失败证据优先级，防止坏帧覆盖有效比对。 */
    bool pendingFailureSnapshotValid_ = false; /**< 是否已经保存失败证据帧。 */
};

#endif
