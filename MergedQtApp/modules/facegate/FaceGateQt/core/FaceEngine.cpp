/**
 * @file FaceEngine.cpp
 * @brief InspireFace 检测、质量、特征提取和图库匹配封装的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "FaceEngine.h"

#include <QByteArray>
#include <QFileInfo>

#include <algorithm>
#include <cmath>

namespace {

/** @brief 计算两个矩形的交并比，用于匹配连续帧中的同一人脸。 */
float rectOverlapScore(const QRect &a, const QRect &b)
{
    if (a.isEmpty() || b.isEmpty()) {
        return 0.0f;
    }
    const QRect inter = a.intersected(b);
    if (inter.isEmpty()) {
        return 0.0f;
    }
    const float interArea = static_cast<float>(inter.width() * inter.height());
    const float unionArea = static_cast<float>(a.width() * a.height() + b.width() * b.height()) - interArea;
    return unionArea > 0.0f ? interArea / unionArea : 0.0f;
}

}

#ifdef FACEGATE_WITH_INSPIREFACE
#include <herror.h>
#include <inspireface.h>
#endif

/** @brief 隔离可选 InspireFace SDK 类型依赖的引擎私有实现。 */
struct FaceEngine::Impl {
#ifdef FACEGATE_WITH_INSPIREFACE
    HFSession session = nullptr; /**< InspireFace 会话句柄。 */
    bool launched = false;       /**< SDK 资源包已经成功启动。 */
    HOption pipelineOption = HF_ENABLE_FACE_RECOGNITION | HF_ENABLE_QUALITY; /**< 启用识别和质量评估。 */
#endif
};

/** @brief 创建尚未初始化 SDK 的引擎对象。 */
FaceEngine::FaceEngine(QObject *parent) : QObject(parent)
{
    impl_.reset(new Impl());
}

/** @brief 销毁前释放 InspireFace 资源。 */
FaceEngine::~FaceEngine()
{
    shutdown();
}

/**
 * @brief 启动 InspireFace，并创建与 face_test_threaded.cpp 相同的 CPU 预处理会话。
 *
 * 旧测试程序会显式把图像预处理切到 CPU，因为
 * 目标板缺少可用的 dma_heap/ion 节点来分配 InspireCV RGA buffer。这里保持
 * 该调用位于创建会话之前，并保持检测模式和像素级参数不变。
 */
bool FaceEngine::initialize(const AppConfig &config)
{
    config_ = config;

#ifdef FACEGATE_WITH_INSPIREFACE
    shutdown();

    const QByteArray packPath = config_.inspirePackPath.toLocal8Bit();
    // 先加载 InspireFace 资源包，再创建后续 session。
    HResult ret = HFLaunchInspireFace(packPath.constData());
    if (ret != HSUCCEED) {
        emit engineStatus(QString("HFLaunchInspireFace 失败：%1").arg(ret));
        ready_ = false;
        return false;
    }
    impl_->launched = true;

    /*
     * 保持原 face_test_threaded.cpp 的行为：
     * 该板卡没有可用于 InspireCV RGA buffer 的 /dev/dma_heap 或 /dev/ion 路径，
     * 因此图像预处理必须保持在 CPU；RKNN/RKNPU 推理保持不变。
     */
    // 强制使用 CPU 图像预处理，避免目标板缺失 DMA/RGA 节点导致分配失败。
    ret = HFSwitchImageProcessingBackend(HF_IMAGE_PROCESSING_CPU);
    if (ret != HSUCCEED) {
        emit engineStatus(QString("HFSwitchImageProcessingBackend CPU 失败：%1").arg(ret));
        shutdown();
        return false;
    }

    // 创建持续检测模式的 InspireFace session，用于实时识别链路。
    ret = HFCreateInspireFaceSessionOptional(
        impl_->pipelineOption,
        HF_DETECT_MODE_ALWAYS_DETECT,
        5,
        320,
        -1,
        &impl_->session
    );
    if (ret != HSUCCEED) {
        emit engineStatus(QString("HFCreateInspireFaceSessionOptional 失败：%1").arg(ret));
        shutdown();
        return false;
    }

    HFSessionSetTrackPreviewSize(impl_->session, 320);
    HFSessionSetFilterMinimumFacePixelSize(impl_->session, 60);
    HFSessionSetFaceDetectThreshold(impl_->session, config_.faceDetectThreshold);

    ready_ = true;
    emit engineStatus("InspireFace 会话已就绪");
#else
    ready_ = false;
    emit engineStatus("InspireFace SDK 未启用；请在 qmake 前设置 INSPIREFACE_ROOT");
#endif

    return ready_;
}

