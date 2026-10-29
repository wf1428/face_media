/**
 * @file ftp_transfer.h
 * @brief 视频与资源包共用的 FTP 路径编码及低优先级进程启动工具。
 * @author Du lin
 * @date 2026-10-24
 */
#pragma once
#include <QFileInfo>
#include <QProcess>
#include <QUrl>

namespace FtpTransfer {
/**
 * @brief 按目录段编码 FTP 路径，保留分隔符及旧视频下载的普通路径行为。
 * @param host 已验证的 FTP 主机，不含协议和路径。
 * @param port TCP 端口，范围 1~65535。
 * @param remotePath FTP 路径；没有开头斜杠时自动补齐。
 * @param encoded 可为空；非空时输出是否执行了百分号编码。
 * @return 可作为 curl 独立参数使用的 FTP URL，不包含账号密码。
 */
inline QString url(const QString &host, int port, QString remotePath, bool *encoded = nullptr)
{
    remotePath = remotePath.trimmed();
    if (!remotePath.startsWith('/')) remotePath.prepend('/');
    bool encode = false;
    const QString reserved = QStringLiteral(" #%?[]{}^|\\\"<>`");
    for (const QChar ch : remotePath)
        if (ch.unicode() > 127 || reserved.contains(ch)) encode = true;
    if (encoded) *encoded = encode;
    if (encode) {
        QStringList parts = remotePath.split('/'); // 默认保留空段，兼容 Qt 5.12 与 5.15。
        for (QString &part : parts)
            if (!part.isEmpty()) part = QString::fromLatin1(QUrl::toPercentEncoding(part));
        remotePath = parts.join('/');
    }
    return QStringLiteral("ftp://%1:%2%3").arg(host).arg(port).arg(remotePath);
}

/**
 * @brief 异步启动进程；Linux 优先使用 ionice idle 类和 nice 10 降低下载争用。
 * @param process 调用线程拥有的 QProcess；为空时不启动，非空时由调用方处理完成及取消。
 * @param program 程序路径。
 * @param arguments 独立参数列表，不经过 shell，也不在本工具中输出凭据。
 * @note 不等待进程退出；缺少调度工具时直接启动原程序。
 */
inline void start(QProcess *process, const QString &program, const QStringList &arguments)
{
    if (!process) return;
#ifdef Q_OS_LINUX
    const auto executable = [](const QString &name) {
        for (const QString &prefix : {QStringLiteral("/usr/bin/"), QStringLiteral("/bin/")})
            if (QFileInfo(prefix + name).isExecutable()) return prefix + name;
        return QString();
    };
    const QString ionice = executable(QStringLiteral("ionice"));
    const QString nice = executable(QStringLiteral("nice"));
    if (!ionice.isEmpty()) {
        QStringList args{QStringLiteral("-c"), QStringLiteral("3")};
        if (!nice.isEmpty()) args << nice << QStringLiteral("-n") << QStringLiteral("10");
        args << program;
        args << arguments;
        process->start(ionice, args);
        return;
    }
    if (!nice.isEmpty()) {
        QStringList args{QStringLiteral("-n"), QStringLiteral("10"), program};
        args << arguments;
        process->start(nice, args);
        return;
    }
#endif
    process->start(program, arguments);
}
} // namespace FtpTransfer
