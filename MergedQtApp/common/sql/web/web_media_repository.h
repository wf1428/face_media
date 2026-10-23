/**
 * @file web_media_repository.h
 * @brief 视频文件索引仓储；目录真实文件为清单依据，兼容原 FTP 文件。
 * @author Du lin
 * @date 2026-10-09
 */

#pragma once

#include <QString>
#include <QVariantMap>

namespace WebMedia {

/**
 * @brief 登记正式文件，预存/FTP 文件可没有 uploadId。
 * @param path 待访问的文件或配置路径。
 * @param uploadId 关联上传任务标识；可空时表示目录扫描发现的文件。
 */
void index(const QString &path, const QString &uploadId = QString());

/**
 * @brief 计算媒体文件当前版本的标识。
 * @param path 待访问的文件或配置路径。
 * @return 基于路径、文件长度、修改时间及 Linux inode 的版本标识，防止过期列表误删同名替换文件。
 */
QString fileId(const QString &path);

/**
 * @brief 读取文件版本标识对应的媒体索引记录。
 * @param id 目标记录或任务的标识。
 * @return 文件索引行；路径仍须由服务检查，不信任浏览器文件名。
 */
QVariantMap get(const QString &id);

/**
 * @brief 先标记 DELETING，删除失败保留可重试状态。
 * @param id 目标记录或任务的标识。
 */
void deleting(const QString &id);

/**
 * @brief 文件实际消失后删除索引记录。
 * @param id 目标记录或任务的标识。
 */
void removed(const QString &id);
} // namespace WebMedia