/** @brief 更新不需要重建 SDK 会话的运行阈值配置。 */
void FaceEngine::updateConfig(const AppConfig &config)
{
    config_ = config;
#ifdef FACEGATE_WITH_INSPIREFACE
    if (impl_ && impl_->session) {
        HFSessionSetFaceDetectThreshold(impl_->session, config_.faceDetectThreshold);
    }
#endif
}

/** @brief 释放全部 SDK 会话和资源。 */
void FaceEngine::shutdown()
{
#ifdef FACEGATE_WITH_INSPIREFACE
    /*
     * 释放顺序与测试程序一致：先释放 session 句柄，再释放全局
     * InspireFace runtime。底库特征保存为 QByteArray，不需要 SDK 释放。
     */
    if (impl_ && impl_->session) {
        HFReleaseInspireFaceSession(impl_->session);
        impl_->session = nullptr;
    }
    if (impl_ && impl_->launched) {
        HFTerminateInspireFace();
        impl_->launched = false;
    }
#endif
    ready_ = false;
}

/** @return SDK 初始化完成且会话可用时返回 true。 */
bool FaceEngine::isReady() const
{
    return ready_;
}

/**
 * @brief 替换用于一对一特征比对的内存底库。
 *
 * 数据库线程拥有 MySQL 访问权。FaceEngine 只保存拷贝后的 QByteArray
 * 特征数据，避免识别流程阻塞在数据库读取上。
 */
void FaceEngine::setGallery(const QVector<FaceRecord> &records)
{
    localGallery_ = records;
    rebuildRecognitionGallery();
}

/**
 * @brief 替换网络人员图库并重建统一识别快照。
 *
 * 只保存数据库线程提供的值副本，避免推理过程跨线程访问数据库对象。
 */
void FaceEngine::setNetworkGallery(const QVector<FaceRecord> &records)
{
    networkGallery_ = records;
    rebuildRecognitionGallery();
}

/** @brief 合并本地与网络图库，使单帧识别只遍历一个稳定快照。 */
void FaceEngine::rebuildRecognitionGallery()
{
    gallery_ = localGallery_;
    gallery_.reserve(localGallery_.size() + networkGallery_.size());
    for (const FaceRecord &record : networkGallery_) {
        gallery_.append(record);
    }
}


/**
 * @brief 只做人脸检测和质量评估，不提取身份特征，也不进行底库比对。
 *
 * 管理员录入预览使用该路径，因此打开管理员页面时不会让
 * 主闸机识别链路继续在后台运行。它只保留人脸框和
 * 用于活体的质量反馈，同时避免底库比对延迟和日志副作用。
 */
