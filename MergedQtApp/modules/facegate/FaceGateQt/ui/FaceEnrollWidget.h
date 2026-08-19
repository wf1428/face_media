/**
 * @file FaceEnrollWidget.h
 * @brief 人脸录入表单和手动/自动采集状态机。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef FACE_ENROLL_WIDGET_H
#define FACE_ENROLL_WIDGET_H

#include <QWidget>
#include <QImage>
#include <QVector>

#include "VerificationTypes.h"

class QFrame;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class EnrollPreviewWidget;

/**
 * @brief 人脸录入表单和手动/自动采集状态机。
 *
 * 自动模式要求检测质量、置信度及可选活体分达到配置阈值后才锁定图像；
 * 最终通过 enrollRequested() 把人员信息和图像交给后台提取特征。
 */
class FaceEnrollWidget : public QWidget {
    Q_OBJECT

public:
    /** @brief 创建人员表单、预览区和采集模式控件。 */
    explicit FaceEnrollWidget(QWidget *parent = nullptr);

    /** @brief 更新最新原图和录入预览图。 */
    void setCurrentFrame(const QImage &image, const QImage &previewImage);

    /** @brief 清除当前采集图像和自动采集状态。 */
    void clearCurrentFrame();

    /** @brief 更新检测分析、人脸框和提示消息。 */
    void setAnalysisFrame(const QImage &image,
                          const QImage &previewImage,
                          const QVector<DetectedFace> &faces,
                          const QString &message);

    /** @brief 更新活体功能开关及状态文本。 */
    void setLivenessStatus(bool enabled, const QString &message);

    /** @brief 更新最近一次活体结果，并评估是否满足自动采集。 */
    void setLivenessResult(bool enabled, const LivenessResult &result, const QString &message);

    /** @brief 设置自动录入所需的人脸质量、检测和活体阈值。 */
    void setAutoEnrollThresholds(float faceQuality, float faceDetect, float liveness);

    /** @brief 更新表单底部业务状态文本。 */
    void setStatusText(const QString &text);

    /** @brief 预填人员编号和姓名。 */
    void setPersonFields(const QString &personNo, const QString &name);

    /** @brief 强制切换到手动采集模式。 */
    void setManualCaptureMode();

    /** @return 当前录入页面需要持续摄像头帧时返回 true。 */
    bool cameraRequired() const;

signals:
    /** @brief 提交人员信息和已锁定录入图像。 */
    void enrollRequested(const PersonInfo &person, const QImage &image);

    /** @brief 手动/自动模式改变摄像头需求时发出。 */
    void cameraRequirementChanged();

private:
    /** @brief 校验人员字段和图像后发出录入请求。 */
    void requestEnroll();

    /** @brief 将当前图像、检测框和状态同步到预览控件。 */
    void updatePreview();

    /** @brief 根据检测、活体和模式更新预览提示。 */
    void updatePreviewStatus();

    /** @brief 切换到手动模式并重置自动锁定状态。 */
    void switchToManualMode();

    /** @brief 切换到自动模式并等待满足阈值的帧。 */
    void switchToAutoMode();

    /** @brief 根据当前模式重排预览和表单区域。 */
    void applyLayoutState();

    /** @brief 从内容布局移除旧项目，避免重复布局。 */
    void clearContentLayout();

    /** @brief 锁定满足自动条件的图像并更新界面。 */
    void acceptAutoCapture(const QImage &image);

private:
    QLineEdit *personNoEdit_ = nullptr;
    QLineEdit *nameEdit_ = nullptr;
    EnrollPreviewWidget *preview_ = nullptr;
    QLabel *previewTitle_ = nullptr;
    QLabel *previewStatusLabel_ = nullptr;
    QLabel *formTitle_ = nullptr;
    QLabel *hintLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QPushButton *captureButton_ = nullptr;
    QRadioButton *manualModeRadio_ = nullptr;
    QRadioButton *autoModeRadio_ = nullptr;
    QFrame *previewPanel_ = nullptr;
    QFrame *formPanel_ = nullptr;
    QWidget *contentHost_ = nullptr;
    QHBoxLayout *contentLayout_ = nullptr;
    QImage currentFrame_;
    QImage currentPreviewFrame_;
    QVector<DetectedFace> currentFaces_;
    QString detectionStatus_;
    QString livenessStatus_;
    QString statusText_;
    bool autoModeEnabled_ = false;              /**< 当前是否启用自动采集。 */
    bool autoCaptured_ = false;                 /**< 本轮是否已经锁定自动图像。 */
    float autoFaceQualityThreshold_ = 0.50f;    /**< 自动采集最低质量分。 */
    float autoFaceDetectThreshold_ = 0.70f;     /**< 自动采集最低检测置信度。 */
    float autoLivenessThreshold_ = 0.80f;       /**< 自动采集最低活体真实分。 */
};

#endif
