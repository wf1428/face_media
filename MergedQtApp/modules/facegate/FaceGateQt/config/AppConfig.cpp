/**
 * @file AppConfig.cpp
 * @brief 人脸门禁全部可持久化运行配置的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "AppConfig.h"

#include <QDir>
#include <QDebug>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QSettings>
#include <QStringList>
#include <QtGlobal>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QTextCodec>
#endif
#include <QUuid>

namespace {
/** @brief 以固定小数精度序列化浮点配置值。 */
QString floatText(float value)
{
    return QString::number(static_cast<double>(value), 'f', 3);
}

/** @brief 将布尔配置值序列化为稳定文本。 */
QString boolText(bool value)
{
    return value ? QStringLiteral("true") : QStringLiteral("false");
}

/** @brief 返回首次安装时使用的管理员密码默认盐值。 */
QString defaultAdminSalt()
{
    return QStringLiteral("FaceGateQt.default.admin.salt.v1");
}

/** @brief 判断文本是否包含中日韩统一表意字符。 */
bool containsCjk(const QString &text)
{
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if ((u >= 0x4e00 && u <= 0x9fff) ||
            (u >= 0x3400 && u <= 0x4dbf) ||
            (u >= 0xf900 && u <= 0xfaff)) {
            return true;
        }
    }
    return false;
}

/** @brief 识别 UTF-8 文本被旧编码错误解码后的常见乱码特征。 */
bool looksLikeLegacyUtf8Mojibake(const QString &text)
{
    if (text.isEmpty()) {
        return false;
    }

    int suspiciousCount = 0;
    for (const QChar ch : text) {
        const ushort u = ch.unicode();
        if (u == 0xfffd ||
            (u >= 0x0080 && u <= 0x009f) ||
            (u >= 0x00c0 && u <= 0x024f)) {
            ++suspiciousCount;
        }
    }

    static const QStringList markers = {
        QStringLiteral("Ã"), QStringLiteral("Â"), QStringLiteral("å"),
        QStringLiteral("æ"), QStringLiteral("ç"), QStringLiteral("è"),
        QStringLiteral("é"), QStringLiteral("ä"), QStringLiteral("ï¼")
    };
    for (const QString &marker : markers) {
        if (text.contains(marker)) {
            ++suspiciousCount;
        }
    }

    return suspiciousCount >= 2 || text.contains(QChar(0xfffd));
}

/** @brief 仅在候选结果明显改善乱码特征时修复历史配置文本编码。 */
QString repairTextEncoding(const QString &text, const QString &fallback)
{
    const QString value = text.trimmed();
    if (value.isEmpty()) {
        return fallback;
    }
    if (!looksLikeLegacyUtf8Mojibake(value)) {
        return value;
    }

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QTextCodec *codec = QTextCodec::codecForName("Windows-1252");
    if (codec) {
        const QString repaired = QString::fromUtf8(codec->fromUnicode(value)).trimmed();
        if (!repaired.isEmpty() && !looksLikeLegacyUtf8Mojibake(repaired) && containsCjk(repaired)) {
            return repaired;
        }
    }
#endif

    const QString latin1Repaired = QString::fromUtf8(value.toLatin1()).trimmed();
    if (!latin1Repaired.isEmpty() && !looksLikeLegacyUtf8Mojibake(latin1Repaired) && containsCjk(latin1Repaired)) {
        return latin1Repaired;
    }

    return fallback;
}

/** @brief 读取配置文本并执行兼容性编码修复。 */
QString readTextSetting(QSettings &settings, const QString &key, const QString &fallback)
{
    return repairTextEncoding(settings.value(key, fallback).toString(), fallback);
}

/** @brief 以 UTF-8 安全形式写入文本配置项。 */
QString writeTextSetting(const QString &text, const QString &fallback)
{
    return repairTextEncoding(text, fallback);
}
}