FaceAnalysisResult FaceEngine::detectFrame(const CameraFrame &frame)
{
    FaceAnalysisResult result;
    result.sdkReady = ready_;

    if (!ready_) {
        result.message = "FaceEngine 未就绪";
        return result;
    }

#ifdef FACEGATE_WITH_INSPIREFACE
    if (frame.image.isNull() || frame.yuv420sp.isEmpty() || !impl_ || !impl_->session) {
        result.message = "无效图像帧或 InspireFace 会话";
        return result;
    }

    // 直接把摄像头连续 YUV420SP 数据交给 InspireFace，保持原输入链路。
    HFImageData imageData{};
    imageData.data = reinterpret_cast<HPUInt8>(const_cast<char *>(frame.yuv420sp.constData()));
    imageData.width = frame.width;
    imageData.height = frame.height;
    imageData.format = frame.nv21 ? HF_STREAM_YUV_NV21 : HF_STREAM_YUV_NV12;
    imageData.rotation = HF_CAMERA_ROTATION_0;

    HFImageStream stream = nullptr;
    HResult ret = HFCreateImageStream(&imageData, &stream);
    if (ret != HSUCCEED) {
        result.message = QString("HFCreateImageStream 失败：%1").arg(ret);
        return result;
    }

    // 执行人脸跟踪，得到当前帧全部候选人脸。
    HFMultipleFaceData faces{};
    ret = HFExecuteFaceTrack(impl_->session, stream, &faces);
    if (ret != HSUCCEED) {
        result.message = QString("HFExecuteFaceTrack 失败：%1").arg(ret);
        HFReleaseImageStream(stream);
        return result;
    }

    if (faces.detectedNum <= 0) {
        result.message = "未检测到人脸";
        HFReleaseImageStream(stream);
        return result;
    }

    HResult pipeRet = HFMultipleFacePipelineProcessOptional(impl_->session, stream, &faces, HF_ENABLE_QUALITY);
    HFFaceQualityConfidence quality{};
    HResult qualityRet = HFGetFaceQualityConfidence(impl_->session, &quality);

    // 遍历检测结果，计算质量和底库匹配信息。
    for (int i = 0; i < faces.detectedNum; ++i) {
        DetectedFace item;
        const HFaceRect r = faces.rects[i];
        item.rect = QRect(r.x, r.y, r.width, r.height);
        item.detConfidence = faces.detConfidence ? faces.detConfidence[i] : -1.0f;
        item.qualityValid = pipeRet == HSUCCEED && qualityRet == HSUCCEED && quality.confidence && i < quality.num;
        item.quality = item.qualityValid ? quality.confidence[i] : -1.0f;
        const bool rectInside = frame.image.rect().contains(item.rect);
        item.faceUsable =
            rectInside &&
            item.detConfidence >= config_.faceDetectThreshold &&
            item.qualityValid &&
            item.quality >= config_.faceQualityThreshold;
        item.galleryEmpty = gallery_.isEmpty();
        item.bestCandidateValid = false;
        item.matched = false;
        item.cosine = -1.0f;
        result.faces.push_back(item);
    }

    HFReleaseImageStream(stream);
    result.valid = !result.faces.isEmpty();
    result.message = result.valid ? "已检测到人脸" : "没有可用人脸";
#endif

    result.valid = !result.faces.isEmpty();
    return result;
}

/**
 * @brief 使用与 face_test_threaded.cpp 相同的 YUV420SP SDK 输入路径分析一帧摄像头图像。
 *
 * RGB QImage 只用于 Qt 预览和 MiniFASNet 裁剪。InspireFace 接收
 * 从 V4L2 mmap 拷贝出的连续 NV12/NV21 buffer，从而避开可能触发
 * RGA DMA 分配的 RGB 转换路径，适配没有 /dev/dma_heap 的板卡。
 */
