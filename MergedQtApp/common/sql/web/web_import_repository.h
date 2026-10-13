/**
 * @file web_import_repository.h
 * @brief Excel/人脸批次与原始文件名清单的独立仓储。
 * @author Du lin
 * @date 2026-10-09
 */
#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QVariantMap>
namespace WebImports {
/** @return 创建的开放批次 UUID。 */
QString create(int owner, const QJsonArray &manifest);
/** @return 指定用户的批次，包含解析后的 result 对象。 */
QJsonObject get(const QString &id, int owner = 0);
/** @brief 保存批次状态和部分成功统计，不自动重放不确定的导入。 */
void setState(const QString &id, const QString &state, const QJsonObject &result = {});
/** @brief 注册上传完成资产，原始文件名用于与 Excel 账号匹配。 */
void addAsset(const QVariantMap &task);
/** @return 指定批次资产，按原始名称排序以保持 U 盘扫描顺序。 */
QList<QVariantMap> assets(const QString &id);
/** @brief 将重启前正在写人员/人脸的批次标记为 RECOVERY_REQUIRED，禁止重复入库。 */
void recover();
}
