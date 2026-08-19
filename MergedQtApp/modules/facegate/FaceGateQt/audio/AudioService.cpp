/**
 * @file AudioService.cpp
 * @brief 使用外部 aplay 进程播放门禁语音提示的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "AudioService.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>

/** @brief 创建尚未配置的音频服务。 */
AudioService::AudioService(QObject *parent)
    : QObject(parent), config_(AppConfig::defaults())
{
}

/** @brief 替换完整运行配置，并应用音量设置。 */
void AudioService::configure(const AppConfig &config)
{
    config_ = config;
    config_.audioVolume = qBound(0, config_.audioVolume, 100);
    applySystemVolume(config_.audioVolume);
}

/** @brief 启用或关闭语音提示总开关。 */
void AudioService::setEnabled(bool enabled)
{
    config_.audioEnabled = enabled;
}

/** @brief 设置 0~100 的逻辑音量并尝试同步系统混音器。 */
void AudioService::setVolume(int volume)
{
    config_.audioVolume = qBound(0, volume, 100);
    applySystemVolume(config_.audioVolume);
}

/** @brief 在提示类型启用且冷却允许时播放对应 WAV。 */
void AudioService::playPrompt(Prompt prompt)
{
    if (!config_.audioEnabled) {
        return;
    }
    if (!promptEnabled(prompt)) {
        return;
    }
    startAplay(absoluteAudioPath(promptFile(prompt)), false, prompt);
}

/** @brief 按配置键测试播放指定提示，忽略业务冷却。 */
void AudioService::testPlay(const QString &promptKey)
{
    const QString key = promptKey.trimmed().toLower();
    Prompt prompt = Prompt::VerifyPassed;
    if (key == QStringLiteral("verify_passed")) {
        prompt = Prompt::VerifyPassed;
    } else if (key == QStringLiteral("verify_failed")) {
        prompt = Prompt::VerifyFailed;
    } else if (key == QStringLiteral("stranger")) {
        prompt = Prompt::Stranger;
    } else if (key == QStringLiteral("liveness_failed")) {
        prompt = Prompt::LivenessFailed;
    } else if (key == QStringLiteral("enroll_success")) {
        prompt = Prompt::EnrollSuccess;
    } else if (key == QStringLiteral("enroll_failed")) {
        prompt = Prompt::EnrollFailed;
    } else {
        emit audioError(QStringLiteral("未知音频事件：%1").arg(promptKey));
        return;
    }
    startAplay(absoluteAudioPath(promptFile(prompt)), true, prompt);
}

/** @brief 兼容预留接口；当前 detached 播放模式下不执行停止操作。 */
void AudioService::stopCurrentPlayback()
{
    // 第一版使用 startDetached 启动短音频，不保留进程句柄。这里保留接口，便于后续扩展为可中断播放。
}

/** @return Prompt 对应的稳定配置键。 */
QString AudioService::keyForPrompt(Prompt prompt)
{
    switch (prompt) {
    case Prompt::VerifyPassed: return QStringLiteral("verify_passed");
    case Prompt::VerifyFailed: return QStringLiteral("verify_failed");
    case Prompt::Stranger: return QStringLiteral("stranger");
    case Prompt::LivenessFailed: return QStringLiteral("liveness_failed");
    case Prompt::EnrollSuccess: return QStringLiteral("enroll_success");
    case Prompt::EnrollFailed: return QStringLiteral("enroll_failed");
    }
    return QStringLiteral("unknown");
}

/** @return 指定提示的独立开关是否启用。 */
bool AudioService::promptEnabled(Prompt prompt) const
{
    switch (prompt) {
    case Prompt::VerifyPassed: return config_.promptVerifyPassedEnabled;
    case Prompt::VerifyFailed: return config_.promptVerifyFailedEnabled;
    case Prompt::Stranger: return config_.promptStrangerEnabled;
    case Prompt::LivenessFailed: return config_.promptLivenessFailedEnabled;
    case Prompt::EnrollSuccess: return config_.promptEnrollSuccessEnabled;
    case Prompt::EnrollFailed: return config_.promptEnrollFailedEnabled;
    }
    return false;
}