FaceAnalysisResult FaceEngine::analyzeFrame(const CameraFrame &frame)
{
    FaceAnalysisResult result;
    result.sdkReady = ready_;

    if (!ready_) {
        result.message = "FaceEngine 未就绪";
        return result;
    }

#ifdef FACEGATE_WITH_INSPIREFACE
    if (frame.image.isNull() || frame.yuv420sp.isEmpty() || !impl_ || !impl_->session) {
        result.message = "无效图像帧或 InspireFace 会话";
        return result;
    }

    // 直接把摄像头连续 YUV420SP 数据交给 InspireFace，保持原输入链路。
    HFImageData imageData{};
    imageData.data = reinterpret_cast<HPUInt8>(const_cast<char *>(frame.yuv420sp.constData()));
    imageData.width = frame.width;
    imageData.height = frame.height;
    imageData.format = frame.nv21 ? HF_STREAM_YUV_NV21 : HF_STREAM_YUV_NV12;
    imageData.rotation = HF_CAMERA_ROTATION_0;

    HFImageStream stream = nullptr;
    HResult ret = HFCreateImageStream(&imageData, &stream);
    if (ret != HSUCCEED) {
        result.message = QString("HFCreateImageStream 失败：%1").arg(ret);
        return result;
    }

    // 执行人脸跟踪，得到当前帧全部候选人脸。
    HFMultipleFaceData faces{};
    ret = HFExecuteFaceTrack(impl_->session, stream, &faces);
    if (ret != HSUCCEED) {
        result.message = QString("HFExecuteFaceTrack 失败：%1").arg(ret);
        HFReleaseImageStream(stream);
        return result;
    }

    if (faces.detectedNum <= 0) {
        result.message = "未检测到人脸";
        HFReleaseImageStream(stream);
        return result;
    }

    HResult pipeRet = HFMultipleFacePipelineProcessOptional(impl_->session, stream, &faces, impl_->pipelineOption);
    HFFaceQualityConfidence quality{};
    HResult qualityRet = HFGetFaceQualityConfidence(impl_->session, &quality);

    // 遍历检测结果，计算质量和底库匹配信息。
    for (int i = 0; i < faces.detectedNum; ++i) {
        DetectedFace item;
        const HFaceRect r = faces.rects[i];
        item.rect = QRect(r.x, r.y, r.width, r.height);
        item.detConfidence = faces.detConfidence ? faces.detConfidence[i] : -1.0f;
        item.qualityValid = pipeRet == HSUCCEED && qualityRet == HSUCCEED && quality.confidence && i < quality.num;
        item.quality = item.qualityValid ? quality.confidence[i] : -1.0f;

        const bool rectInside = frame.image.rect().contains(item.rect);
        const bool faceUsable =
            rectInside &&
            item.detConfidence >= config_.faceDetectThreshold &&
            item.qualityValid &&
            item.quality >= config_.faceQualityThreshold;
        item.faceUsable = faceUsable;
        item.galleryEmpty = gallery_.isEmpty();

        HFFaceFeature currentFeature{};
        ret = HFCreateFaceFeature(&currentFeature);
        if (ret == HSUCCEED) {
            ret = HFFaceFeatureExtractTo(impl_->session, stream, faces.tokens[i], currentFeature);
            if (ret == HSUCCEED && faceUsable) {
                float bestCosine = -1.0f;
                int bestIndex = -1;

                for (int g = 0; g < gallery_.size(); ++g) {
                    const QByteArray &blob = gallery_[g].feature.blob;
                    if (blob.isEmpty()) {
                        continue;
                    }

                    HFFaceFeature galleryFeature{};
                    galleryFeature.size = blob.size() / static_cast<int>(sizeof(float));
                    galleryFeature.data = reinterpret_cast<HPFloat>(const_cast<char *>(blob.constData()));

                    HFloat cosine = 0.0f;
                    HResult cmpRet = HFFaceComparison(galleryFeature, currentFeature, &cosine);
                    if (cmpRet == HSUCCEED && cosine > bestCosine) {
                        bestCosine = cosine;
                        bestIndex = g;
                    }
                }

                item.cosine = bestCosine;
                item.bestCandidateValid = bestIndex >= 0;
                item.matched = bestIndex >= 0 && bestCosine >= config_.faceCosineThreshold;
                if (bestIndex >= 0) {
                    item.person = gallery_[bestIndex].person;
                    item.faceHash = gallery_[bestIndex].faceHash;
                }
            }

            HFReleaseFaceFeature(&currentFeature);
        }

        result.faces.push_back(item);
    }

    HFReleaseImageStream(stream);
    result.valid = !result.faces.isEmpty();
    result.message = result.valid ? "人脸分析完成" : "没有可用人脸";
#endif

    result.valid = !result.faces.isEmpty();
    return result;
}


