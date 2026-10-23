/**
 * @file web_resource_update.cpp
 * @brief libarchive 流式读取资源包，先暂存、后逐目录替换，异常回滚。
 * @note 最近修改：2026-10-23；修改人：Du lin；复用归档读取器展开人员图片包，允许任意目录名并保留原资源更新规则。
 * @author Du lin
 * @date 2026-10-20
 */
#include "web_resource_update.h"
#include "web_paths.h"
#include "common/sql/web/web_sql.h"
#include <archive.h>
#include <archive_entry.h>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLibrary>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <memory>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#ifdef Q_OS_LINUX
#include <locale.h>
#endif

namespace {
constexpr qint64 MiB = 1024LL * 1024;
constexpr qint64 ExpandedLimit = 2048 * MiB;
const QString Work = QStringLiteral(".ycest-update");

/**
 * @brief 保持异常格式与原 Web 业务一致。
 * @param message 可返回调用方的结果或错误说明。
 * @param code 业务状态码，成功通常为 200。
 */
void fail(const QString &message, int code = 400)
{
    throw WebError(code, message);
}

/**
 * @brief 在安全检查点响应停止；回滚阶段不会因停止请求中断。
 */
void interrupted()
{
    if (QThread::currentThread()->isInterruptionRequested())
        fail(QStringLiteral("资源更新已中止，未提交的目录将恢复"));
}

/** @brief 按需加载 C 接口，解压库缺失不会阻止不使用 Web 更新的 Qt 启动。 */
class ArchiveApi {
public:

    QLibrary library;
#define ARCHIVE_FUNCTION(name) decltype(&::name) name = nullptr;
    ARCHIVE_FUNCTION(archive_read_new)
    ARCHIVE_FUNCTION(archive_read_free)
    ARCHIVE_FUNCTION(archive_read_support_filter_none)
    ARCHIVE_FUNCTION(archive_read_support_filter_gzip)
    ARCHIVE_FUNCTION(archive_read_support_filter_bzip2)
    ARCHIVE_FUNCTION(archive_read_support_filter_xz)
    ARCHIVE_FUNCTION(archive_read_support_format_tar)
    ARCHIVE_FUNCTION(archive_read_support_format_zip)
    ARCHIVE_FUNCTION(archive_read_support_format_rar)
    ARCHIVE_FUNCTION(archive_read_support_format_rar5)
    ARCHIVE_FUNCTION(archive_read_open)
    ARCHIVE_FUNCTION(archive_read_set_seek_callback)
    ARCHIVE_FUNCTION(archive_read_next_header)
    ARCHIVE_FUNCTION(archive_read_data)
    ARCHIVE_FUNCTION(archive_read_data_skip)
    ARCHIVE_FUNCTION(archive_error_string)
    ARCHIVE_FUNCTION(archive_entry_pathname_utf8)
    ARCHIVE_FUNCTION(archive_entry_filetype)
    ARCHIVE_FUNCTION(archive_entry_symlink)
    ARCHIVE_FUNCTION(archive_entry_hardlink)
    ARCHIVE_FUNCTION(archive_entry_is_encrypted)
    ARCHIVE_FUNCTION(archive_entry_size_is_set)
    ARCHIVE_FUNCTION(archive_entry_size)
#undef ARCHIVE_FUNCTION

