/**
 * @file FaceEnrollWidget.cpp
 * @brief 人脸录入表单和手动/自动采集状态机的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "FaceEnrollWidget.h"
#include "EnrollPreviewWidget.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QLayoutItem>
#include <QPushButton>
#include <QRadioButton>
#include <QSizePolicy>
#include <QSize>
#include <QStringList>
#include <QtGlobal>
#include <QVBoxLayout>

namespace {
/** @brief 将人脸质量分数格式化为固定精度文本。 */
QString scoreText(float value)
{
    return value >= 0.0f ? QString::number(value, 'f', 2) : QStringLiteral("--");
}
}

/** @brief 创建人员表单、预览区和采集模式控件。 */
FaceEnrollWidget::FaceEnrollWidget(QWidget *parent) : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto *modeBar = new QFrame(this);
    modeBar->setObjectName("modeBar");
    auto *modeLayout = new QHBoxLayout(modeBar);
    modeLayout->setContentsMargins(10, 6, 10, 6);
    modeLayout->setSpacing(8);
    auto *modeTitle = new QLabel("采集方式", modeBar);
    modeTitle->setObjectName("modeTitle");
    manualModeRadio_ = new QRadioButton("手动采集", modeBar);
    autoModeRadio_ = new QRadioButton("自动采集", modeBar);
    manualModeRadio_->setChecked(true);
    modeLayout->addWidget(modeTitle);
    modeLayout->addSpacing(8);
    modeLayout->addWidget(manualModeRadio_);
    modeLayout->addWidget(autoModeRadio_);
    modeLayout->addStretch();

    contentHost_ = new QWidget(this);
    contentLayout_ = new QHBoxLayout(contentHost_);
    contentLayout_->setContentsMargins(0, 0, 0, 0);
    contentLayout_->setSpacing(8);

    previewPanel_ = new QFrame(contentHost_);
    previewPanel_->setObjectName("previewPanel");
    auto *previewLayout = new QVBoxLayout(previewPanel_);
    previewLayout->setContentsMargins(10, 10, 10, 10);
    previewLayout->setSpacing(6);

    previewTitle_ = new QLabel("实时采集", previewPanel_);
    previewTitle_->setObjectName("sectionTitle");
    preview_ = new EnrollPreviewWidget(previewPanel_);
    previewStatusLabel_ = new QLabel("自动采集中：请正视摄像头", previewPanel_);
    previewStatusLabel_->setObjectName("statusLabel");
    previewStatusLabel_->setWordWrap(true);
    previewStatusLabel_->setMinimumHeight(44);
    previewStatusLabel_->hide();

    previewLayout->addWidget(previewTitle_);
    previewLayout->addWidget(preview_, 1);
    previewLayout->addWidget(previewStatusLabel_);

    formPanel_ = new QFrame(contentHost_);
    formPanel_->setObjectName("formPanel");
    auto *formLayout = new QVBoxLayout(formPanel_);
    formLayout->setContentsMargins(10, 10, 10, 10);
    formLayout->setSpacing(8);

    formTitle_ = new QLabel("录入人员", formPanel_);
    formTitle_->setObjectName("sectionTitle");
    hintLabel_ = new QLabel("请正视摄像头", formPanel_);
    hintLabel_->setObjectName("formHint");
    hintLabel_->setWordWrap(true);

    personNoEdit_ = new QLineEdit(formPanel_);
    personNoEdit_->setObjectName("personNoEdit");
    personNoEdit_->setPlaceholderText("人员编号");
    personNoEdit_->setMinimumHeight(42);
    personNoEdit_->setAttribute(Qt::WA_InputMethodEnabled, true);
    personNoEdit_->setInputMethodHints(Qt::ImhNoPredictiveText);
    nameEdit_ = new QLineEdit(formPanel_);
    nameEdit_->setObjectName("nameEdit");
    nameEdit_->setPlaceholderText("姓名");
    nameEdit_->setMinimumHeight(42);
    nameEdit_->setAttribute(Qt::WA_InputMethodEnabled, true);
    nameEdit_->setInputMethodHints(Qt::ImhNoPredictiveText);

    captureButton_ = new QPushButton("保存人脸", formPanel_);
    captureButton_->setObjectName("saveButton");
    captureButton_->setIcon(QIcon(QStringLiteral(":/icons/action/save.svg")));
    captureButton_->setIconSize(QSize(18, 18));
    captureButton_->setMinimumHeight(44);
    connect(captureButton_, &QPushButton::clicked, this, &FaceEnrollWidget::requestEnroll);

    statusLabel_ = new QLabel("就绪", formPanel_);
    statusLabel_->setObjectName("statusLabel");
    statusLabel_->setWordWrap(true);
    statusLabel_->setMinimumHeight(42);

    formLayout->addWidget(formTitle_);
    formLayout->addWidget(hintLabel_);
    formLayout->addSpacing(6);
    formLayout->addWidget(personNoEdit_);
    formLayout->addWidget(nameEdit_);
    formLayout->addSpacing(6);
    formLayout->addWidget(captureButton_);
    formLayout->addWidget(statusLabel_);
    formLayout->addStretch();

    root->addWidget(modeBar);
    root->addWidget(contentHost_, 1);

    connect(manualModeRadio_, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) {
            switchToManualMode();
        }
    });
    connect(autoModeRadio_, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked) {
            switchToAutoMode();
        }
    });

    setStyleSheet(
        "QWidget{background:#162236;color:#edf5f8;font-family:'Microsoft YaHei';}"
        "#modeBar,#previewPanel,#formPanel{background:#1b2a40;border:1px solid #30445f;border-radius:18px;}"
        "#modeTitle{font-size:16px;font-weight:800;color:#f7fbff;}"
        "QRadioButton{font-size:15px;color:#d8e5eb;padding:4px 8px;}"
        "QRadioButton::indicator{width:18px;height:18px;border-radius:9px;border:1px solid #4a5e79;background:#111c2c;}"
        "QRadioButton::indicator:checked{background:#36d4c7;border:1px solid #36d4c7;}"
        "#sectionTitle{font-size:20px;font-weight:850;color:#f7fbff;}"
        "#formHint{font-size:13px;color:#9fb0c7;}"
        "#enrollPreview{background:#0b1320;color:#718590;border:1px solid #2d3c52;border-radius:14px;font-size:15px;}"
        "QLineEdit{background:#111c2c;color:#ffffff;border:1px solid #394b63;border-radius:12px;"
        "font-size:15px;padding:7px 10px;selection-background-color:#2b7fff;}"
        "QLineEdit:focus{border:1px solid #5eead4;background:#142236;}"
        "#saveButton{background:#2b7fff;color:#ffffff;border:1px solid #4c93ff;border-radius:12px;font-size:16px;font-weight:800;padding:8px;}"
        "#saveButton:hover{background:#3b8cff;}"
        "#statusLabel{background:#22324a;border:1px solid #31465f;border-radius:12px;color:#cfe0e8;font-size:13px;padding:8px;}"
    );

    statusText_ = "就绪";
    applyLayoutState();
}

