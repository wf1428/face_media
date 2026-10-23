/** @file media_content_policy.h
 * @brief MQTT、本地播放和FTP共用的广告类型、图片格式及持久化选择。 */
#pragma once

#include <QDir>
#include <QFileInfo>
#include <QJsonValue>
#include <QSettings>
#include <QStringList>

namespace MediaContentPolicy {

/**
 * @brief 规范化广告类型，缺省、空值或非法值统一使用混播。
 * @param value 平台或网页传入的播放类型文本，允许空值及大小写差异。
 * @return VIDEO、IMAGE 或 VIDEO_IMAGE。
 */
inline QString normalizeContentType(const QString &value)
{
    const QString type = value.trimmed().toUpper();
    return type == QLatin1String("VIDEO") || type == QLatin1String("IMAGE")
            ? type : QStringLiteral("VIDEO_IMAGE");
}

/**
 * @brief 解析小图保存目录选择；不控制 pic 区域是否播放。
 * @param value 平台传入的 JSON 小图标记，转换为文本后仅 1 表示开启。
 * @return 值为 1 时返回 true，缺省或其他值返回 false。
 */
inline bool smallImageMode(const QJsonValue &value)
{
    return value.toVariant().toString().trimmed() == QLatin1String("1");
}

/**
 * @brief 按扩展名判断是否属于广告图片集合，不执行图片解码。
 * @param path 媒体文件路径，按扩展名判断类型。
 * @return 扩展名在图片集合中时为 true；不保证文件内容有效。
 */
inline bool isImage(const QString &path)
{
    static const QStringList suffixes = {"png", "jpg", "jpeg", "bmp", "gif", "webp", "tif", "tiff"};
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

/**
 * @brief 按扩展名判断是否属于广告视频集合，不探测实际编码。
 * @param path 媒体文件路径，按扩展名判断类型。
 * @return 扩展名在视频集合中时为 true；不保证编解码器可用。
 */
inline bool isVideo(const QString &path)
{
    static const QStringList suffixes = {
        "mp4", "avi", "mov", "mkv", "wmv", "flv", "ts", "pls", "webm", "rmvb",
        "3gp", "m4v", "mpg", "mpeg"
    };
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

/**
 * @brief 根据广告类型筛选 video 区域允许播放的媒体。
 * @param contentType VIDEO、IMAGE 或 VIDEO_IMAGE，按统一规则规范化。
 * @param path 媒体文件路径，按扩展名判断类型。
 * @return 媒体扩展名已识别且符合所选播放类型时为 true。
 */
inline bool allows(const QString &contentType, const QString &path)
{
    const QString type = normalizeContentType(contentType);
    return (isImage(path) && type != QLatin1String("VIDEO")) ||
           (isVideo(path) && type != QLatin1String("IMAGE"));
}

/**
 * @brief 读取与 MQTT 共用的持久化广告播放类型。
 * @param iniPath 广告播放配置 INI 的完整路径。
 * @return 规范化后的播放类型；缺省或非法值为 VIDEO_IMAGE。
 */
inline QString contentType(const QString &iniPath)
{
    QSettings ini(iniPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");
    return normalizeContentType(ini.value("playback/contentType").toString());
}

/**
 * @brief 解析小图保存目录选择；不控制 pic 区域是否播放。
 * @param iniPath 广告播放配置 INI 的完整路径。
 * @return 值为 1 时返回 true，缺省或其他值返回 false。
 */
inline bool smallImageMode(const QString &iniPath)
{
    QSettings ini(iniPath, QSettings::IniFormat);
    return ini.value("playback/smallImageMode", 0).toInt() == 1;
}

/**
 * @brief Web持久化与MQTT一致的配置字段，不改动MQTT保存入口。
 * @param iniPath 广告播放配置 INI 的完整路径。
 * @param type 待保存的广告播放类型。
 * @param small 图片是否保存到 pic 目录；不改变 pic 区域的配置开关。
 * @param[out] error 接收失败原因；允许为空时不写入错误文本。
 * @return 同步写入 INI 成功时为 true，失败时为 false。
 */
inline bool saveSelection(const QString &iniPath, const QString &type, bool small, QString *error = nullptr)
{
    QSettings ini(iniPath, QSettings::IniFormat);
    ini.setIniCodec("UTF-8");
    ini.setValue("playback/contentType", normalizeContentType(type));
    ini.setValue("playback/smallImageMode", small ? 1 : 0);
    ini.sync();
    if (ini.status() == QSettings::NoError) return true;
    if (error) *error = QStringLiteral("保存广告播放选择失败");
    return false;
}

/**
 * @brief 从 static 根目录构造独立 pic 图片目录。
 * @param staticRoot static 资源根目录。
 * @return demoResources/images/pic 的完整路径。
 */
inline QString pictureDir(const QString &staticRoot)
{
    return QDir(staticRoot).filePath(QStringLiteral("demoResources/images/pic"));
}

/**
 * @brief 比较文件的直接父目录，不递归匹配子目录。
 * @param file 待检查的文件路径。
 * @param directory 待扫描或比较的图片目录。
 * @return 清理后的绝对父目录相同时为 true。
 */
inline bool isInDirectory(const QString &file, const QString &directory)
{
    return QDir::cleanPath(QFileInfo(file).absolutePath()) ==
           QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
}

/**
 * @brief 枚举目录中可读的广告图片，名称不限。
 * @param directory 待扫描或比较的图片目录。
 * @return 按文件名忽略大小写排序的绝对路径列表；无图片时为空。
 */
inline QStringList imageFiles(const QString &directory)
{
    QStringList result;
    const QDir dir(directory);
    const auto files = dir.entryInfoList(QDir::Files | QDir::Readable,
                                         QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &file : files) {
        if (isImage(file.fileName())) result.append(file.absoluteFilePath());
    }
    return result;
}

} // namespace MediaContentPolicy
