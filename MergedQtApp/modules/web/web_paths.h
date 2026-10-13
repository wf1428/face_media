/**
 * @file web_paths.h
 * @brief Web 文件名、目录边界、存储挂载及持久化辅助方法。
 * @author Du lin
 * @date 2026-10-09
 */
#pragma once
#include <QString>
namespace WebPaths {
/** @return 经检查的原始文件名；禁止目录、控制字符及隐藏名字。 */
QString filename(const QString &name);
/** @return 文件后缀对应上传类型是否允许。 */
bool accepts(const QString &kind, const QString &name);
/** @brief 检查已存在的受控根目录和父目录，不允许符号链接逃逸。 */
void inside(const QString &path, const QString &root, bool mustExist = false);
/** @brief 检查指定媒体存储确实挂载可写，禁止写进未挂载的根分区。 */
void mounted(const QString &mediaRoot, const QString &mountRoot);
/** @brief 新上传缓存目录赋予所属组写入权限；组由部署时父目录 setgid 继承，不修改原媒体文件。 */
void uploadDirectory(const QString &path);
/** @brief 同步普通文件数据及目录元数据；Linux 失败即报告，不假设 rename 等同 fsync。 */
void sync(const QString &path, bool directory = false);
/** @return 指定目录普通文件总字节数，不追随符号链接。 */
qint64 usage(const QString &root);
/** @brief Web 接收 Excel 的大小与 ZIP 解压预算检查，不修改原解析器。 */
void validateExcel(const QString &path);
}