/** @return 指定提示配置的文件名。 */
QString AudioService::promptFile(Prompt prompt) const
{
    switch (prompt) {
    case Prompt::VerifyPassed: return config_.promptVerifyPassedFile;
    case Prompt::VerifyFailed: return config_.promptVerifyFailedFile;
    case Prompt::Stranger: return config_.promptStrangerFile;
    case Prompt::LivenessFailed: return config_.promptLivenessFailedFile;
    case Prompt::EnrollSuccess: return config_.promptEnrollSuccessFile;
    case Prompt::EnrollFailed: return config_.promptEnrollFailedFile;
    }
    return QString();
}

/** @return 相对 audioDir 解析后的音频绝对路径。 */
QString AudioService::absoluteAudioPath(const QString &fileName) const
{
    const QString trimmedFile = fileName.trimmed();
    if (trimmedFile.isEmpty()) {
        return QString();
    }

    QFileInfo fileInfo(trimmedFile);
    if (fileInfo.isAbsolute()) {
        return fileInfo.absoluteFilePath();
    }

    QString dirText = config_.audioDir.trimmed();
    if (dirText.isEmpty()) {
        dirText = QStringLiteral("./audio");
    }

    QDir dir(dirText);
    if (!dir.isAbsolute()) {
        dir = QDir(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(dirText));
    }
    return dir.absoluteFilePath(trimmedFile);
}

/** @return 距同类提示上次播放达到 audioCooldownMs 时返回 true。 */
bool AudioService::cooldownAllowed(Prompt prompt) const
{
    const int cooldownMs = qMax(0, config_.audioCooldownMs);
    if (cooldownMs <= 0) {
        return true;
    }

    const int key = static_cast<int>(prompt);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 lastMs = lastPromptMs_.value(key, 0);
    if (lastMs > 0 && nowMs - lastMs < cooldownMs) {
        return false;
    }
    lastPromptMs_.insert(key, nowMs);
    return true;
}

/** @brief 校验文件并启动 aplay 进程。 */
bool AudioService::startAplay(const QString &path, bool ignoreCooldown, Prompt prompt)
{
    if (path.trimmed().isEmpty()) {
        emit audioError(QStringLiteral("音频文件路径为空"));
        return false;
    }
    if (!ignoreCooldown && !cooldownAllowed(prompt)) {
        return false;
    }

    QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        const QString message = QStringLiteral("音频文件不存在：%1").arg(path);
        qWarning().noquote() << message;
        emit audioError(message);
        return false;
    }

    QString program = config_.audioPlayer.trimmed();
    if (program.isEmpty()) {
        program = QStringLiteral("aplay");
    }

    QStringList args;
    args << QStringLiteral("-q");
    if (!config_.audioDevice.trimmed().isEmpty()) {
        args << QStringLiteral("-D") << config_.audioDevice.trimmed();
    }
    args << info.absoluteFilePath();

    const bool started = QProcess::startDetached(program, args);
    if (!started) {
        const QString message = QStringLiteral("启动音频播放失败：%1 %2").arg(program, args.join(QStringLiteral(" ")));
        qWarning().noquote() << message;
        emit audioError(message);
        return false;
    }

    emit audioStatus(QStringLiteral("正在播放：%1").arg(info.fileName()));
    return true;
}

/** @brief 使用系统混音器工具设置配置的播放控制项。 */
bool AudioService::applySystemVolume(int volume)
{
    if (!config_.audioEnabled) {
        return true;
    }
    QString mixer = config_.audioMixerControl.trimmed();
    if (mixer.isEmpty()) {
        mixer = QStringLiteral("Playback");
    }

    const QString value = QString::number(qBound(0, volume, 100)) + QStringLiteral("%");
    QStringList args;
    args << QStringLiteral("sset") << mixer << value;

    QProcess process;
    process.start(QStringLiteral("amixer"), args);
    if (!process.waitForFinished(1500)) {
        process.kill();
        emit audioError(QStringLiteral("音量设置超时"));
        return false;
    }

    const bool ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    if (!ok) {
        const QString error = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        const QString message = error.isEmpty() ? QStringLiteral("音量设置失败") : QStringLiteral("音量设置失败：%1").arg(error);
        qWarning().noquote() << message;
        emit audioError(message);
        return false;
    }
    emit audioStatus(QStringLiteral("音量已设置为 %1%2").arg(qBound(0, volume, 100)).arg(QStringLiteral("%")));
    return true;
}
