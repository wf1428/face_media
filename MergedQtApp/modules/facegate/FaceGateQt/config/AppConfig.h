/**
 * @file AppConfig.h
 * @brief 人脸门禁全部可持久化运行配置。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "CameraProfile.h"

#include <QString>

/**
 * @brief 人脸门禁全部可持久化运行配置。
 *
 * load()/save() 与 INI 文件互转；defaults() 提供设备可启动的基线值。
 * 时间字段统一使用 ms，图像尺寸使用 px，相似度和质量阈值使用 0.0~1.0。
 */
class AppConfig {
public:
    /** @return 包含内置默认值以及初始化管理员凭据的配置。 */
    static AppConfig defaults();

    /** @return 从 INI 加载并归一化后的配置；缺失键保留默认值。 */
    static AppConfig load(const QString &iniPath);

    /** @brief 将当前配置写入 INI 文件。 */
    bool save(const QString &iniPath, QString *errorText = nullptr) const;

    /** @return 用于管理员密码散列的随机盐文本。 */
    static QString generateSalt();

    /** @return salt 与 password 组合后的稳定密码散列。 */
    static QString hashPassword(const QString &salt, const QString &password);

    /** @return 用户名和密码与当前管理员凭据匹配时返回 true。 */
    bool verifyAdminPassword(const QString &username, const QString &password) const;

    /** @brief 校验旧密码后更新盐和密码散列。 */
    bool setAdminPassword(const QString &oldPassword, const QString &newPassword, QString *errorText = nullptr);

    /** @return 根据 cameraSource 选择并组装的当前摄像头配置。 */
    CameraProfile activeCameraProfile() const;

    /** @return 设备 UI 设计宽度 1024 px。 */
    static constexpr int screenWidth() { return 1024; }

    /** @return 设备 UI 设计高度 768 px。 */
    static constexpr int screenHeight() { return 768; }

    // MIPI 摄像头基础参数。
    QString cameraDevice = "/dev/video0";
    int cameraWidth = 640;
    int cameraHeight = 480;
    QString cameraPixelFormat = "nv12";

    // 摄像头来源及 USB UVC 采集/控制参数；尺寸单位 px，帧率单位 fps。
    QString cameraSource = "auto";
    QString usbCameraDevice = "auto";
    int usbCameraWidth = 640;
    int usbCameraHeight = 480;
    int usbCameraFps = 30;
    QString usbCameraPixelFormat = "yuyv";
    bool usbAutoWhiteBalance = true;
    int usbWhiteBalanceTemperature = 4600;
    bool usbAutoExposure = true;
    int usbExposureAbsolute = 166;
    bool usbExposureAutoPriority = false;
    int usbPowerLineFrequency = 1;
    QString configPath; /**< 当前配置文件路径，供界面保存和诊断使用。 */

    // InspireFace 资源包和两个活体检测 RKNN 模型路径。
    QString inspirePackPath = "./Gundam_RK356X";
    QString miniFasnetV2Path = "./2.7_80x80_MiniFASNetV2.rknn";
    QString miniFasnetV1SePath = "./4_0_0_80x80_MiniFASNetV1SE.rknn";

    // 识别、去重、质量、检测和活体阈值，范围均为 0.0~1.0。
    float faceCosineThreshold = 0.50f;
    float duplicateFaceCosineThreshold = 0.50f;
    float faceQualityThreshold = 0.50f;
    float faceDetectThreshold = 0.70f;
    float livenessThreshold = 0.80f;

    int collectFrameCount = 5;       /**< 一次验证采集的候选帧数。 */
    int livenessPassRequired = 5;    /**< 判定活体通过所需的通过帧数。 */
    int resultHoldMs = 2000;         /**< 验证结果界面保持时间，单位 ms。 */
    int sameFaceSuppressMs = 3000;   /**< 同一人员重复通过抑制窗口，单位 ms。 */
    bool livenessEnabled = true;     /**< 是否在身份匹配后执行活体检测。 */

