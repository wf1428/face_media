/**
 * @file web_paths.cpp
 * @brief Web 受控路径与文件校验实现，平台挂载检查仅在 Linux 生效。
 * @author Du lin
 * @date 2026-10-09
 */
#include "web_paths.h"
#include "common/sql/web/web_sql.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QtEndian>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

QString WebPaths::filename(const QString &name) {
    if (name.isEmpty() || name.size() > 180 || name.startsWith('.') || name != name.trimmed() || name.contains('/') || name.contains('\\') || name.contains(':'))
        throw WebError(400,QStringLiteral("文件名无效，不能包含目录"));
    for (QChar c : name) if (c.unicode() < 32 || c.unicode() == 127) throw WebError(400,QStringLiteral("文件名含控制字符"));
    return name;
}
bool WebPaths::accepts(const QString &kind, const QString &name) {
    const QString suffix = QFileInfo(name).suffix().toLower();
    if (kind == "video") return QStringList({"mp4","avi","mov","mkv","m4v","ts","webm"}).contains(suffix);
    if (kind == "person_excel") return suffix == "xls" || suffix == "xlsx";
    if (kind == "person_face") return QStringList({"jpg","jpeg","png","bmp"}).contains(suffix);
    return false;
}
void WebPaths::inside(const QString &path, const QString &root, bool mustExist) {
    const QFileInfo rootInfo(root), file(path);
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    const QString absolute = QDir::cleanPath(file.absoluteFilePath());
    const QString absoluteRoot = QDir::cleanPath(rootInfo.absoluteFilePath());
    if (canonicalRoot.isEmpty() || rootInfo.isSymLink() || canonicalRoot != absoluteRoot || !absolute.startsWith(absoluteRoot + '/'))
        throw WebError(400,QStringLiteral("文件路径越过受控目录"));
    const QString parent = QFileInfo(file.absolutePath()).canonicalFilePath();
    if (file.isSymLink() || (mustExist && !file.isFile()) || (parent != canonicalRoot && !parent.startsWith(canonicalRoot + '/')))
        throw WebError(400,QStringLiteral("文件目录不可用或存在符号链接"));
    if (file.exists() && !file.canonicalFilePath().startsWith(canonicalRoot + '/')) throw WebError(400,QStringLiteral("文件路径无效"));
}
void WebPaths::mounted(const QString &root, const QString &mountRoot) {
    QStorageInfo storage(root);
    storage.refresh();
    if (!QFileInfo(root).isDir() || !storage.isValid() || !storage.isReady() || storage.isReadOnly())
        throw WebError(503,QStringLiteral("视频存储未就绪"));
#ifdef Q_OS_LINUX
    if (QDir::cleanPath(storage.rootPath()) != QDir::cleanPath(mountRoot))
        throw WebError(503,QStringLiteral("视频存储未挂载，已阻止写入系统分区"));
#else
    Q_UNUSED(mountRoot);
#endif
}
void WebPaths::uploadDirectory(const QString &path) {
    const QFileInfo directory(path);
    if (!directory.isDir() || directory.isSymLink()) throw WebError(503,QStringLiteral("上传缓存目录不可用"));
#ifdef Q_OS_UNIX
    // Qt 可由现有 root 服务启动，daemon 使用 cat；新缓存目录需让继承的共享组写入。
    const auto permissions=QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadGroup|QFile::WriteGroup|QFile::ExeGroup;
    if (!QFile::setPermissions(path,permissions)) throw WebError(503,QStringLiteral("上传缓存目录共享权限设置失败"));
#endif
}
void WebPaths::sync(const QString &path, bool directory) {
#ifdef Q_OS_UNIX
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | (directory ? O_DIRECTORY : 0));
    if (fd < 0) throw WebError(503,QStringLiteral("无法打开文件进行同步"));
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    if (!ok) throw WebError(503,QStringLiteral("文件同步失败"));
#else
    Q_UNUSED(path); Q_UNUSED(directory);
#endif
}
qint64 WebPaths::usage(const QString &root) {
    qint64 bytes = 0;
    QDirIterator files(root,QDir::Files | QDir::NoSymLinks | QDir::Hidden,QDirIterator::Subdirectories);
    while (files.hasNext()) { files.next(); bytes += files.fileInfo().size(); }
    return bytes;
}
void WebPaths::validateExcel(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 20LL*1024*1024) throw WebError(400,QStringLiteral("Excel 无效或超过 20 MiB"));
    const QByteArray data = file.readAll();
    if (path.endsWith(".xls",Qt::CaseInsensitive)) {
        if (!data.startsWith(QByteArray::fromHex("d0cf11e0a1b11ae1"))) throw WebError(400,QStringLiteral("不是有效 XLS 文件"));
        return;
    }
    // 预检所有中央目录项，限制旧解析器按声明大小分配内存的预算。
    const int end = data.lastIndexOf(QByteArray::fromHex("504b0506"));
    if (end < 0 || end + 22 > data.size()) throw WebError(400,QStringLiteral("XLSX ZIP 目录无效"));
    const auto u16 = [&data](int i) { return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(data.constData()+i)); };
    const auto u32 = [&data](int i) { return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(data.constData()+i)); };
    const int count = u16(end+10);
    quint64 offset = u32(end+16), budget = 0;
    if (count == 0 || count > 2048) throw WebError(400,QStringLiteral("XLSX 目录项过多或为空"));
    for (int i=0; i<count; ++i) {
        if (offset+46 > quint64(data.size()) || u32(int(offset)) != 0x02014b50) throw WebError(400,QStringLiteral("XLSX 目录损坏"));
        const int at = int(offset);
        const quint32 expanded = u32(at+24), compressed = u32(at+20), local = u32(at+42);
        budget += expanded;
        if (expanded > 8*1024*1024 || budget > 32*1024*1024 || compressed > quint32(data.size()) || quint64(local)+30 > quint64(data.size()) || u32(int(local)) != 0x04034b50 || (u16(at+8)&1))
            throw WebError(400,QStringLiteral("XLSX 解压预算超限或文件加密"));
        const quint64 payload = quint64(local)+30+u16(int(local)+26)+u16(int(local)+28);
        if (payload+compressed > quint64(data.size())) throw WebError(400,QStringLiteral("XLSX 内容越界"));
        offset += 46+u16(at+28)+u16(at+30)+u16(at+32);
    }
}