/**
 * @brief 对已经通过活体检测的最佳帧执行特征提取和底库比对。
 *
 * 主闸机链路必须先完成活体检测，确认是真人人脸后，才允许进入
 * 身份识别和底库相似度比较。这样非活体人员不会因为底库相似度低
 * 被误归类为“未录入人员”。
 */
bool FaceEngine::matchSnapshot(const VerificationSnapshot &snapshot, DetectedFace *matchedFace, QString *errorText)
{
    if (matchedFace) {
        *matchedFace = snapshot.face;
        matchedFace->galleryEmpty = gallery_.isEmpty();
        matchedFace->matched = false;
        matchedFace->bestCandidateValid = false;
        matchedFace->cosine = -1.0f;
    }

    if (!ready_) {
        if (errorText) {
            *errorText = QStringLiteral("FaceEngine 未就绪");
        }
        return false;
    }

#ifdef FACEGATE_WITH_INSPIREFACE
    if (snapshot.image.isNull() || snapshot.yuv420sp.isEmpty() ||
        snapshot.width <= 0 || snapshot.height <= 0 || !impl_ || !impl_->session) {
        if (errorText) {
            *errorText = QStringLiteral("无效活体帧，无法进行身份识别");
        }
        return false;
    }

    HFImageData imageData{};
    imageData.data = reinterpret_cast<HPUInt8>(const_cast<char *>(snapshot.yuv420sp.constData()));
    imageData.width = snapshot.width;
    imageData.height = snapshot.height;
    imageData.format = snapshot.nv21 ? HF_STREAM_YUV_NV21 : HF_STREAM_YUV_NV12;
    imageData.rotation = HF_CAMERA_ROTATION_0;

    HFImageStream stream = nullptr;
    HResult ret = HFCreateImageStream(&imageData, &stream);
    if (ret != HSUCCEED) {
        if (errorText) {
            *errorText = QStringLiteral("HFCreateImageStream 失败：%1").arg(ret);
        }
        return false;
    }

    HFMultipleFaceData faces{};
    ret = HFExecuteFaceTrack(impl_->session, stream, &faces);
    if (ret != HSUCCEED || faces.detectedNum <= 0) {
        if (errorText) {
            *errorText = ret != HSUCCEED
                ? QStringLiteral("HFExecuteFaceTrack 失败：%1").arg(ret)
                : QStringLiteral("活体通过后未检测到可识别人脸");
        }
        HFReleaseImageStream(stream);
        return false;
    }

    HResult pipeRet = HFMultipleFacePipelineProcessOptional(impl_->session, stream, &faces, impl_->pipelineOption);
    HFFaceQualityConfidence quality{};
    HResult qualityRet = HFGetFaceQualityConfidence(impl_->session, &quality);

    int bestFaceIndex = 0;
    float bestFaceScore = -1.0f;
    for (int i = 0; i < faces.detectedNum; ++i) {
        const HFaceRect r = faces.rects[i];
        const QRect rect(r.x, r.y, r.width, r.height);
        const float overlap = rectOverlapScore(rect, snapshot.face.rect);
        const float det = faces.detConfidence ? faces.detConfidence[i] : 0.0f;
        const bool qualityValid = pipeRet == HSUCCEED && qualityRet == HSUCCEED && quality.confidence && i < quality.num;
        const float qualityScore = qualityValid ? quality.confidence[i] : 0.0f;
        const float score = overlap * 2.0f + det + qualityScore;
        if (score > bestFaceScore) {
            bestFaceScore = score;
            bestFaceIndex = i;
        }
    }

    DetectedFace item = snapshot.face;
    const HFaceRect r = faces.rects[bestFaceIndex];
    item.rect = QRect(r.x, r.y, r.width, r.height);
    item.detConfidence = faces.detConfidence ? faces.detConfidence[bestFaceIndex] : -1.0f;
    item.qualityValid = pipeRet == HSUCCEED && qualityRet == HSUCCEED && quality.confidence && bestFaceIndex < quality.num;
    item.quality = item.qualityValid ? quality.confidence[bestFaceIndex] : -1.0f;
    item.galleryEmpty = gallery_.isEmpty();
    item.bestCandidateValid = false;
    item.matched = false;
    item.cosine = -1.0f;
    item.person = PersonInfo();

    const bool rectInside = snapshot.image.rect().contains(item.rect);
    item.faceUsable =
        rectInside &&
        item.detConfidence >= config_.faceDetectThreshold &&
        item.qualityValid &&
        item.quality >= config_.faceQualityThreshold;

    if (!item.faceUsable) {
        if (matchedFace) {
            *matchedFace = item;
        }
        if (errorText) {
            *errorText = QStringLiteral("验证失败，请正对摄像头");
        }
        HFReleaseImageStream(stream);
        return false;
    }

    HFFaceFeature currentFeature{};
    ret = HFCreateFaceFeature(&currentFeature);
    if (ret != HSUCCEED) {
        if (matchedFace) {
            *matchedFace = item;
        }
        if (errorText) {
            *errorText = QStringLiteral("HFCreateFaceFeature 失败：%1").arg(ret);
        }
        HFReleaseImageStream(stream);
        return false;
    }

    ret = HFFaceFeatureExtractTo(impl_->session, stream, faces.tokens[bestFaceIndex], currentFeature);
    if (ret != HSUCCEED) {
        if (matchedFace) {
            *matchedFace = item;
        }
        if (errorText) {
            *errorText = QStringLiteral("HFFaceFeatureExtractTo 失败：%1").arg(ret);
        }
        HFReleaseFaceFeature(&currentFeature);
        HFReleaseImageStream(stream);
        return false;
    }

    float bestCosine = -1.0f;
    int bestIndex = -1;
    for (int g = 0; g < gallery_.size(); ++g) {
        const QByteArray &blob = gallery_[g].feature.blob;
        if (blob.isEmpty()) {
            continue;
        }

        HFFaceFeature galleryFeature{};
        galleryFeature.size = blob.size() / static_cast<int>(sizeof(float));
        galleryFeature.data = reinterpret_cast<HPFloat>(const_cast<char *>(blob.constData()));

        HFloat cosine = 0.0f;
        HResult cmpRet = HFFaceComparison(galleryFeature, currentFeature, &cosine);
        if (cmpRet == HSUCCEED && cosine > bestCosine) {
            bestCosine = cosine;
            bestIndex = g;
        }
    }

    item.cosine = bestCosine;
    item.bestCandidateValid = bestIndex >= 0;
    item.matched = bestIndex >= 0 && bestCosine >= config_.faceCosineThreshold;
    if (bestIndex >= 0) {
        item.person = gallery_[bestIndex].person;
        item.faceHash = gallery_[bestIndex].faceHash;
    }

    if (matchedFace) {
        *matchedFace = item;
    }
    if (!item.matched && errorText) {
        *errorText = item.bestCandidateValid
            ? QStringLiteral("验证失败，请正对摄像头")
            : QStringLiteral("未录入人员");
    }

    HFReleaseFaceFeature(&currentFeature);
    HFReleaseImageStream(stream);
    return item.matched;
#else
    Q_UNUSED(snapshot)
    if (errorText) {
        *errorText = QStringLiteral("InspireFace SDK 未启用");
    }
    return false;
#endif
}


