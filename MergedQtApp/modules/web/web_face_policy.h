/**
 * @file web_face_policy.h
 * @brief 新增本地导入防重策略，阈值由网络下发流程使用的 AppConfig 参数提供。
 * @author Du lin
 * @date 2026-10-09
 */

#pragma once

#include "modules/facegate/FaceGateQt/core/VerificationTypes.h"
#include <QJsonArray>

namespace WebFacePolicy {

/**
 * @brief 对本地/网络/同批特征防重；排除目标本地身份自身旧特征，拒绝模型不一致。
 * @param input 待校验的输入数据。
 * @param local 本地人员图库。
 * @param network 网络人员图库。
 * @param duplicateFaceCosineThreshold 余弦防重阈值，使用时限制在 [0, 1]。
 * @param[out] accepted 追加通过防重的人脸，调用方提供有效数组指针。
 * @param[out] failed 追加失败条目及原因，调用方提供有效数组指针。
 */
void filter(const QJsonArray &input,
            const QVector<FaceRecord> &local,
            const QVector<FaceRecord> &network,
            float duplicateFaceCosineThreshold,
            QJsonArray *accepted,
            QJsonArray *failed);

/**
 * @brief 在当前数据库工作线程读取最新有效图库，SQL 失败抛错而非当作空图库。
 * @param[out] local 输出本地图库，先清空再填入，不可为空。
 * @param[out] network 输出网络图库，先清空再填入，不可为空。
 */
void currentGalleries(QVector<FaceRecord> *local, QVector<FaceRecord> *network);
} // namespace WebFacePolicy
