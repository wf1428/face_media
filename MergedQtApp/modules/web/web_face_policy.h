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
/** @brief 对本地/网络/同批特征防重；排除目标本地身份自身旧特征，拒绝模型不一致。 */
void filter(const QJsonArray &input,const QVector<FaceRecord> &local,const QVector<FaceRecord> &network,float duplicateFaceCosineThreshold,QJsonArray *accepted,QJsonArray *failed);
/** @brief 在当前数据库工作线程读取最新有效图库，SQL 失败抛错而非当作空图库。 */
void currentGalleries(QVector<FaceRecord> *local,QVector<FaceRecord> *network);
}