/**
 * @brief 更新录入页面显示的实时原始截图。
 */
void FaceEnrollWidget::setCurrentFrame(const QImage &image, const QImage &previewImage)
{
    setAnalysisFrame(image, previewImage, QVector<DetectedFace>(), QString());
}

/** @brief 清除当前采集图像和自动采集状态。 */
void FaceEnrollWidget::clearCurrentFrame()
{
    if (autoModeEnabled_ && autoCaptured_) {
        return;
    }

    currentFrame_ = QImage();
    currentPreviewFrame_ = QImage();
    currentFaces_.clear();
    detectionStatus_.clear();
    livenessStatus_.clear();
    if (preview_) {
        preview_->clearFrame();
    }
    if (previewStatusLabel_) {
        previewStatusLabel_->setText(autoModeEnabled_ && !autoCaptured_
            ? QStringLiteral("自动采集中：请正视摄像头")
            : statusText_);
    }
}

/**
 * @brief 只用检测框更新录入预览，不携带身份匹配数据。
 */
void FaceEnrollWidget::setAnalysisFrame(const QImage &image,
                                        const QImage &previewImage,
                                        const QVector<DetectedFace> &faces,
                                        const QString &message)
{
    if (autoModeEnabled_ && autoCaptured_) {
        return;
    }

    currentFrame_ = image;
    currentPreviewFrame_ = previewImage;
    currentFaces_ = faces;
    detectionStatus_ = message;
    updatePreview();
}

/**
 * @brief 显示仅录入流程使用的当前活体预览状态。
 */
void FaceEnrollWidget::setLivenessStatus(bool enabled, const QString &message)
{
    livenessStatus_ = enabled ? message : QStringLiteral("活体关闭");
    if (autoModeEnabled_ && !autoCaptured_) {
        setStatusText(livenessStatus_);
    }
    updatePreviewStatus();
}

