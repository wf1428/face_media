/**
 * @file web_face_validation.cpp
 * @brief Web 人脸结果桥接；与 U 盘共用本地质量和跨库相似度防重入口。
 * @author Du lin
 * @date 2026-10-09
 * @note 最近修改：2026-10-23；修改人：Du lin；防重收敛到本地校验入口，Web 仅转换结果信号，避免重复执行相同的图库比较。
 */

#include "modules/facegate/FaceGateQt/core/FaceInferenceWorker.h"
#include "modules/facegate/FaceGateQt/core/FaceEngine.h"
#include <QJsonObject>

/**
 * @brief 沿用 Qt 人脸推理流程校验 Web 批次图片。
 * @param token 贯穿请求与异步结果的任务标识。
 * @param faces 待校验的人脸条目，含人员及资源信息。
 * @param config 设备受控配置，包含运行所需路径或功能设置。
 */
void FaceInferenceWorker::validateWebImportFaceImages(const QString &token,
                                                      const QJsonArray &faces,
                                                      const AppConfig &config)
{
    QJsonArray extracted, failed;

    // 同步调用 U 盘与 Web 共用的质量及防重入口，仅捕获本次私有 token。
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

    emit webImportFaceImagesValidated(token, extracted, failed);
}
