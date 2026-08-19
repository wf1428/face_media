/**
 * @file SnapshotService.h
 * @brief 验证抓拍文件命名和持久化服务。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef SNAPSHOT_SERVICE_H
#define SNAPSHOT_SERVICE_H

#include <QImage>
#include <QString>

#include "AppConfig.h"

/** @brief 验证抓拍文件命名和持久化服务。 */
class SnapshotService {
public:
    /** @brief 保存抓拍目录配置快照。 */
    explicit SnapshotService(const AppConfig &config);

    /**
     * @brief 按日期目录和人员/结果安全文件名保存验证图片。
     * @return 成功时返回图片路径，失败时返回空字符串并填写 errorText。
     */
    QString saveVerifySnapshot(const QImage &image,
                               const QString &personNo,
                               const QString &result,
                               QString *errorText = nullptr) const;

private:
    /** @return 基于应用目录解析并确保存在的抓拍根路径。 */
    QString snapshotRootPath() const;

    /** @return 移除路径非法字符后的安全文件名片段。 */
    QString safeFilePart(const QString &text) const;

private:
    AppConfig config_; /**< 抓拍根目录配置快照。 */
};

#endif
