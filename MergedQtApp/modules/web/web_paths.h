/**
 * @file web_paths.h
 * @brief Web 文件名、目录边界、存储可用性及持久化辅助方法。
 * @author Du lin
 * @date 2026-10-09
 * @note 最近修改：2026-10-23；修改人：Du lin；新增人脸图片统一格式校验接口，供直接上传和压缩包展开图片共同使用。
 */

#pragma once

#include <QString>

namespace WebPaths {

/**
 * @brief 校验单一文件名，拒绝目录、隐藏名称和控制字符。
 * @param name 待处理的名称。
 * @return 经检查的原始文件名；禁止目录、控制字符及隐藏名字。
 */
QString filename(const QString &name);

/**
 * @brief 检查上传类型是否允许目标文件的扩展名。
 * @param kind 资源或任务类型，用于选择目录、配额和校验规则。
 * @param name 待处理的名称。
 * @return 文件后缀对应上传类型是否允许。
 */
bool accepts(const QString &kind, const QString &name);

/**
 * @brief 核对人脸图片真实格式、扩展名、像素数及单文件长度。
 * @param path 受控临时文件的完整路径。
 * @param name 原始图片名称，支持 JPG、JPEG、PNG、BMP。
 * @throws WebError 图片超过 10 MiB、1600 万像素或格式无效时拒绝。
 */
void validateFaceImage(const QString &path, const QString &name);

/**
 * @brief 检查已存在的受控根目录和父目录，不允许符号链接逃逸。
 * @param path 待访问的文件或配置路径。
 * @param root 受控根目录。
 * @param mustExist 为 true 时要求目标已存在。
 */
void inside(const QString &path, const QString &root, bool mustExist = false);

/**
 * @brief 检查媒体目录及所在文件系统可用可写；mountRoot 仅保留部署兼容，不要求独立挂载。
 * @param mediaRoot 实际媒体目录。
 * @param mountRoot 旧挂载点配置，保留兼容用途。
 */
void mounted(const QString &mediaRoot, const QString &mountRoot);

/**
 * @brief 新上传缓存目录赋予所属组写入权限；组由部署时父目录 setgid 继承，不修改原媒体文件。
 * @param path 待访问的文件或配置路径。
 */
void uploadDirectory(const QString &path);

/**
 * @brief 同步普通文件数据及目录元数据；Linux 失败即报告，不假设 rename 等同 fsync。
 * @param path 待访问的文件或配置路径。
 * @param directory 为 true 时按目录执行同步，否则按文件同步。
 */
void sync(const QString &path, bool directory = false);

/**
 * @brief 递归统计目录普通文件的大小，跳过符号链接。
 * @param root 受控根目录。
 * @return 指定目录普通文件总字节数，不追随符号链接。
 */
qint64 usage(const QString &root);

/**
 * @brief Web 接收 Excel 的大小与 ZIP 解压预算检查，不修改原解析器。
 * @param path 待访问的文件或配置路径。
 */
void validateExcel(const QString &path);
} // namespace WebPaths