/**
 * @brief 接收活体检测结果，并在自动采集模式下判断是否已经得到合格人脸帧。
 */
void FaceEnrollWidget::setLivenessResult(bool enabled, const LivenessResult &result, const QString &message)
{
    livenessStatus_ = enabled ? message : QStringLiteral("活体关闭");

    if (autoModeEnabled_ && !autoCaptured_) {
        if (!enabled) {
            setStatusText("自动采集需要开启活体检测");
            updatePreviewStatus();
            return;
        }
        if (!result.valid) {
            setStatusText(result.message.isEmpty() ? QStringLiteral("自动采集中：等待活体结果") : result.message);
            updatePreviewStatus();
            return;
        }

        const DetectedFace &face = result.snapshot.face;
        const bool detectOk = face.detConfidence >= autoFaceDetectThreshold_;
        const bool qualityOk = face.qualityValid && face.quality >= autoFaceQualityThreshold_;
        const bool liveOk = result.passed && result.realScore >= autoLivenessThreshold_;
        if (detectOk && qualityOk && liveOk) {
            const QImage captured = result.snapshot.image.isNull() ? currentFrame_ : result.snapshot.image;
            acceptAutoCapture(captured);
            return;
        }

        const QString quality = face.qualityValid ? scoreText(face.quality) : QStringLiteral("未评估");
        if (!liveOk) {
            livenessStatus_.clear();
            setStatusText(QString("自动采集中：活体检测失败，真人分数 %1/%2，请重新正对摄像头")
                .arg(result.realScore, 0, 'f', 2)
                .arg(autoLivenessThreshold_, 0, 'f', 2));
            updatePreviewStatus();
            return;
        }

        setStatusText(QString("自动采集中：检测 %1/%2，质量 %3/%4，活体 %5/%6")
            .arg(scoreText(face.detConfidence))
            .arg(autoFaceDetectThreshold_, 0, 'f', 2)
            .arg(quality)
            .arg(autoFaceQualityThreshold_, 0, 'f', 2)
            .arg(result.realScore, 0, 'f', 2)
            .arg(autoLivenessThreshold_, 0, 'f', 2));
    }

    updatePreviewStatus();
}

/**
 * @brief 同步管理员设置页中的自动采集阈值。
 */
void FaceEnrollWidget::setAutoEnrollThresholds(float faceQuality, float faceDetect, float liveness)
{
    autoFaceQualityThreshold_ = qBound(0.0f, faceQuality, 1.0f);
    autoFaceDetectThreshold_ = qBound(0.0f, faceDetect, 1.0f);
    autoLivenessThreshold_ = qBound(0.0f, liveness, 1.0f);
}

/**
 * @brief 显示当前录入状态或校验错误。
 */
void FaceEnrollWidget::setStatusText(const QString &text)
{
    statusText_ = text;
    if (statusLabel_) {
        statusLabel_->setText(text);
    }
    if (previewStatusLabel_) {
        previewStatusLabel_->setText(text);
    }
}

/** @brief 预填人员编号和姓名。 */
void FaceEnrollWidget::setPersonFields(const QString &personNo, const QString &name)
{
    if (personNoEdit_) {
        personNoEdit_->setText(personNo);
    }
    if (nameEdit_) {
        nameEdit_->setText(name);
    }
}

/**
 * @brief 外部进入重新录入流程时，强制恢复“左侧预览、右侧信息”的手动录入布局。
 */
void FaceEnrollWidget::setManualCaptureMode()
{
    if (manualModeRadio_ && !manualModeRadio_->isChecked()) {
        manualModeRadio_->setChecked(true);
        return;
    }
    switchToManualMode();
}

/** @return 当前录入页面需要持续摄像头帧时返回 true。 */
bool FaceEnrollWidget::cameraRequired() const
{
    return !autoModeEnabled_ || !autoCaptured_;
}

/** @brief 将当前图像、检测框和状态同步到预览控件。 */
void FaceEnrollWidget::updatePreview()
{
    if (currentFrame_.isNull() || currentPreviewFrame_.isNull()) {
        preview_->clearFrame();
    } else {
        preview_->setFrame(currentPreviewFrame_, currentFrame_.size(), currentFaces_);
    }

    updatePreviewStatus();
}

/** @brief 根据检测、活体和模式更新预览提示。 */
void FaceEnrollWidget::updatePreviewStatus()
{
    QStringList parts;
    if (!detectionStatus_.isEmpty()) {
        parts << detectionStatus_;
    }
    if (!livenessStatus_.isEmpty()) {
        parts << livenessStatus_;
    }
    if (!parts.isEmpty() && statusText_ != QStringLiteral("正在保存人脸...") &&
        !autoModeEnabled_ && !autoCaptured_) {
        setStatusText(parts.join(" | "));
    }
}

