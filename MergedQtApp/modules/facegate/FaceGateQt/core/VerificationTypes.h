/**
 * @file VerificationTypes.h
 * @brief 人脸验证从待机到结果冷却的状态机阶段。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef VERIFICATION_TYPES_H
#define VERIFICATION_TYPES_H

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>
#include <QMetaType>
#include <QtGlobal>

/** @brief 人脸验证从待机到结果冷却的状态机阶段。 */
enum class VerifyState {
    Idle,
    CollectingFrames,
    SelectBestFrame,
    LivenessChecking,
    FeatureMatching,
    Passed,
    Failed,
    Cooldown
};

/** @brief 人员主数据。时间字段使用数据库保存的文本格式。 */
struct PersonInfo {
    qint64 id = 0;          /**< 数据库人员主键。 */
    QString personNo;       /**< 业务唯一人员编号。 */
    QString name;           /**< 人员姓名。 */
    bool enabled = true;    /**< 是否允许参与门禁验证。 */
    bool deleted = false;   /**< 软删除标志。 */
    QString createdAt;      /**< 创建时间文本。 */
    QString updatedAt;      /**< 最近更新时间文本。 */
};

/** @brief 一个人脸特征向量及其模型、质量元数据。 */
struct FaceFeatureData {
    QByteArray blob;        /**< SDK 特征向量二进制。 */
    QString modelVersion;   /**< 生成该向量的模型版本。 */
    float quality = 0.0f;   /**< 录入图像质量分，通常为 0.0~1.0。 */
};

/** @brief 图库中一条人员与人脸特征的关联记录。 */
struct FaceRecord {
    qint64 featureId = 0;   /**< 人脸特征数据库主键。 */
    PersonInfo person;      /**< 特征所属人员快照。 */
    FaceFeatureData feature; /**< 特征数据。 */
    QString faceHash;       /**< 服务端下发的人脸原始哈希。 */
    QString imagePath;      /**< 录入原图路径。 */
    QString enrolledAt;     /**< 特征录入时间文本。 */
};

/** @brief 管理界面使用的人员及特征数量汇总记录。 */
struct PersonAdminRecord {
    qint64 id = 0;          /**< 人员主键。 */
    QString personNo;       /**< 业务人员编号。 */
    QString name;           /**< 人员姓名。 */
    bool enabled = true;    /**< 门禁启用状态。 */
    bool deleted = false;   /**< 软删除状态。 */
    int featureCount = 0;   /**< 当前有效人脸特征数量。 */
    QString createdAt;      /**< 人员创建时间。 */
    QString updatedAt;      /**< 人员最近更新时间。 */
    QString firstFeatureAt; /**< 最早特征录入时间。 */
    QString lastFeatureAt;  /**< 最近特征录入时间。 */
};

/** @brief 一次门禁验证的持久化日志。 */
struct VerifyLog {
    qint64 logId = 0;       /**< 数据库日志主键。 */
    qint64 personId = 0;    /**< 匹配人员主键；陌生人或失败时可为 0。 */
    QString personNo;       /**< 匹配人员编号快照。 */
    QString faceHash;       /**< 本次实际匹配的人脸原始哈希。 */
    QString result;         /**< 通过/失败等稳定结果码。 */
    float cosine = 0.0f;    /**< 最佳人脸余弦相似度。 */
    float liveScore = 0.0f; /**< 活体真实类别得分。 */
    QString snapshotPath;   /**< 验证抓拍路径。 */
    QString nameSnapshot;   /**< 验证时姓名快照，防止后续改名影响历史。 */
    QString failReason;     /**< 失败原因。 */
    QString deviceSn;       /**< 产生事件的设备序列号。 */
    QString eventType = "face_verify"; /**< 事件类型。 */
    QString direction = "in";          /**< 通行方向。 */
    QDateTime createdAt = QDateTime::currentDateTime(); /**< 事件本地时间。 */
};