    /**
     * @brief 只解析受支持格式的必要符号，不调用外部 tar/unzip/unrar。
     */
    ArchiveApi()
    {
#ifdef Q_OS_WIN
        library.setFileName(QStringLiteral("archiveint"));
#else
        library.setFileName(QStringLiteral("archive"));
        if (!library.load())
            library.setFileName(QStringLiteral("libarchive.so.13"));
#endif
        if (!library.load())
            fail(QStringLiteral("Qt 资源解压库不可用，请检查系统 libarchive"), 503);
#define LOAD(name)                                                                                 \
    name = reinterpret_cast<decltype(name)>(library.resolve(#name));                               \
    if (!name)                                                                                     \
        fail(QStringLiteral("系统 libarchive 缺少接口：") + QStringLiteral(#name), 503);
        LOAD(archive_read_new)
        LOAD(archive_read_free)
        LOAD(archive_read_support_filter_none)
        LOAD(archive_read_support_filter_gzip)
        LOAD(archive_read_support_filter_bzip2)
        LOAD(archive_read_support_filter_xz)
        LOAD(archive_read_support_format_tar)
        LOAD(archive_read_support_format_zip)
        LOAD(archive_read_support_format_rar)
        LOAD(archive_read_support_format_rar5)
        LOAD(archive_read_open)
        LOAD(archive_read_set_seek_callback)
        LOAD(archive_read_next_header)
        LOAD(archive_read_data)
        LOAD(archive_read_data_skip)
        LOAD(archive_error_string)
        LOAD(archive_entry_pathname_utf8)
        LOAD(archive_entry_filetype)
        LOAD(archive_entry_symlink)
        LOAD(archive_entry_hardlink)
        LOAD(archive_entry_is_encrypted)
        LOAD(archive_entry_size_is_set)
        LOAD(archive_entry_size)
#undef LOAD
    }

    /**
     * @brief 警告也不当作完整成功，避免发布截断或无法解码的归档。
     * @param code libarchive 调用返回码。
     * @param handle libarchive 句柄，用于获取详细错误。
     */
    void check(int code, struct archive *handle) const
    {
        if (code != ARCHIVE_OK) {
            const char *detail = archive_error_string(handle);
            fail(QStringLiteral("资源包解压失败：") +
                 (detail ? QString::fromUtf8(detail) : QStringLiteral("归档格式无效")));
        }
    }
};

/** @brief 仅更改本工作线程的字符集，避免修改 Qt 全局 locale。 */
class ArchiveLocale {
public:

    /**
     * @brief 临时切换当前线程的归档文件名 locale。
     */
    ArchiveLocale()
    {
#ifdef Q_OS_LINUX
        locale_ = newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
        if (!locale_)
            fail(QStringLiteral("解压中文资源需要系统 C.UTF-8 locale"), 503);
        previous_ = uselocale(locale_);
#endif
    }

    /**
     * @brief 恢复线程原 locale 并释放临时对象。
     */
    ~ArchiveLocale()
    {
#ifdef Q_OS_LINUX
        uselocale(previous_);
        freelocale(locale_);
#endif
    }

private:

#ifdef Q_OS_LINUX
    locale_t locale_ = nullptr, previous_ = nullptr;
#endif
};

/** @brief 固定块读取输入；ZIP 中央目录在进入解码器前单独限长。 */
class ArchiveReader {
public:

    ArchiveApi api;
    QFile file;
    QByteArray block = QByteArray(65536, '\0');
    struct archive *handle = nullptr;

    /**
     * @brief 打开普通受控文件；各格式由库根据实际内容识别。
     * @param path 待访问的文件或配置路径。
     */
    explicit ArchiveReader(const QString &path) : file(path)
    {
        if (!file.open(QIODevice::ReadOnly))
            fail(QStringLiteral("资源包缓存不可读"));
        handle = api.archive_read_new();
        if (!handle)
            fail(QStringLiteral("资源解压器内存不足"), 503);
        try {
            for (auto function : {api.archive_read_support_filter_none,
                                  api.archive_read_support_filter_gzip,
                                  api.archive_read_support_filter_bzip2,
                                  api.archive_read_support_filter_xz,
                                  api.archive_read_support_format_tar,
                                  api.archive_read_support_format_zip,
                                  api.archive_read_support_format_rar,
                                  api.archive_read_support_format_rar5})
                api.check(function(handle), handle);
            api.check(api.archive_read_set_seek_callback(handle, &ArchiveReader::seek), handle);
            api.check(api.archive_read_open(handle, this, nullptr, &ArchiveReader::read, nullptr),
                      handle);
        } catch (...) {
            api.archive_read_free(handle);
            handle = nullptr;
            throw;
        }
    }

    /**
     * @brief 任意退出路径均关闭归档句柄。
     */
    ~ArchiveReader()
    {
        if (handle)
            api.archive_read_free(handle);
    }

    /**
     * @brief 向 C 解码器提供固定大小的下一个输入块。
     * @param context 归档文件读取上下文。
     * @param buffer 输出读取缓冲区地址，内存由上下文持有。
     * @return 实际读取字节数；底层文件错误返回负值。
     */
    static la_ssize_t read(struct archive *, void *context, const void **buffer)
    {
        auto *self = static_cast<ArchiveReader *>(context);
        const auto count = self->file.read(self->block.data(), self->block.size());
        *buffer = self->block.constData();
        return static_cast<la_ssize_t>(count);
    }

    /**
     * @brief 由 Qt 文件对象处理定位，兼容带 data descriptor 的常见 ZIP。
     * @param context 归档读取上下文。
     * @param offset 相对指定起点的字节偏移。
     * @param whence 定位起点，遵循 SEEK_SET、SEEK_CUR 或 SEEK_END。
     * @return 定位后的文件偏移，失败返回 -1。
     */
    static la_int64_t seek(struct archive *, void *context, la_int64_t offset, int whence)
    {
        auto &file = static_cast<ArchiveReader *>(context)->file;
        const qint64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? file.pos() : file.size();
        if (offset < -base || offset > file.size() - base || !file.seek(base + offset))
            return -1;
        return file.pos();
    }
};

/**
 * @brief 按原资源包规则限制 ZIP 条目/中央目录，防止解码元数据占满 Qt 内存。
 * @param path 待访问的文件或配置路径。
 */
void checkZipDirectory(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail(QStringLiteral("资源包不可读"));
    if (file.peek(8).startsWith("Rar!"))
        return;
    file.seek(qMax<qint64>(0, file.size() - 65557));
    const auto tail = file.readAll();
    const int marker = tail.lastIndexOf(QByteArray("PK\x05\x06", 4));
    if (marker < 0 || tail.size() - marker < 22)
        return;
    const auto u16 = [&](int at) {
        return quint32(uchar(tail.at(marker + at))) |
               (quint32(uchar(tail.at(marker + at + 1))) << 8);
    };
    const quint32 entries = u16(10), size = u16(12) | (u16(14) << 16);
    if (entries > 20000 || size > 16 * MiB)
        fail(QStringLiteral("ZIP 目录过大或不支持 ZIP64"));
}

/**
 * @brief 规范化归档成员名称并拒绝越界路径。
 * @param name 待处理的名称。
 * @param directory 目标是否为目录。
 * @return Ycest 下的安全相对路径，空字符串代表包根目录本身。
 */
QString relativePath(const QString &name, bool directory)
{
    QString clean = name;
    while (clean.endsWith('/'))
        clean.chop(1);
    const auto parts = clean.split('/');
    if (parts.isEmpty() || parts.first() != "Ycest" || clean.contains('\\') || clean.contains(':'))
        fail(QStringLiteral("压缩包根目录必须为 Ycest，且不能包含非法路径"));
    for (const auto &part : parts) {
        if (part.isEmpty() || part == "." || part == "..")
            fail(QStringLiteral("压缩包包含路径穿越或非法路径"));
        for (auto ch : part)
            if (ch.unicode() < 32 || ch.unicode() == 127)
                fail(QStringLiteral("资源路径含控制字符"));
    }
    if (parts.size() == 1) {
        if (!directory)
            fail(QStringLiteral("Ycest 必须是目录"));
        return {};
    }
    if ((parts.size() == 2 && !directory) || parts.at(1) == Work ||
        parts.at(1) == ".ycest-update.lock" || parts.at(1) == ".ycest-update.qt-lock")
        fail(QStringLiteral("Ycest 下只能提供普通资源目录，不能使用系统保留名称"));
    return parts.mid(1).join('/');
}

/**
 * @brief 持久化文件或目录项，保证事务日志和目录重命名顺序。
 * @param path 待访问的文件或配置路径。
 * @param directory 目标是否为目录。
 */
void syncPath(const QString &path, bool directory = false)
{
#ifdef Q_OS_UNIX
    const int fd = ::open(QFile::encodeName(path).constData(),
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (directory ? O_DIRECTORY : 0));
    if (fd < 0)
        fail(QStringLiteral("资源同步路径不可用"), 503);
    const int result = ::fsync(fd);
    ::close(fd);
    if (result != 0)
        fail(QStringLiteral("资源写入同步失败"), 503);
#else
    Q_UNUSED(path)
    Q_UNUSED(directory)
#endif
}

/**
 * @brief 原子保存任务状态或事务日志，不向查询方暴露半个 JSON。
 * @param path 待访问的文件或配置路径。
 * @param value 待解析或转换的输入值。
 */
void saveJson(const QString &path, const QJsonObject &value)
{
    QSaveFile file(path);
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        fail(QStringLiteral("资源更新状态无法保存"), 503);
    syncPath(QFileInfo(path).absolutePath(), true);
}

/**
 * @brief 读取资源更新的持久化 JSON 结果。
 * @param path 待访问的文件或配置路径。
 * @return 不存在时为空；事务日志损坏时拒绝继续替换。
 */
QJsonObject readJson(const QString &path)
{
    if (!QFileInfo::exists(path))
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * MiB)
        fail(QStringLiteral("资源更新记录不可读"));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        fail(QStringLiteral("资源更新记录损坏，保留现场"));
    return document.object();
}

/**
 * @brief 仅允许删除受控目录的直接子项，符号链接本身被删除但不跟随。
 * @param path 待访问的文件或配置路径。
 * @param root 受控根目录。
 */
void removeChild(const QString &path, const QString &root)
{
    const QFileInfo info(path);
    if (info.absolutePath() != QDir::cleanPath(QFileInfo(root).absoluteFilePath()) ||
        info.fileName().isEmpty() || info.fileName() == "." || info.fileName() == "..")
        fail(QStringLiteral("资源清理路径越界"));
    if (info.isSymLink() || info.isFile()) {
        if (!QFile::remove(path))
            fail(QStringLiteral("资源文件清理失败"), 503);
    } else if (info.exists() && !QDir(path).removeRecursively()) {
        fail(QStringLiteral("资源目录清理失败"), 503);
    }
}

/**
 * @brief 同文件系统内移动目录，并同步两侧目录项。
 * @param source 待移动的源目录。
 * @param destination 受控目标目录。
 */
void moveDirectory(const QString &source, const QString &destination)
{
    if (!QDir().rename(source, destination))
        fail(QStringLiteral("资源目录替换失败"), 503);
    syncPath(QFileInfo(source).absolutePath(), true);
    if (QFileInfo(destination).absolutePath() != QFileInfo(source).absolutePath())
        syncPath(QFileInfo(destination).absolutePath(), true);
}

/** @brief 跨进程互斥，Linux 兼容旧 Python 更新留下的锁文件。 */
class UpdateLock {
public:

    /**
     * @brief 为资源目录更新取得进程间独占锁。
     * @param root 受控根目录。
     */
    explicit UpdateLock(const QString &root)
    {
#ifdef Q_OS_UNIX
        const QString path = QDir(root).filePath(".ycest-update.lock");
        fd_ = ::open(QFile::encodeName(path).constData(),
                     O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC,
                     0600);
        if (fd_ < 0 || ::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = -1;
            fail(QStringLiteral("已有资源更新任务正在执行"), 409);
        }
#else
        lock_.reset(new QLockFile(QDir(root).filePath(".ycest-update.qt-lock")));
        if (!lock_->tryLock(0))
            fail(QStringLiteral("已有资源更新任务正在执行"), 409);
#endif
    }

    /**
     * @brief 退出后自动释放，不删除可复用的 Linux 锁文件。
     */
    ~UpdateLock()
    {
#ifdef Q_OS_UNIX
        if (fd_ >= 0)
            ::close(fd_);
#endif
    }

private:

#ifdef Q_OS_UNIX
    int fd_ = -1;
#else
    std::unique_ptr<QLockFile> lock_;
#endif
};

/**
 * @brief 根据目录事务日志恢复中断的资源替换。
 * @param root 受控根目录。
 * @return 恢复的旧日志；未提交目录回滚，已提交事务仅清理备份。
 */
QJsonObject recoverDirectories(const QString &root)
{
    const QString work = QDir(root).filePath(Work);
    if (QFileInfo(work).isSymLink())
        fail(QStringLiteral("资源暂存目录不能是符号链接"));
    const auto journal = readJson(QDir(work).filePath("journal.json"));
    if (!journal.isEmpty() && journal.value("phase") != "COMMITTED") {
        const auto entries = journal.value("directories").toArray();
        for (int i = entries.size() - 1; i >= 0; --i) {
            const auto item = entries.at(i).toObject();
            const auto name = item.value("name").toString();
            if (name.contains('/') || relativePath("Ycest/" + name, true) != name)
                fail(QStringLiteral("资源恢复日志目录无效"));
            const QString target = QDir(root).filePath(name),
                          backup = QDir(work).filePath("backup/" + name),
                          stage = QDir(work).filePath("stage/" + name);
            if (QFileInfo::exists(backup)) {
                removeChild(target, root);
                moveDirectory(backup, target);
            } else if (!item.value("existed").toBool() && !QFileInfo::exists(stage)) {
                removeChild(target, root);
            }
        }
    }
    if (QFileInfo::exists(work))
        removeChild(work, root);
    return journal;
}

/**
 * @brief 保存提交成功状态后安排独立的既有 Qt 重启入口。
 * @param command 固定的重启命令及参数。
 * @param[in,out] result 处理结果。
 */
void scheduleRestart(const QStringList &command, QJsonObject &result)
{
    QProcess process;
    process.start(command.value(0), command.mid(1));
    if (process.waitForStarted(1000) && process.waitForFinished(5000) &&
        process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) {
        result.insert("restartScheduled", true);
        return;
    }
    process.kill();
    process.waitForFinished(1000);
    result.insert("restartScheduled", false);
    result.insert("restartError", QStringLiteral("资源已更新，但设备未刷新，请检查设备服务。"));
}
} // namespace

/**
 * @brief 初始化线程及跨线程 JSON 结果类型。
 * @param options 受控资源更新参数；路径和重启命令均不来自网页。
 * @param parent Qt 父对象；对象销毁由父子关系管理，空指针表示无父对象。
 */
WebResourceUpdate::WebResourceUpdate(const WebResourceOptions &options, QObject *parent)
    : QThread(parent), options_(options)
{
    qRegisterMetaType<QJsonObject>();
    setObjectName(QStringLiteral("web-resource-update"));
}

/**
 * @brief 检查资源更新所需的 libarchive 接口是否可用。
 * @return 只检查库和接口，不修改资源、不启动线程或重启。
 */
bool WebResourceUpdate::available()
{
    try {
        ArchiveApi api;
        return true;
    } catch (...) {
        return false;
    }
}

/**
 * @brief 独立线程调用同一生产处理函数，再排队通知 Web 服务。
 */
void WebResourceUpdate::run()
{
    emit completed(
        execute(options_, [this](int percent, const QString &message, const QString &phase) {
            emit progress(percent, message, phase);
        }));
}

/** @brief 在独立批次目录展开图片，所有路径只用于校验，输出文件统一使用原始文件名。 */
QJsonArray WebResourceUpdate::extractFaceImages(const QString &archive,
                                               const QString &destination,
                                               qint64 expandedBudget)
{
    const QFileInfo input(archive), folder(destination);
    if (!input.isFile() || input.isSymLink() || input.size() <= 0 || input.size() > 100 * MiB)
        fail(QStringLiteral("人员图片压缩包大小无效，最多 100 MiB"));
    if (expandedBudget <= 0)
        fail(QStringLiteral("人员图片展开空间或配额不足"), 507);
    const QString parent = folder.absolutePath();
    if (folder.exists() || folder.isSymLink() || QFileInfo(parent).canonicalFilePath() != parent ||
        !QDir().mkdir(folder.absoluteFilePath()))
        fail(QStringLiteral("人员图片解压目录不可用"));
    ArchiveLocale locale;
    checkZipDirectory(archive);
    ArchiveReader reader(archive);
    struct archive_entry *item = nullptr;
    QJsonArray images;
    QSet<QString> names;
    qint64 expanded = 0;
    int entries = 0;
    QByteArray buffer(65536, '\0');
    for (;;) {
        interrupted();
        const int code = reader.api.archive_read_next_header(reader.handle, &item);
        if (code == ARCHIVE_EOF) break;
        reader.api.check(code, reader.handle);
        if (++entries > 20000) fail(QStringLiteral("人员图片压缩包条目过多"));
        const auto type = reader.api.archive_entry_filetype(item);
        const bool directory = type == AE_IFDIR;
        if ((type != AE_IFREG && !directory) || reader.api.archive_entry_symlink(item) ||
            reader.api.archive_entry_hardlink(item) || reader.api.archive_entry_is_encrypted(item))
            fail(QStringLiteral("人员图片包不允许加密、链接或特殊文件"));
        const char *text = reader.api.archive_entry_pathname_utf8(item);
        if (!text) fail(QStringLiteral("图片包文件名无法转换为 UTF-8"));
        QString path = QString::fromUtf8(text);
        while (path.startsWith("./")) path.remove(0, 2);
        while (path.endsWith('/')) path.chop(1);
        if (path.isEmpty() && directory) continue;
        const auto parts = path.split('/');
        if (path.startsWith('/') || path.contains('\\') || path.contains(':'))
            fail(QStringLiteral("人员图片包路径无效"));
        for (const auto &part : parts) {
            if (part.isEmpty() || part == "." || part == "..")
                fail(QStringLiteral("人员图片包包含路径穿越"));
            for (const auto ch : part)
                if (ch.unicode() < 32 || ch.unicode() == 127)
                    fail(QStringLiteral("人员图片包路径含控制字符"));
        }
        if (directory) continue;
        const qint64 size = reader.api.archive_entry_size(item);
        if (!reader.api.archive_entry_size_is_set(item) || size < 0 ||
            size > expandedBudget - expanded)
            fail(QStringLiteral("人员图片展开空间或配额不足"), 507);
        expanded += size;
        const QString name = parts.last();
        if (!WebPaths::accepts("person_face", name) || parts.contains("__MACOSX") || name.startsWith("._")) {
            reader.api.check(reader.api.archive_read_data_skip(reader.handle), reader.handle);
            continue;
        }
        WebPaths::filename(name);
        if (images.size() >= 499 || names.contains(name.toCaseFolded()))
            fail(QStringLiteral("图片包最多 499 张，且不同目录不能包含同名图片"));
        if (size <= 0 || size > 10 * MiB)
            fail(QStringLiteral("人脸图片为空或超过 10 MiB：") + name);
        QStorageInfo storage(parent);
        storage.refresh();
        if (!storage.isReady() || storage.bytesAvailable() < size + 64 * MiB)
            fail(QStringLiteral("人员图片解压剩余空间不足"), 507);
        const QString target = QDir(destination).filePath(name);
        QFile output(target);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            fail(QStringLiteral("人员图片暂存文件创建失败"), 503);
        qint64 written = 0;
        for (;;) {
            interrupted();
            const auto count = reader.api.archive_read_data(reader.handle, buffer.data(), size_t(buffer.size()));
            if (count < 0) reader.api.check(int(count), reader.handle);
            if (count == 0) break;
            if (count > size - written || output.write(buffer.constData(), count) != count)
                fail(QStringLiteral("人员图片长度错误或写入失败"), 503);
            written += count;
        }
        if (written != size || !output.flush()) fail(QStringLiteral("人员图片解压不完整"));
        output.close();
        WebPaths::validateFaceImage(target, name);
        names.insert(name.toCaseFolded());
        images.append(QJsonObject{{"kind", "person_face"}, {"original_name", name}, {"path", target}});
    }
    if (images.isEmpty()) fail(QStringLiteral("压缩包中没有可导入的 JPG、JPEG、PNG 或 BMP 图片"));
    return images;
}

/**
 * @brief 校验全部条目、流式解压、目录事务、落盘结果；失败回滚未提交目录。
 * @param options 受控资源更新参数；路径和重启命令均不来自网页。
 * @param progress 实际进度回调，不提供时忽略进度通知。
 * @return 实际更新或恢复结果，含成功目录或失败原因。
 * @note 仅操作受控文件路径；归档校验、目录替换和结果落盘在线程内完成，不访问数据库或控件。
 */
QJsonObject WebResourceUpdate::execute(const WebResourceOptions &options, Progress progress)
{
    QJsonObject result;
    const QString root = QDir::cleanPath(QFileInfo(options.destination).absoluteFilePath());
    const QString work = QDir(root).filePath(Work), stage = QDir(work).filePath("stage"),
                  backup = QDir(work).filePath("backup"),
                  journalPath = QDir(work).filePath("journal.json");
    bool locked = false;
    std::unique_ptr<UpdateLock> lock;
    QElapsedTimer checkpoint;
    checkpoint.start();
    // 进度信号和状态文件节流，避免大量小文件拖慢 Web 接口或设备界面。
    const auto report =
        [&](int percent, const QString &message, const QString &phase, bool force = false) {
            if (!force && checkpoint.elapsed() < 150)
                return;
            checkpoint.restart();
            saveJson(options.result,
                     {{"state", phase}, {"progress", percent}, {"message", message}});
            if (progress)
                progress(percent, message, phase);
        };
    try {
        if (options.destination.isEmpty() || !QDir().mkpath(root) || QFileInfo(root).isSymLink() ||
            QFileInfo(root).canonicalFilePath() != root)
            fail(QStringLiteral("资源目标目录不可用或包含符号链接"));
        lock.reset(new UpdateLock(root));
        locked = true;
        const auto previous = recoverDirectories(root);
        if (options.recovery) {
            result = readJson(options.result);
            if (previous.value("phase") == "COMMITTED" &&
                previous.value("result").toString() == options.result) {
                QJsonArray names;
                for (const auto &item : previous.value("directories").toArray())
                    names.append(item.toObject().value("name"));
                result.insert("state", "READY");
                result.insert("updatedDirectories", names);
            }
            if (result.value("state") != "READY")
                fail(QStringLiteral("资源更新中断，未提交的目录已恢复，请重新上传"));
            saveJson(options.result, result);
            return result; // 恢复只核对旧事务，不重放包、不重复安排重启。
        }
        if (options.restart.isEmpty())
            fail(QStringLiteral("设备重启入口未配置"), 503);
        if (!QFileInfo(options.archive).isFile() || QFileInfo(options.archive).isSymLink() ||
            QFileInfo(options.archive).size() <= 0 || QFileInfo(options.archive).size() > 512 * MiB)
            fail(QStringLiteral("资源包文件或大小无效"));
        ArchiveLocale locale;
        checkZipDirectory(options.archive);

        struct Entry {
            QString name;
            bool directory;
            qint64 size;
        };

        QList<Entry> entries;
        QSet<QString> paths, directories;
        qint64 total = 0;
        report(0, QStringLiteral("正在检查资源包"), "VALIDATING", true);
        {
            ArchiveReader reader(options.archive);
            struct archive_entry *item = nullptr;
            for (;;) {
                interrupted();
                const int code = reader.api.archive_read_next_header(reader.handle, &item);
                if (code == ARCHIVE_EOF)
                    break;
                reader.api.check(code, reader.handle);
                if (entries.size() >= 20000)
                    fail(QStringLiteral("压缩包条目过多"));
                const auto type = reader.api.archive_entry_filetype(item);
                const bool directory = type == AE_IFDIR;
                if ((type != AE_IFREG && !directory) || reader.api.archive_entry_symlink(item) ||
                    reader.api.archive_entry_hardlink(item) ||
                    reader.api.archive_entry_is_encrypted(item))
                    fail(QStringLiteral("资源包不允许加密、链接或特殊文件"));
                const char *text = reader.api.archive_entry_pathname_utf8(item);
                if (!text)
                    fail(QStringLiteral("资源文件名无法转换为 UTF-8"));
                const QString name = relativePath(QString::fromUtf8(text), directory);
                const qint64 size = reader.api.archive_entry_size(item);
                if ((!directory && !reader.api.archive_entry_size_is_set(item)) || size < 0 ||
                    size > ExpandedLimit - total)
                    fail(QStringLiteral("压缩包展开后超过 2 GiB 或长度无效"));
                if (paths.contains(name))
                    fail(QStringLiteral("压缩包包含重复路径"));
                paths.insert(name);
                entries.append({name, directory, size});
                if (!directory)
                    total += size;
                if (!name.isEmpty())
                    directories.insert(name.section('/', 0, 0));
                reader.api.check(reader.api.archive_read_data_skip(reader.handle), reader.handle);
                report(2,
                       QStringLiteral("正在检查资源包（%1 项）").arg(entries.size()),
                       "VALIDATING");
            }
        }
        if (directories.isEmpty())
            fail(QStringLiteral("Ycest 下没有可更新的资源目录"));
        QStorageInfo storage(root);
        storage.refresh();
        if (!storage.isReady() || storage.bytesAvailable() < total + 64 * MiB)
            fail(QStringLiteral("资源存储空间不足，原资源未修改"), 507);
        if (!QDir().mkpath(stage) || !QDir().mkpath(backup))
            fail(QStringLiteral("无法创建资源暂存目录"), 503);
        report(5, QStringLiteral("正在解压资源包"), "VALIDATING", true);
        qint64 received = 0;
        {
            ArchiveReader reader(options.archive);
            struct archive_entry *item = nullptr;
            QByteArray bytes(1024 * 1024, '\0');
            for (const auto &entry : entries) {
                interrupted();
                reader.api.check(reader.api.archive_read_next_header(reader.handle, &item),
                                 reader.handle);
                const char *text = reader.api.archive_entry_pathname_utf8(item);
                if (!text || relativePath(QString::fromUtf8(text), entry.directory) != entry.name ||
                    reader.api.archive_entry_size(item) != entry.size)
                    fail(QStringLiteral("资源包内容在检查后发生变化"));
                if (entry.name.isEmpty())
                    continue;
                const auto target = QDir(stage).filePath(entry.name);
                if (!QDir().mkpath(entry.directory ? target : QFileInfo(target).absolutePath()))
                    fail(QStringLiteral("资源暂存目录创建失败"), 503);
                if (entry.directory)
                    continue;
                storage.refresh();
                if (storage.bytesAvailable() < entry.size + 64 * MiB)
                    fail(QStringLiteral("资源存储空间不足"), 507);
                QFile output(target);
                if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                    fail(QStringLiteral("无法创建资源暂存文件"), 503);
                qint64 written = 0;
                for (;;) {
                    interrupted();
                    const auto count = reader.api.archive_read_data(reader.handle,
                                                                    bytes.data(),
                                                                    size_t(bytes.size()));
                    if (count < 0)
                        reader.api.check(int(count), reader.handle);
                    if (count == 0)
                        break;
                    if (count > entry.size - written ||
                        output.write(bytes.constData(), count) != count)
                        fail(QStringLiteral("资源内容长度错误或磁盘写入失败"), 503);
                    written += count;
                    received += count;
                    const int percent = total ? 5 + int(85.0 * received / total) : 90;
                    report(percent, QStringLiteral("正在解压：%1").arg(entry.name), "VALIDATING");
                }
                if (written != entry.size || !output.flush())
                    fail(QStringLiteral("资源文件不完整"));
                output.close();
                syncPath(target);
            }
            if (reader.api.archive_read_next_header(reader.handle, &item) != ARCHIVE_EOF)
                fail(QStringLiteral("资源包内容在检查后发生变化"));
        }
        // 新目录全部写盘后才开始正式目录替换，缺失目录不会进入事务日志。
        QDirIterator staged(stage,
                            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
                            QDirIterator::Subdirectories);
        while (staged.hasNext())
            syncPath(staged.next(), true);
        syncPath(stage, true);
        QStringList names = directories.values();
        names.sort();
        QJsonArray journalEntries;
        for (const auto &name : names) {
            const QFileInfo target(QDir(root).filePath(name));
            if (target.isSymLink() || (target.exists() && !target.isDir()))
                fail(QStringLiteral("目标资源必须为普通目录：") + name);
            journalEntries.append(QJsonObject{{"name", name}, {"existed", target.exists()}});
        }
        QJsonObject journal{{"phase", "REPLACING"},
                            {"directories", journalEntries},
                            {"result", options.result}};
        saveJson(journalPath, journal);
        for (int i = 0; i < names.size(); ++i) {
            interrupted();
            const auto &name = names.at(i);
            const auto target = QDir(root).filePath(name);
            report(90 + 8 * i / names.size(),
                   QStringLiteral("正在更新：%1").arg(name),
                   "COMMITTING",
                   true);
            if (QFileInfo::exists(target))
                moveDirectory(target, QDir(backup).filePath(name));
            moveDirectory(QDir(stage).filePath(name), target);
        }
        journal.insert("phase", "COMMITTED");
        saveJson(journalPath, journal);
        result = {{"state", "READY"},
                  {"progress", 100},
                  {"updatedDirectories", QJsonArray::fromStringList(names)}};
        saveJson(options.result, result);
        try {
            recoverDirectories(root);
        } catch (...) { /* 已提交更新的备份留待下次清理。 */
        }
        scheduleRestart(options.restart, result);
        saveJson(options.result, result);
        if (progress)
            progress(100,
                     result.value("restartScheduled").toBool()
                         ? QStringLiteral("更新完成！设备即将重启，请稍候")
                         : result.value("restartError").toString(),
                     "READY");
        return result;
    } catch (const WebError &error) {
        result = {{"state", "FAILED"}, {"error", error.message}};
    } catch (const std::exception &error) {
        result = {{"state", "FAILED"}, {"error", QString::fromUtf8(error.what())}};
    } catch (...) {
        result = {{"state", "FAILED"}, {"error", QStringLiteral("资源更新发生未知错误")}};
    }
    if (locked) {
        try {
            recoverDirectories(root);
        } catch (...) {
            result.insert("error",
                          result.value("error").toString() +
                              QStringLiteral("；恢复未完成，请保留现场"));
        }
    }
    try {
        saveJson(options.result, result);
    } catch (...) { /* 原错误优先返回给 Web 查询。 */
    }
    if (progress)
        progress(0, result.value("error").toString(), "FAILED");
    return result;
}