/** @return 包含内置默认值以及初始化管理员凭据的配置。 */
AppConfig AppConfig::defaults()
{
    AppConfig cfg;
    cfg.adminPasswordSalt = defaultAdminSalt();
    cfg.adminPasswordHash = hashPassword(cfg.adminPasswordSalt, QStringLiteral("admin"));
    cfg.adminPasswordChangeRequired = true;
    return cfg;
}

/** @return 根据 cameraSource 选择并组装的当前摄像头配置。 */
CameraProfile AppConfig::activeCameraProfile() const
{
    CameraProfile profile;
    if (cameraSource == QStringLiteral("usb")) {
        profile.source = CameraSourceType::Usb;
        profile.devicePath = usbCameraDevice;
        profile.width = usbCameraWidth;
        profile.height = usbCameraHeight;
        profile.fps = usbCameraFps;
        profile.pixelFormat = usbCameraPixelFormat;
        profile.usbControls.autoWhiteBalance = usbAutoWhiteBalance;
        profile.usbControls.whiteBalanceTemperature = usbWhiteBalanceTemperature;
        profile.usbControls.autoExposure = usbAutoExposure;
        profile.usbControls.exposureAbsolute = usbExposureAbsolute;
        profile.usbControls.exposureAutoPriority = usbExposureAutoPriority;
        profile.usbControls.powerLineFrequency = usbPowerLineFrequency;
        return profile;
    }

    profile.source = cameraSource == QStringLiteral("auto")
                         ? CameraSourceType::Auto
                         : CameraSourceType::Mipi;
    profile.devicePath = cameraDevice;
    profile.width = cameraWidth;
    profile.height = cameraHeight;
    profile.pixelFormat = cameraPixelFormat;
    return profile;
}

/** @return 用于管理员密码散列的随机盐文本。 */
QString AppConfig::generateSalt()
{
    return QUuid::createUuid().toString(QUuid::Id128);
}

/** @return salt 与 password 组合后的稳定密码散列。 */
QString AppConfig::hashPassword(const QString &salt, const QString &password)
{
    // 管理员密码统一使用 sha256(salt + password)，配置文件中只保存盐和哈希。
    const QByteArray input = (salt + password).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex());
}

/** @return 用户名和密码与当前管理员凭据匹配时返回 true。 */
bool AppConfig::verifyAdminPassword(const QString &username, const QString &password) const
{
    if (!adminLoginRequired) {
        return true;
    }
    const QString trimmedUser = username.trimmed();
    if (trimmedUser != adminUsername || adminPasswordSalt.isEmpty() || adminPasswordHash.isEmpty()) {
        return false;
    }
    return hashPassword(adminPasswordSalt, password) == adminPasswordHash;
}

/** @brief 校验旧密码后更新盐和密码散列。 */
bool AppConfig::setAdminPassword(const QString &oldPassword, const QString &newPassword, QString *errorText)
{
    if (adminLoginRequired && !verifyAdminPassword(adminUsername, oldPassword)) {
        if (errorText) {
            *errorText = QStringLiteral("原密码不正确");
        }
        return false;
    }
    if (newPassword.size() < 6) {
        if (errorText) {
            *errorText = QStringLiteral("新密码至少需要 6 位");
        }
        return false;
    }

    adminPasswordSalt = generateSalt();
    adminPasswordHash = hashPassword(adminPasswordSalt, newPassword);
    adminPasswordHint = QStringLiteral("已设置自定义密码");
    adminPasswordChangeRequired = false;
    return true;
}

