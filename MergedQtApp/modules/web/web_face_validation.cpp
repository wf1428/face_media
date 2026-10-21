/**
 * @file web_face_validation.cpp
 * @brief 新 Web 本地导入的跨库人脸防重；保留 U 盘和网络同步入口原有行为。
 * @author Du lin
 * @date 2026-10-09
 */

#include "modules/facegate/FaceGateQt/core/FaceInferenceWorker.h"
#include "modules/facegate/FaceGateQt/core/FaceEngine.h"
#include <QJsonObject>
#include "web_face_policy.h"

/** @brief 沿用 Qt 人脸推理流程校验 Web 批次图片。 */
void FaceInferenceWorker::validateWebImportFaceImages(const QString &token,
                                                      const QJsonArray &faces,
                                                      const AppConfig &config)
{
    QJsonArray extracted, failed, accepted;

    // 同步调用既有质量提取方法，仅捕获本次私有 token；不改变其原实现。
    const auto connection = connect(
        this,
        &FaceInferenceWorker::localImportFaceImagesValidated,
        this,
        [&extracted, &failed, &token](const QString &id,
                                      const QJsonArray &ok,
                                      const QJsonArray &bad) {
            if (id == token) {
                extracted = ok;
                failed = bad;
            }
        },
        Qt::DirectConnection);
    validateLocalImportFaceImages(token, faces, config);
    disconnect(connection);

    // 与网络人员下发校验使用同一配置项和 qBound 范围，不设置独立 Web 阈值。
    WebFacePolicy::filter(extracted,
                          gallery_,
                          networkGallery_,
                          config.duplicateFaceCosineThreshold,
                          &accepted,
                          &failed);
    emit webImportFaceImagesValidated(token, accepted, failed);
}