/**
 * @brief 校验操作员输入，并携带当前帧发出保存请求。
 */
void FaceEnrollWidget::requestEnroll()
{
    PersonInfo person;
    person.personNo = personNoEdit_->text().trimmed();
    person.name = nameEdit_->text().trimmed();

    if (person.personNo.isEmpty() || person.name.isEmpty()) {
        setStatusText("人员编号和姓名不能为空");
        return;
    }

    if (currentFrame_.isNull()) {
        setStatusText("当前没有可用摄像头画面");
        return;
    }

    setStatusText("正在保存人脸...");
    emit enrollRequested(person, currentFrame_);
}

/** @brief 切换到手动模式并重置自动锁定状态。 */
void FaceEnrollWidget::switchToManualMode()
{
    autoModeEnabled_ = false;
    autoCaptured_ = false;
    detectionStatus_.clear();
    livenessStatus_.clear();
    setStatusText("就绪");
    applyLayoutState();
    updatePreview();
    emit cameraRequirementChanged();
}

/** @brief 切换到自动模式并等待满足阈值的帧。 */
void FaceEnrollWidget::switchToAutoMode()
{
    autoModeEnabled_ = true;
    autoCaptured_ = false;
    detectionStatus_.clear();
    livenessStatus_.clear();
    setStatusText("自动采集中：请正视摄像头，满足检测、质量、活体阈值后自动锁定画面");
    applyLayoutState();
    updatePreview();
    emit cameraRequirementChanged();
}

/** @brief 从内容布局移除旧项目，避免重复布局。 */
void FaceEnrollWidget::clearContentLayout()
{
    if (!contentLayout_) {
        return;
    }
    while (QLayoutItem *item = contentLayout_->takeAt(0)) {
        delete item;
    }
    if (previewPanel_) {
        previewPanel_->hide();
    }
    if (formPanel_) {
        formPanel_->hide();
    }
}

/** @brief 根据当前模式重排预览和表单区域。 */
void FaceEnrollWidget::applyLayoutState()
{
    clearContentLayout();

    if (!autoModeEnabled_) {
        previewTitle_->setText("实时采集");
        formTitle_->setText("录入人员");
        hintLabel_->setText("请正视摄像头");
        // 管理后台右侧实际可用宽度约为 828px；人员管理页和 Tab 页还有边距，
        // 手动录入必须使用固定窄表单，避免预览区把右侧人员信息挤出屏幕。
        formPanel_->setMinimumWidth(260);
        formPanel_->setMaximumWidth(260);
        formPanel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        previewPanel_->setMinimumWidth(0);
        previewPanel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        previewStatusLabel_->hide();
        previewPanel_->show();
        formPanel_->show();
        contentLayout_->addWidget(previewPanel_, 1);
        contentLayout_->addWidget(formPanel_);
        return;
    }

    if (!autoCaptured_) {
        previewTitle_->setText("自动采集预览");
        previewStatusLabel_->show();
        previewPanel_->show();
        formPanel_->hide();
        contentLayout_->addWidget(previewPanel_, 1);
        return;
    }

    formTitle_->setText("录入人员");
    hintLabel_->setText("自动采集已完成，请输入人员编号和姓名后保存，若需重新采集，请切换到手动采集后再切回自动采集");
    formPanel_->setMinimumWidth(320);
    formPanel_->setMaximumWidth(QWIDGETSIZE_MAX);
    formPanel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    previewStatusLabel_->hide();
    previewPanel_->hide();
    formPanel_->show();
    contentLayout_->addStretch(1);
    contentLayout_->addWidget(formPanel_, 3);
    contentLayout_->addStretch(1);
}

/** @brief 锁定满足自动条件的图像并更新界面。 */
void FaceEnrollWidget::acceptAutoCapture(const QImage &image)
{
    if (image.isNull()) {
        setStatusText("自动采集失败：没有可用画面");
        return;
    }

    currentFrame_ = image.copy();
    currentFaces_.clear();
    detectionStatus_.clear();
    livenessStatus_.clear();
    autoCaptured_ = true;
    setStatusText("自动采集成功，请输入人员编号和姓名后保存");
    applyLayoutState();
    emit cameraRequirementChanged();
    personNoEdit_->setFocus();
}