/** @return 从 INI 加载并归一化后的配置；缺失键保留默认值。 */
AppConfig AppConfig::load(const QString &iniPath)
{
    AppConfig cfg = defaults();
    cfg.configPath = iniPath;
    QSettings s(iniPath, QSettings::IniFormat);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    s.setIniCodec("UTF-8");
#endif

    // 按配置分组读取运行参数，未配置项继续使用默认值。
    cfg.cameraDevice = s.value("camera/device", cfg.cameraDevice).toString();
    cfg.cameraWidth = s.value("camera/width", cfg.cameraWidth).toInt();
    cfg.cameraHeight = s.value("camera/height", cfg.cameraHeight).toInt();
    cfg.cameraPixelFormat = s.value("camera/pixel_format", cfg.cameraPixelFormat).toString().toLower();

    cfg.usbCameraDevice =
        s.value("camera_usb/device", cfg.usbCameraDevice).toString().trimmed();
    cfg.usbCameraWidth =
        qMax(1, s.value("camera_usb/width", cfg.usbCameraWidth).toInt());
    cfg.usbCameraHeight =
        qMax(1, s.value("camera_usb/height", cfg.usbCameraHeight).toInt());
    cfg.usbCameraFps =
        qMax(1, s.value("camera_usb/fps", cfg.usbCameraFps).toInt());
    cfg.usbCameraPixelFormat =
        s.value("camera_usb/pixel_format", cfg.usbCameraPixelFormat)
            .toString()
            .trimmed()
            .toLower();
    cfg.usbAutoWhiteBalance =
        s.value("camera_usb/auto_white_balance", cfg.usbAutoWhiteBalance).toBool();
    cfg.usbWhiteBalanceTemperature =
        s.value("camera_usb/white_balance_temperature",
                cfg.usbWhiteBalanceTemperature)
            .toInt();
    cfg.usbAutoExposure =
        s.value("camera_usb/auto_exposure", cfg.usbAutoExposure).toBool();
    cfg.usbExposureAbsolute =
        s.value("camera_usb/exposure_absolute", cfg.usbExposureAbsolute).toInt();
    cfg.usbExposureAutoPriority =
        s.value("camera_usb/exposure_auto_priority",
                cfg.usbExposureAutoPriority)
            .toBool();
    cfg.usbPowerLineFrequency =
        s.value("camera_usb/power_line_frequency", cfg.usbPowerLineFrequency)
            .toInt();

    const QString cameraSource =
        QString::fromLocal8Bit(qgetenv("QT_YCEST_CAMERA_SOURCE"))
            .trimmed()
            .toLower();
    if (cameraSource.isEmpty() || cameraSource == QStringLiteral("auto")) {
        cfg.cameraSource = QStringLiteral("auto");
    } else if (cameraSource == QStringLiteral("mipi")) {
        cfg.cameraSource = QStringLiteral("mipi");
    } else if (cameraSource == QStringLiteral("usb")) {
        cfg.cameraSource = QStringLiteral("usb");
    } else {
        qWarning() << "[CAMERA-CONFIG] invalid QT_YCEST_CAMERA_SOURCE="
                   << cameraSource << "; using auto";
        cfg.cameraSource = QStringLiteral("auto");
    }
    qInfo() << "[CAMERA-CONFIG] selected source=" << cfg.cameraSource;

    cfg.inspirePackPath = s.value("model/inspire_pack", cfg.inspirePackPath).toString();
    cfg.miniFasnetV2Path = s.value("model/minifasnet_v2", cfg.miniFasnetV2Path).toString();
    cfg.miniFasnetV1SePath = s.value("model/minifasnet_v1se", cfg.miniFasnetV1SePath).toString();

    cfg.faceCosineThreshold = s.value("threshold/face_cosine", cfg.faceCosineThreshold).toFloat();
    cfg.duplicateFaceCosineThreshold = s.value("threshold/duplicate_face_cosine", cfg.duplicateFaceCosineThreshold).toFloat();
    cfg.faceQualityThreshold = s.value("threshold/face_quality", cfg.faceQualityThreshold).toFloat();
    cfg.faceDetectThreshold = s.value("threshold/face_detect", cfg.faceDetectThreshold).toFloat();
    cfg.livenessThreshold = s.value("threshold/liveness", cfg.livenessThreshold).toFloat();

    /*
     * 重复录入判断必须至少和正常识别一样严格。
     * 如果 duplicate_face_cosine 高于 face_cosine，同一个人可能已经能通过
     * 闸机识别，却仍被允许用另一个 ID 再次录入。
     */
    if (cfg.duplicateFaceCosineThreshold > cfg.faceCosineThreshold) {
        cfg.duplicateFaceCosineThreshold = cfg.faceCosineThreshold;
    }

    cfg.collectFrameCount = s.value("verify/collect_frame_count", cfg.collectFrameCount).toInt();
    cfg.livenessPassRequired = s.value("verify/liveness_pass_required", cfg.livenessPassRequired).toInt();
    cfg.resultHoldMs = s.value("verify/result_hold_ms", cfg.resultHoldMs).toInt();
    cfg.sameFaceSuppressMs = s.value("verify/same_face_suppress_ms", cfg.sameFaceSuppressMs).toInt();
    cfg.livenessEnabled = s.value("verify/liveness_enabled", cfg.livenessEnabled).toBool();

    cfg.snapshotDir = s.value("log/snapshot_dir", cfg.snapshotDir).toString();
    cfg.saveFailedSnapshot = s.value("log/save_failed_snapshot", cfg.saveFailedSnapshot).toBool();

    cfg.privacyMode = s.value("ui/privacy_mode", cfg.privacyMode).toBool();
    cfg.strangerPromptCooldownMs = s.value("ui/stranger_prompt_cooldown_ms", cfg.strangerPromptCooldownMs).toInt();

    cfg.audioEnabled = s.value("audio/enabled", cfg.audioEnabled).toBool();
    cfg.audioPlayer = s.value("audio/player", cfg.audioPlayer).toString();
    cfg.audioDevice = s.value("audio/device", cfg.audioDevice).toString();
    cfg.audioDir = s.value("audio/dir", cfg.audioDir).toString();
    cfg.audioVolume = qBound(0, s.value("audio/volume", cfg.audioVolume).toInt(), 100);
    cfg.audioMixerControl = s.value("audio/mixer_control", cfg.audioMixerControl).toString();
    cfg.audioCooldownMs = qMax(0, s.value("audio/cooldown_ms", cfg.audioCooldownMs).toInt());

    cfg.promptVerifyPassedEnabled = s.value("audio/prompt_verify_passed_enabled", cfg.promptVerifyPassedEnabled).toBool();
    cfg.promptVerifyPassedText = readTextSetting(s, QStringLiteral("audio/prompt_verify_passed_text"), cfg.promptVerifyPassedText);
    cfg.promptVerifyPassedFile = s.value("audio/prompt_verify_passed_file", cfg.promptVerifyPassedFile).toString();

    cfg.promptVerifyFailedEnabled = s.value("audio/prompt_verify_failed_enabled", cfg.promptVerifyFailedEnabled).toBool();
    cfg.promptVerifyFailedText = readTextSetting(s, QStringLiteral("audio/prompt_verify_failed_text"), cfg.promptVerifyFailedText);
    cfg.promptVerifyFailedFile = s.value("audio/prompt_verify_failed_file", cfg.promptVerifyFailedFile).toString();

    cfg.promptStrangerEnabled = s.value("audio/prompt_stranger_enabled", cfg.promptStrangerEnabled).toBool();
    cfg.promptStrangerText = readTextSetting(s, QStringLiteral("audio/prompt_stranger_text"), cfg.promptStrangerText);
    cfg.promptStrangerFile = s.value("audio/prompt_stranger_file", cfg.promptStrangerFile).toString();

    cfg.promptLivenessFailedEnabled = s.value("audio/prompt_liveness_failed_enabled", cfg.promptLivenessFailedEnabled).toBool();
    cfg.promptLivenessFailedText = readTextSetting(s, QStringLiteral("audio/prompt_liveness_failed_text"), cfg.promptLivenessFailedText);
    cfg.promptLivenessFailedFile = s.value("audio/prompt_liveness_failed_file", cfg.promptLivenessFailedFile).toString();

    cfg.promptEnrollSuccessEnabled = s.value("audio/prompt_enroll_success_enabled", cfg.promptEnrollSuccessEnabled).toBool();
    cfg.promptEnrollSuccessText = readTextSetting(s, QStringLiteral("audio/prompt_enroll_success_text"), cfg.promptEnrollSuccessText);
    cfg.promptEnrollSuccessFile = s.value("audio/prompt_enroll_success_file", cfg.promptEnrollSuccessFile).toString();

    cfg.promptEnrollFailedEnabled = s.value("audio/prompt_enroll_failed_enabled", cfg.promptEnrollFailedEnabled).toBool();
    cfg.promptEnrollFailedText = readTextSetting(s, QStringLiteral("audio/prompt_enroll_failed_text"), cfg.promptEnrollFailedText);
    cfg.promptEnrollFailedFile = s.value("audio/prompt_enroll_failed_file", cfg.promptEnrollFailedFile).toString();

    cfg.maxFeaturesPerPerson = qMax(1, s.value("enroll/max_features_per_person", cfg.maxFeaturesPerPerson).toInt());

    // 合并应用只使用一份设备 SQLite；忽略旧配置中的 MySQL、相对 DB 和 FGDB 路径。
    cfg.databaseDriver = QStringLiteral("QSQLITE");
    cfg.localStorePath = QStringLiteral("/home/cat/face_media/access_control.db");
    cfg.sqlitePath = QStringLiteral("/home/cat/face_media/access_control.db");

    cfg.mysqlHost = s.value("mysql/host", cfg.mysqlHost).toString();
    cfg.mysqlPort = s.value("mysql/port", cfg.mysqlPort).toInt();
    cfg.mysqlDatabase = s.value("mysql/database", cfg.mysqlDatabase).toString();
    cfg.mysqlUser = s.value("mysql/user", cfg.mysqlUser).toString();
    cfg.mysqlPassword = s.value("mysql/password", cfg.mysqlPassword).toString();

    cfg.networkDhcp = s.value("network/dhcp", cfg.networkDhcp).toBool();
    cfg.networkStaticIp = s.value("network/static_ip", cfg.networkStaticIp).toString();
    cfg.networkNetmask = s.value("network/netmask", cfg.networkNetmask).toString();
    cfg.networkGateway = s.value("network/gateway", cfg.networkGateway).toString();
    cfg.networkDns = s.value("network/dns", cfg.networkDns).toString();

    const QString defaultSalt = defaultAdminSalt();
    const QString defaultHash = hashPassword(defaultSalt, QStringLiteral("admin"));
    const bool hasAdminHash = s.contains("admin/password_hash") && s.contains("admin/password_salt");
    cfg.adminUsername = s.value("admin/username", cfg.adminUsername).toString().trimmed();
    if (cfg.adminUsername.isEmpty()) {
        cfg.adminUsername = QStringLiteral("admin");
    }
    cfg.adminPasswordSalt = s.value("admin/password_salt", cfg.adminPasswordSalt).toString();
    cfg.adminPasswordHash = s.value("admin/password_hash", cfg.adminPasswordHash).toString();
    cfg.adminPasswordHint = readTextSetting(s, QStringLiteral("admin/password_hint"), cfg.adminPasswordHint);
    cfg.adminLoginRequired = s.value("admin/login_required", cfg.adminLoginRequired).toBool();
    cfg.adminPasswordChangeRequired = s.value("admin/password_change_required", !hasAdminHash).toBool();
    if (cfg.adminPasswordSalt.isEmpty() || cfg.adminPasswordHash.isEmpty()) {
        cfg.adminPasswordSalt = defaultSalt;
        cfg.adminPasswordHash = defaultHash;
        cfg.adminPasswordChangeRequired = true;
    }
    if (cfg.adminPasswordSalt == defaultSalt && cfg.adminPasswordHash == defaultHash) {
        cfg.adminPasswordChangeRequired = true;
    }

    cfg.backupDir = s.value("maintenance/backup_dir", cfg.backupDir).toString();
    cfg.diskFreeWarnMb = qMax(0, s.value("maintenance/disk_free_warn_mb", cfg.diskFreeWarnMb).toInt());

    return cfg;
}