/** @brief 管理界面查询展示用的验证日志文本记录。 */
struct VerifyLogViewRecord {
    qint64 logId = 0;
    qint64 personId = 0;
    QString personNo;
    QString name;
    QString result;
    float cosine = 0.0f;
    float liveScore = 0.0f;
    QString snapshotPath;
    QString nameSnapshot;
    QString failReason;
    QString deviceSn;
    QString eventType;
    QString direction;
    QString createdAt;
};

/** @brief 验证日志筛选条件；空字段表示不限制。 */
struct VerifyLogFilter {
    QString result;         /**< 结果码过滤。 */
    QString personNo;       /**< 人员编号模糊/精确规则由仓储实现定义。 */
    QString name;           /**< 姓名过滤。 */
    QDateTime startTime;    /**< 起始时间；无效表示不限制。 */
    QDateTime endTime;      /**< 结束时间；无效表示不限制。 */
    int limit = 500;        /**< 最大返回记录数。 */
};

/** @brief 管理员操作审计日志。 */
struct OperatorAuditLog {
    qint64 id = 0;
    QString operatorName;
    QString action;
    QString targetType;
    QString targetId;
    QString result;
    QString detail;
    QString createdAt;
};

/** @brief 待远端同步任务及重试状态。 */
struct SyncTaskRecord {
    qint64 id = 0;
    QString taskType;
    QString payloadJson;
    QString syncStatus;
    int retryCount = 0;     /**< 已执行的重试次数。 */
    QString lastError;
    QString createdAt;
    QString updatedAt;
};

/** @brief 系统运行事件日志。 */
struct SystemEventLog {
    qint64 id = 0;
    QString eventType;
    QString level;
    QString message;
    QString detail;
    QString createdAt;
};

/** @brief 本地存储关键表的记录数量汇总。 */
struct StorageStats {
    int personCount = 0;       /**< 人员记录数。 */
    int faceFeatureCount = 0;  /**< 本地与网络注册照片的实际文件数。 */
    qint64 registrationPhotoBytes = 0; /**< 本地与网络注册照片实际占用字节数。 */
    int verifyLogCount = 0;    /**< 验证日志记录数。 */
};

/**
 * @brief 摄像头服务跨线程投递的完整帧。
 *
 * image 用于显示/检测，yuv420sp 保留给需要原始 NV12/NV21 的算法；
 * enrollmentPreview 可提供针对录入流程优化的预览图。
 */
struct CameraFrame {
    QImage image;             /**< RGB 预览/检测图像。 */
    QImage enrollmentPreview; /**< 录入页面专用预览，可为空。 */
    QByteArray yuv420sp;      /**< 连续 YUV420SP 数据。 */
    QString pixelFormat;      /**< 标准化像素格式名称。 */
    int width = 0;            /**< 原始帧宽度，单位 px。 */
    int height = 0;           /**< 原始帧高度，单位 px。 */
    int frameIndex = 0;       /**< 单调递增采集序号。 */
    bool nv21 = true;         /**< true 为 VU，false 为 NV12 的 UV 排列。 */
};

/** @brief 一张图像中的单个人脸检测、质量和图库匹配结果。 */
struct DetectedFace {
    QRect rect;                  /**< 人脸框，坐标基于 CameraFrame::image。 */
    float detConfidence = -1.0f; /**< 检测置信度；-1 表示未提供。 */
    float quality = -1.0f;       /**< 人脸质量分；-1 表示未提供。 */
    float cosine = -1.0f;        /**< 最佳图库余弦相似度；-1 表示未比对。 */
    bool qualityValid = false;   /**< quality 是否可用于阈值判断。 */
    bool faceUsable = false;     /**< 人脸尺寸、位置和质量是否适合验证。 */
    bool galleryEmpty = true;    /**< 比对时图库是否为空。 */
    bool bestCandidateValid = false; /**< 是否存在可报告的最佳候选。 */
    bool matched = false;        /**< 最佳相似度是否达到识别阈值。 */
    QString overlayText;         /**< 预览覆盖层主文本。 */
    QString overlayStatus;       /**< 预览覆盖层状态码。 */
    QString faceHash;            /**< 最佳匹配人脸的原始哈希。 */
    PersonInfo person;           /**< 最佳匹配人员；无候选时为空。 */
};