/**
 * @brief 将新录入特征与本地离线底库比对，SDK 可用时使用 SDK 比较器。
 *
 * 该接口用于管理员录入时的重复检查，保持原有本地人员管理语义；正常身份
 * 识别由 analyzeFrame/matchSnapshot 使用合并后的本地与网络图库。
 */
bool FaceEngine::findSimilarFace(const FaceFeatureData &feature, float threshold, FaceRecord *matchedRecord, float *bestCosine) const
{
    if (bestCosine) {
        *bestCosine = -1.0f;
    }
    if (matchedRecord) {
        *matchedRecord = FaceRecord();
    }
    if (feature.blob.isEmpty() || localGallery_.isEmpty()) {
        return false;
    }

    float best = -1.0f;
    int bestIndex = -1;

#ifdef FACEGATE_WITH_INSPIREFACE
    HFFaceFeature currentFeature{};
    currentFeature.size = feature.blob.size() / static_cast<int>(sizeof(float));
    currentFeature.data = reinterpret_cast<HPFloat>(const_cast<char *>(feature.blob.constData()));
#endif

    for (int i = 0; i < localGallery_.size(); ++i) {
        const QByteArray &blob = localGallery_[i].feature.blob;
        if (blob.isEmpty() || blob.size() != feature.blob.size()) {
            continue;
        }

        float cosine = -1.0f;
#ifdef FACEGATE_WITH_INSPIREFACE
        HFFaceFeature galleryFeature{};
        galleryFeature.size = blob.size() / static_cast<int>(sizeof(float));
        galleryFeature.data = reinterpret_cast<HPFloat>(const_cast<char *>(blob.constData()));

        HFloat sdkCosine = 0.0f;
        HResult cmpRet = HFFaceComparison(galleryFeature, currentFeature, &sdkCosine);
        if (cmpRet == HSUCCEED) {
            cosine = sdkCosine;
        }
#else
        const int count = qMin(blob.size(), feature.blob.size()) / static_cast<int>(sizeof(float));
        const float *l = reinterpret_cast<const float *>(blob.constData());
        const float *r = reinterpret_cast<const float *>(feature.blob.constData());
        double dot = 0.0;
        double leftNorm = 0.0;
        double rightNorm = 0.0;
        for (int j = 0; j < count; ++j) {
            dot += static_cast<double>(l[j]) * static_cast<double>(r[j]);
            leftNorm += static_cast<double>(l[j]) * static_cast<double>(l[j]);
            rightNorm += static_cast<double>(r[j]) * static_cast<double>(r[j]);
        }
        if (leftNorm > 0.0 && rightNorm > 0.0) {
            cosine = static_cast<float>(dot / (std::sqrt(leftNorm) * std::sqrt(rightNorm)));
        }
#endif

        if (cosine > best) {
            best = cosine;
            bestIndex = i;
        }
    }

    if (bestCosine) {
        *bestCosine = best;
    }
    if (bestIndex >= 0 && best >= threshold) {
        if (matchedRecord) {
            *matchedRecord = localGallery_[bestIndex];
        }
        return true;
    }
    return false;
}