/** @brief 将当前配置写入 INI 文件。 */
bool AppConfig::save(const QString &iniPath, QString *errorText) const
{
    const QString path = iniPath.trimmed().isEmpty() ? configPath : iniPath.trimmed();
    if (path.isEmpty()) {
        if (errorText) {
            *errorText = "配置文件路径为空";
        }
        return false;
    }

    const QFileInfo info(path);
    if (!info.absolutePath().isEmpty() && !QDir().mkpath(info.absolutePath())) {
        if (errorText) {
            *errorText = "创建配置目录失败：" + info.absolutePath();
        }
        return false;
    }

    QSettings s(path, QSettings::IniFormat);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    s.setIniCodec("UTF-8");
#endif
    s.setValue("camera/device", cameraDevice);
    s.setValue("camera/width", QString::number(cameraWidth));
    s.setValue("camera/height", QString::number(cameraHeight));
    s.setValue("camera/pixel_format", cameraPixelFormat);
    s.setValue("camera_usb/device", usbCameraDevice);
    s.setValue("camera_usb/width", QString::number(usbCameraWidth));
    s.setValue("camera_usb/height", QString::number(usbCameraHeight));
    s.setValue("camera_usb/fps", QString::number(usbCameraFps));
    s.setValue("camera_usb/pixel_format", usbCameraPixelFormat);
    s.setValue("camera_usb/auto_white_balance", boolText(usbAutoWhiteBalance));
    s.setValue("camera_usb/white_balance_temperature",
               QString::number(usbWhiteBalanceTemperature));
    s.setValue("camera_usb/auto_exposure", boolText(usbAutoExposure));
    s.setValue("camera_usb/exposure_absolute",
               QString::number(usbExposureAbsolute));
    s.setValue("camera_usb/exposure_auto_priority",
               boolText(usbExposureAutoPriority));
    s.setValue("camera_usb/power_line_frequency",
               QString::number(usbPowerLineFrequency));

    s.setValue("model/inspire_pack", inspirePackPath);
    s.setValue("model/minifasnet_v2", miniFasnetV2Path);
    s.setValue("model/minifasnet_v1se", miniFasnetV1SePath);

    s.setValue("threshold/face_cosine", floatText(faceCosineThreshold));
    s.setValue("threshold/duplicate_face_cosine", floatText(duplicateFaceCosineThreshold));
    s.setValue("threshold/face_quality", floatText(faceQualityThreshold));
    s.setValue("threshold/face_detect", floatText(faceDetectThreshold));
    s.setValue("threshold/liveness", floatText(livenessThreshold));

    s.setValue("verify/collect_frame_count", QString::number(collectFrameCount));
    s.setValue("verify/liveness_pass_required", QString::number(livenessPassRequired));
    s.setValue("verify/result_hold_ms", QString::number(resultHoldMs));
    s.setValue("verify/same_face_suppress_ms", QString::number(sameFaceSuppressMs));
    s.setValue("verify/liveness_enabled", boolText(livenessEnabled));

    s.setValue("log/snapshot_dir", snapshotDir);
    s.setValue("log/save_failed_snapshot", boolText(saveFailedSnapshot));

    s.setValue("ui/privacy_mode", boolText(privacyMode));
    s.setValue("ui/stranger_prompt_cooldown_ms", QString::number(strangerPromptCooldownMs));

    s.setValue("audio/enabled", boolText(audioEnabled));
    s.setValue("audio/player", audioPlayer);
    s.setValue("audio/device", audioDevice);
    s.setValue("audio/dir", audioDir);
    s.setValue("audio/volume", QString::number(qBound(0, audioVolume, 100)));
    s.setValue("audio/mixer_control", audioMixerControl);
    s.setValue("audio/cooldown_ms", QString::number(qMax(0, audioCooldownMs)));

    s.setValue("audio/prompt_verify_passed_enabled", boolText(promptVerifyPassedEnabled));
    s.setValue("audio/prompt_verify_passed_text", writeTextSetting(promptVerifyPassedText, AppConfig::defaults().promptVerifyPassedText));
    s.setValue("audio/prompt_verify_passed_file", promptVerifyPassedFile);

    s.setValue("audio/prompt_verify_failed_enabled", boolText(promptVerifyFailedEnabled));
    s.setValue("audio/prompt_verify_failed_text", writeTextSetting(promptVerifyFailedText, AppConfig::defaults().promptVerifyFailedText));
    s.setValue("audio/prompt_verify_failed_file", promptVerifyFailedFile);

    s.setValue("audio/prompt_stranger_enabled", boolText(promptStrangerEnabled));
    s.setValue("audio/prompt_stranger_text", writeTextSetting(promptStrangerText, AppConfig::defaults().promptStrangerText));
    s.setValue("audio/prompt_stranger_file", promptStrangerFile);

    s.setValue("audio/prompt_liveness_failed_enabled", boolText(promptLivenessFailedEnabled));
    s.setValue("audio/prompt_liveness_failed_text", writeTextSetting(promptLivenessFailedText, AppConfig::defaults().promptLivenessFailedText));
    s.setValue("audio/prompt_liveness_failed_file", promptLivenessFailedFile);

    s.setValue("audio/prompt_enroll_success_enabled", boolText(promptEnrollSuccessEnabled));
    s.setValue("audio/prompt_enroll_success_text", writeTextSetting(promptEnrollSuccessText, AppConfig::defaults().promptEnrollSuccessText));
    s.setValue("audio/prompt_enroll_success_file", promptEnrollSuccessFile);

    s.setValue("audio/prompt_enroll_failed_enabled", boolText(promptEnrollFailedEnabled));
    s.setValue("audio/prompt_enroll_failed_text", writeTextSetting(promptEnrollFailedText, AppConfig::defaults().promptEnrollFailedText));
    s.setValue("audio/prompt_enroll_failed_file", promptEnrollFailedFile);

    s.setValue("enroll/max_features_per_person", QString::number(maxFeaturesPerPerson));

    s.setValue("database/driver", databaseDriver);
    s.setValue("database/local_store_path", localStorePath);
    s.setValue("database/sqlite_path", sqlitePath);

    s.setValue("mysql/host", mysqlHost);
    s.setValue("mysql/port", QString::number(mysqlPort));
    s.setValue("mysql/database", mysqlDatabase);
    s.setValue("mysql/user", mysqlUser);
    s.setValue("mysql/password", mysqlPassword);

    s.setValue("network/dhcp", boolText(networkDhcp));
    s.setValue("network/static_ip", networkStaticIp);
    s.setValue("network/netmask", networkNetmask);
    s.setValue("network/gateway", networkGateway);
    s.setValue("network/dns", networkDns);

    s.setValue("admin/username", adminUsername);
    s.setValue("admin/password_hash", adminPasswordHash);
    s.setValue("admin/password_salt", adminPasswordSalt);
    s.setValue("admin/password_hint", writeTextSetting(adminPasswordHint, AppConfig::defaults().adminPasswordHint));
    s.setValue("admin/login_required", boolText(adminLoginRequired));
    s.setValue("admin/password_change_required", boolText(adminPasswordChangeRequired));

    s.setValue("maintenance/backup_dir", backupDir);
    s.setValue("maintenance/disk_free_warn_mb", QString::number(diskFreeWarnMb));
    s.sync();

    if (s.status() != QSettings::NoError) {
        if (errorText) {
            *errorText = "写入配置文件失败";
        }
        return false;
    }

    return true;
}
