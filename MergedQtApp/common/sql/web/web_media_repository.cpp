/**
 * @file web_media_repository.cpp
 * @brief 媒体索引持久化，文件发布/删除由 Qt 服务协调。
 * @author Du lin
 * @date 2026-10-09
 */
#include "web_media_repository.h"
#include "web_sql.h"
#include <QFileInfo>
#include <QDateTime>
#include <QCryptographicHash>
#include <QFile>
#ifdef Q_OS_LINUX
#include <sys/stat.h>
#endif
QString WebMedia::fileId(const QString &path) {
    const QFileInfo file(path);
    if (!file.isFile() || file.isSymLink()) throw WebError(404,QStringLiteral("视频文件不存在或已替换"));
    QByteArray identity=path.toUtf8()+'\n'+QByteArray::number(file.size())+'\n'+QByteArray::number(file.lastModified().toMSecsSinceEpoch());
#ifdef Q_OS_LINUX
    struct stat state{};
    if (::lstat(QFile::encodeName(path).constData(),&state)!=0 || !S_ISREG(state.st_mode)) throw WebError(404,QStringLiteral("视频版本无法核对"));
    identity+='\n'+QByteArray::number(qulonglong(state.st_dev))+'\n'+QByteArray::number(qulonglong(state.st_ino))+'\n'+QByteArray::number(qlonglong(state.st_mtim.tv_sec))+'\n'+QByteArray::number(qlonglong(state.st_mtim.tv_nsec));
#endif
    return QString::fromLatin1(QCryptographicHash::hash(identity,QCryptographicHash::Sha256).toHex());
}
void WebMedia::index(const QString &path, const QString &uploadId) {
    const QFileInfo file(path);
    WebSql::execute(QStringLiteral("INSERT INTO web_media_files(id,upload_id,name,path,bytes,modified_ms) VALUES(?,?,?,?,?,?) ON CONFLICT(path) DO UPDATE SET state=CASE WHEN web_media_files.id<>excluded.id THEN 'READY' ELSE web_media_files.state END,id=excluded.id,upload_id=CASE WHEN web_media_files.id=excluded.id THEN COALESCE(excluded.upload_id,web_media_files.upload_id) ELSE excluded.upload_id END,bytes=excluded.bytes,modified_ms=excluded.modified_ms"),
                    {fileId(path),uploadId.isEmpty() ? QVariant() : QVariant(uploadId),file.fileName(),path,file.size(),file.lastModified().toMSecsSinceEpoch()});
}
QVariantMap WebMedia::get(const QString &id) {
    const auto rows = WebSql::query(QStringLiteral("SELECT * FROM web_media_files WHERE id=?"),{id});
    if (rows.isEmpty()) throw WebError(404,QStringLiteral("视频文件不存在"));
    if (fileId(rows.first().value("path").toString())!=id) throw WebError(409,QStringLiteral("视频已被替换，请刷新列表后操作"));
    return rows.first();
}
void WebMedia::deleting(const QString &id) { WebSql::execute(QStringLiteral("UPDATE web_media_files SET state='DELETING' WHERE id=?"),{id}); }
void WebMedia::removed(const QString &id) { WebSql::execute(QStringLiteral("DELETE FROM web_media_files WHERE id=?"),{id}); }