    QString snapshotDir = "./data/snapshots"; /**< 验证抓拍保存目录。 */
    bool saveFailedSnapshot = true;           /**< 是否保存验证失败抓拍。 */

    bool privacyMode = false;                 /**< 是否隐藏姓名等可识别个人信息。 */
    int strangerPromptCooldownMs = 2000;      /**< 陌生人提示最小间隔，单位 ms。 */

    bool audioEnabled = true;                 /**< 语音提示总开关。 */
    QString audioPlayer = QStringLiteral("aplay"); /**< 外部 WAV 播放程序。 */
    QString audioDevice = QStringLiteral("plughw:CARD=rockchiprk809co,DEV=0"); /**< ALSA 输出设备。 */
    QString audioDir = QStringLiteral("./audio"); /**< 提示音文件目录。 */
    int audioVolume = 80;                     /**< 逻辑播放音量，范围 0~100。 */
    QString audioMixerControl = QStringLiteral("Playback"); /**< 系统混音器控制项。 */
    int audioCooldownMs = 1500;               /**< 同类语音提示冷却时间，单位 ms。 */

    // 各业务提示的独立开关、界面文本和 WAV 文件名。
    bool promptVerifyPassedEnabled = true;
    QString promptVerifyPassedText = QStringLiteral("验证通过，请通行");
    QString promptVerifyPassedFile = QStringLiteral("verify_passed.wav");

    bool promptVerifyFailedEnabled = true;
    QString promptVerifyFailedText = QStringLiteral("验证失败，请正对摄像头");
    QString promptVerifyFailedFile = QStringLiteral("verify_failed.wav");

    bool promptStrangerEnabled = true;
    QString promptStrangerText = QStringLiteral("未录入人员，请联系管理员");
    QString promptStrangerFile = QStringLiteral("stranger.wav");

    bool promptLivenessFailedEnabled = true;
    QString promptLivenessFailedText = QStringLiteral("活体检测失败，请重试");
    QString promptLivenessFailedFile = QStringLiteral("liveness_failed.wav");

    bool promptEnrollSuccessEnabled = true;
    QString promptEnrollSuccessText = QStringLiteral("录入成功");
    QString promptEnrollSuccessFile = QStringLiteral("enroll_success.wav");

    bool promptEnrollFailedEnabled = true;
    QString promptEnrollFailedText = QStringLiteral("录入失败，请重新采集");
    QString promptEnrollFailedFile = QStringLiteral("enroll_failed.wav");

    int maxFeaturesPerPerson = 3; /**< 每个人员最多保留的人脸特征数量。 */

    QString databaseDriver = "QSQLITE";       /**< Qt SQL 驱动名。 */
    QString localStorePath = "/home/cat/face_media/access_control.db"; /**< 统一门禁数据库兼容路径。 */
    QString sqlitePath = "/home/cat/face_media/access_control.db";     /**< 统一 SQLite 数据库路径。 */

    // MySQL 连接参数，mysqlPort 单位为 TCP 端口号。
    QString mysqlHost = "127.0.0.1";
    int mysqlPort = 3306;
    QString mysqlDatabase = "face_gate";
    QString mysqlUser = "root";
    QString mysqlPassword;

    // 管理界面展示和保存的网络配置。
    bool networkDhcp = true;
    QString networkStaticIp;
    QString networkNetmask;
    QString networkGateway;
    QString networkDns;

    QString adminUsername = QStringLiteral("admin"); /**< 管理员登录名。 */
    QString adminPasswordHash;              /**< 加盐后的管理员密码散列，不保存明文。 */
    QString adminPasswordSalt;              /**< 管理员密码随机盐。 */
    QString adminPasswordHint = QStringLiteral("初始密码：admin，首次登录后请修改"); /**< 登录提示。 */
    bool adminLoginRequired = true;         /**< 进入管理页前是否必须认证。 */
    bool adminPasswordChangeRequired = true; /**< 是否强制修改初始密码。 */

    QString backupDir = "./backup"; /**< 数据备份目录。 */
    int diskFreeWarnMb = 512;       /**< 低磁盘空间告警阈值，单位 MiB。 */

};

#endif