/**
 * @brief 从保存的截图图片中提取一条录入人脸特征。
 *
 * 这里保留原注册辅助函数行为，并增加一个闸机专用限制：
 * 人脸质量和检测置信度必须通过配置阈值，
 * 特征才允许写入数据库。
 */
bool FaceEngine::extractFeatureFromImage(const QString &imagePath,
                                         FaceFeatureData &feature,
                                         QString *errorText,
                                         float *detectConfidenceOut)
{
    if (detectConfidenceOut) *detectConfidenceOut = -1.0f;
    if (!ready_) {
        if (errorText) {
            *errorText = "FaceEngine 未就绪";
        }
        return false;
    }

#ifdef FACEGATE_WITH_INSPIREFACE
    if (!QFileInfo::exists(imagePath)) {
        if (errorText) {
            *errorText = "图片文件不存在";
        }
        return false;
    }

    const QByteArray path = imagePath.toLocal8Bit();
    HFImageBitmap imageBitmap = nullptr;
    HFImageStream imageStream = nullptr;

    // 从录入截图创建 SDK bitmap，用于离线特征提取。
    HResult ret = HFCreateImageBitmapFromFilePath(path.constData(), 3, &imageBitmap);
    if (ret != HSUCCEED) {
        if (errorText) *errorText = QString("HFCreateImageBitmapFromFilePath 失败：%1").arg(ret);
        return false;
    }

    ret = HFCreateImageStreamFromImageBitmap(imageBitmap, HF_CAMERA_ROTATION_0, &imageStream);
    if (ret != HSUCCEED) {
        if (errorText) *errorText = QString("HFCreateImageStreamFromImageBitmap 失败：%1").arg(ret);
        HFReleaseImageBitmap(imageBitmap);
        return false;
    }

    HFMultipleFaceData faces{};
    ret = HFExecuteFaceTrack(impl_->session, imageStream, &faces);
    if (ret != HSUCCEED || faces.detectedNum <= 0) {
        if (errorText) *errorText = ret != HSUCCEED ? QString("HFExecuteFaceTrack 失败：%1").arg(ret) : "未检测到人脸";
        HFReleaseImageStream(imageStream);
        HFReleaseImageBitmap(imageBitmap);
        return false;
    }

    if (faces.detectedNum != 1) {
        if (errorText) {
            *errorText = "录入画面中请只保留一张人脸";
        }
        HFReleaseImageStream(imageStream);
        HFReleaseImageBitmap(imageBitmap);
        return false;
    }

    const float detConfidence = faces.detConfidence ? faces.detConfidence[0] : -1.0f;
    if (detectConfidenceOut) *detectConfidenceOut = detConfidence;
    HResult pipeRet = HFMultipleFacePipelineProcessOptional(impl_->session, imageStream, &faces, impl_->pipelineOption);
    HFFaceQualityConfidence quality{};
    HResult qualityRet = HFGetFaceQualityConfidence(impl_->session, &quality);
    const bool qualityValid =
        pipeRet == HSUCCEED &&
        qualityRet == HSUCCEED &&
        quality.confidence &&
        quality.num > 0;
    const float qualityScore = qualityValid ? quality.confidence[0] : -1.0f;

    if (detConfidence < config_.faceDetectThreshold) {
        if (errorText) {
            *errorText = QStringLiteral("人脸识别质量过低，请重新录入人脸。");
        }
        HFReleaseImageStream(imageStream);
        HFReleaseImageBitmap(imageBitmap);
        return false;
    }
    if (!qualityValid || qualityScore < config_.faceQualityThreshold) {
        if (errorText) {
            *errorText = QStringLiteral("人脸质量过低，请重新录入人脸。");
        }
        HFReleaseImageStream(imageStream);
        HFReleaseImageBitmap(imageBitmap);
        return false;
    }

    HFFaceFeature sdkFeature{};
    ret = HFCreateFaceFeature(&sdkFeature);
    if (ret != HSUCCEED) {
        if (errorText) *errorText = QString("HFCreateFaceFeature 失败：%1").arg(ret);
        HFReleaseImageStream(imageStream);
        HFReleaseImageBitmap(imageBitmap);
        return false;
    }

    // 对唯一人脸提取特征，后续写入数据库。
    ret = HFFaceFeatureExtractTo(impl_->session, imageStream, faces.tokens[0], sdkFeature);
    if (ret != HSUCCEED) {
        if (errorText) *errorText = QString("HFFaceFeatureExtractTo 失败：%1").arg(ret);
        HFReleaseFaceFeature(&sdkFeature);
        HFReleaseImageStream(imageStream);
        HFReleaseImageBitmap(imageBitmap);
        return false;
    }

    feature.blob = QByteArray(reinterpret_cast<const char *>(sdkFeature.data), sdkFeature.size * static_cast<int>(sizeof(float)));
    feature.modelVersion = "InspireFace";
    feature.quality = qualityScore;

    HFReleaseFaceFeature(&sdkFeature);
    HFReleaseImageStream(imageStream);
    HFReleaseImageBitmap(imageBitmap);
    return true;
#else
    Q_UNUSED(imagePath)
    Q_UNUSED(feature)
    Q_UNUSED(detectConfidenceOut)
    if (errorText) {
        *errorText = "InspireFace SDK 未启用";
    }
    return false;
#endif
}
