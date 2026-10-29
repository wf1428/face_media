/**
 * @file resource_update_lock.h
 * @brief 资源包、Web、U盘及媒体传输共用的非阻塞文件锁。
 * @author Du lin
 * @date 2026-10-24
 */
#pragma once
#include <QFile>
#include <QLockFile>
#include <memory>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

/**
 * @brief Linux 使用与 Web 更新相同的 flock 文件；媒体持共享锁，资源替换持独占锁。
 * @note 调用方先创建可信资源根目录；锁文件不能为符号链接。不等待其他持有者。
 *       非 Unix 使用同名 Qt 独占锁，保守串行化媒体与资源更新。
 */
class ResourceUpdateLock final {
public:
    /** @brief 创建未持锁对象，不访问文件系统。 */
    ResourceUpdateLock() = default;
    /** @brief 关闭句柄释放锁；不删除 Linux 锁文件，避免 inode 锁分裂。 */
    ~ResourceUpdateLock() {
#ifdef Q_OS_UNIX
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    ResourceUpdateLock(const ResourceUpdateLock &) = delete;
    ResourceUpdateLock &operator=(const ResourceUpdateLock &) = delete;

    /**
     * @brief 尝试取得资源访问权；同一对象已持锁时直接成功。
     * @param root 可信的 images 资源根目录。
     * @param exclusive true 为资源更新独占，false 为媒体传输共享。
     * @return 是否持有锁；失败不修改资源。
     */
    bool acquire(const QString &root, bool exclusive = true) {
#ifdef Q_OS_UNIX
        if (fd_ >= 0) return true;
        const QByteArray path = QFile::encodeName(root + QStringLiteral("/.ycest-update.lock"));
        fd_ = ::open(path.constData(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd_ < 0) return false;
        if (::flock(fd_, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) == 0) return true;
        ::close(fd_);
        fd_ = -1;
        return false;
#else
        Q_UNUSED(exclusive);
        if (lock_) return true;
        auto lock = std::make_unique<QLockFile>(root + QStringLiteral("/.ycest-update.qt-lock"));
        if (!lock->tryLock(0)) return false;
        lock_ = std::move(lock);
        return true;
#endif
    }
private:
#ifdef Q_OS_UNIX
    int fd_ = -1; /**< 持锁描述符，由析构关闭；-1 表示未持有。 */
#else
    std::unique_ptr<QLockFile> lock_; /**< 拥有 Qt 锁，析构时释放。 */
#endif
};