/** @brief FaceEngine 对一帧的整体分析结果。 */
struct FaceAnalysisResult {
    bool sdkReady = false;       /**< InspireFace SDK 会话是否可用。 */
    bool valid = false;          /**< 本帧分析是否成功完成。 */
    QString message;             /**< 状态或错误说明。 */
    QVector<DetectedFace> faces; /**< 检测到的全部人脸。 */
};

/** @brief 验证状态机保存的一帧候选及其目标人脸。 */
struct VerificationSnapshot {
    QImage image;
    QByteArray yuv420sp;
    QString pixelFormat;
    int width = 0;
    int height = 0;
    bool nv21 = true;
    DetectedFace face;
    int frameIndex = 0;

    /**
     * @return 质量 60%、检测置信度 30%、归一化人脸面积 10% 的候选分数。
     */
    float score() const
    {
        const float quality = face.qualityValid ? face.quality : 0.0f;
        const float det = face.detConfidence > 0.0f ? face.detConfidence : 0.0f;
        const float faceSize = image.isNull() ? 0.0f :
            qMin(1.0f, static_cast<float>(face.rect.width() * face.rect.height()) /
                       static_cast<float>(image.width() * image.height()) * 12.0f);
        return quality * 0.6f + det * 0.3f + faceSize * 0.1f;
    }
};

/** @brief MiniFASNet 双模型活体检测结果。 */
struct LivenessResult {
    bool valid = false;            /**< 推理是否成功。 */
    bool passed = false;           /**< realScore 达到配置阈值时为 true。 */
    int label = -1;                /**< 最佳分类标签；-1 表示无结果。 */
    float paperScore = 0.0f;       /**< 纸张攻击类别得分。 */
    float realScore = 0.0f;        /**< 真人类别得分。 */
    float screenScore = 0.0f;      /**< 屏幕攻击类别得分。 */
    QString message;               /**< 推理状态或失败原因。 */
    VerificationSnapshot snapshot; /**< 产生该结果的原始验证快照。 */
};

Q_DECLARE_METATYPE(VerifyState)
Q_DECLARE_METATYPE(PersonInfo)
Q_DECLARE_METATYPE(QVector<PersonInfo>)
Q_DECLARE_METATYPE(FaceFeatureData)
Q_DECLARE_METATYPE(FaceRecord)
Q_DECLARE_METATYPE(QVector<FaceRecord>)
Q_DECLARE_METATYPE(PersonAdminRecord)
Q_DECLARE_METATYPE(QVector<PersonAdminRecord>)
Q_DECLARE_METATYPE(DetectedFace)
Q_DECLARE_METATYPE(QVector<DetectedFace>)
Q_DECLARE_METATYPE(VerifyLog)
Q_DECLARE_METATYPE(VerifyLogViewRecord)
Q_DECLARE_METATYPE(QVector<VerifyLogViewRecord>)
Q_DECLARE_METATYPE(VerifyLogFilter)
Q_DECLARE_METATYPE(OperatorAuditLog)
Q_DECLARE_METATYPE(QVector<OperatorAuditLog>)
Q_DECLARE_METATYPE(SyncTaskRecord)
Q_DECLARE_METATYPE(QVector<SyncTaskRecord>)
Q_DECLARE_METATYPE(SystemEventLog)
Q_DECLARE_METATYPE(QVector<SystemEventLog>)
Q_DECLARE_METATYPE(StorageStats)
Q_DECLARE_METATYPE(LivenessResult)
Q_DECLARE_METATYPE(CameraFrame)
Q_DECLARE_METATYPE(FaceAnalysisResult)

#endif
