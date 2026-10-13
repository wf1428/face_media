/**
 * @file LivenessWorker.cpp
 * @brief MiniFASNet 双模型活体检测后台线程的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "LivenessWorker.h"

#include <QThread>

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef FACEGATE_WITH_RKNN
#include <rknn_api.h>
#endif

namespace {

constexpr int kInputW = 80;
constexpr int kInputH = 80;
constexpr int kClassNum = 3;
constexpr int kPaperIndex = 0;
constexpr int kRealIndex = 1;
constexpr int kScreenIndex = 2;

/** @brief 裁剪人脸区域并缩放、换序为活体模型要求的 80×80 BGR 输入。 */
bool cropResizeRgbToBgr80(const QImage &image, const QRect &faceRect, float scale, std::vector<unsigned char> &bgr)
{
    if (image.isNull() || faceRect.isEmpty()) {
        return false;
    }

    const QRect boundedFace = faceRect.intersected(image.rect());
    if (boundedFace.isEmpty()) {
        return false;
    }

    const QPointF center = boundedFace.center();
    const int cropW = std::max(1, static_cast<int>(boundedFace.width() * scale));
    const int cropH = std::max(1, static_cast<int>(boundedFace.height() * scale));
    QRect crop(static_cast<int>(center.x() - cropW * 0.5f),
               static_cast<int>(center.y() - cropH * 0.5f),
               cropW,
               cropH);
    crop = crop.intersected(image.rect());
    if (crop.isEmpty()) {
        return false;
    }

    QImage rgb = image.copy(crop)
        .scaled(kInputW, kInputH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_RGB888);

    bgr.resize(kInputW * kInputH * 3);
    for (int y = 0; y < kInputH; ++y) {
        const uchar *src = rgb.constScanLine(y);
        for (int x = 0; x < kInputW; ++x) {
            const int si = x * 3;
            const int di = (y * kInputW + x) * 3;
            bgr[di + 0] = src[si + 2];
            bgr[di + 1] = src[si + 1];
            bgr[di + 2] = src[si + 0];
        }
    }

    return true;
}

#ifdef FACEGATE_WITH_RKNN
/** @brief 完整读取模型二进制文件，失败时返回空数据。 */
bool readBinaryFile(const QString &path, std::vector<unsigned char> &data)
{
    std::ifstream ifs(path.toLocal8Bit().constData(), std::ios::binary);
    if (!ifs) {
        return false;
    }

    ifs.seekg(0, std::ios::end);
    const std::streamoff size = ifs.tellg();
    if (size <= 0) {
        return false;
    }

    data.resize(static_cast<size_t>(size));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char *>(data.data()), size);
    return ifs.good();
}

/** @brief 单个 MiniFASNet 模型的一次三分类推理结果。 */
struct ModelResult {
    bool valid = false; /**< 推理输出维度和数据有效时为 true。 */
    std::array<float, kClassNum> prob{{0.0f, 0.0f, 0.0f}}; /**< 三个类别的归一化概率。 */
    QString error; /**< 初始化或推理失败原因。 */
};

/**
 * @brief 单个 MiniFASNet RKNN 模型上下文的资源封装。
 *
 * 模型数据只在初始化阶段使用；RKNN context 在工作线程内推理，并在析构时释放。
 */
class MiniFasnetRknnModel {
public:
    /** @brief 释放仍有效的 RKNN context。 */
    ~MiniFasnetRknnModel()
    {
        release();
    }

    /** @brief 从文件加载模型并创建 RKNN context，同时记录该模型的人脸裁剪尺度。 */
    bool init(const QString &path, float scale, const char *name, QString *error)
    {
        release();
        scale_ = scale;
        name_ = name;

        std::vector<unsigned char> modelData;
        if (!readBinaryFile(path, modelData)) {
            if (error) *error = QString("%1 模型读取失败：%2").arg(name_).arg(path);
            return false;
        }

        const int ret = rknn_init(&ctx_, modelData.data(), static_cast<uint32_t>(modelData.size()), 0, nullptr);
        if (ret != RKNN_SUCC) {
            if (error) *error = QString("%1 rknn_init 失败：%2").arg(name_).arg(ret);
            ctx_ = 0;
            return false;
        }

        ready_ = true;
        return true;
    }

    /** @brief 销毁 RKNN context 并恢复未就绪状态。 */
    void release()
    {
        if (ctx_) {
            rknn_destroy(ctx_);
            ctx_ = 0;
        }
        ready_ = false;
    }

    /** @return RKNN context 已成功初始化时返回 true。 */
    bool ready() const { return ready_; }
    /** @return 该模型要求的人脸区域扩展尺度。 */
    float scale() const { return scale_; }

    /** @brief 对 80×80 BGR 输入执行三分类推理，并确保输出缓冲及时归还 RKNN。 */
    ModelResult infer(const std::vector<unsigned char> &bgr)
    {
        ModelResult result;
        if (!ready_) {
            result.error = name_ + " 未初始化。";
            return result;
        }

        rknn_input input{};
        input.index = 0;
        input.buf = const_cast<unsigned char *>(bgr.data());
        input.size = static_cast<uint32_t>(bgr.size());
        input.pass_through = 0;
        input.type = RKNN_TENSOR_UINT8;
        input.fmt = RKNN_TENSOR_NHWC;

        int ret = rknn_inputs_set(ctx_, 1, &input);
        if (ret != RKNN_SUCC) {
            result.error = QString("%1 rknn_inputs_set 失败：%2").arg(name_).arg(ret);
            return result;
        }

        ret = rknn_run(ctx_, nullptr);
        if (ret != RKNN_SUCC) {
            result.error = QString("%1 rknn_run 失败：%2").arg(name_).arg(ret);
            return result;
        }

        rknn_output output{};
        output.want_float = 1;
        output.is_prealloc = 0;

        ret = rknn_outputs_get(ctx_, 1, &output, nullptr);
        if (ret != RKNN_SUCC) {
            result.error = QString("%1 rknn_outputs_get 失败：%2").arg(name_).arg(ret);
            return result;
        }

        const int elemCount = static_cast<int>(output.size / sizeof(float));
        if (!output.buf || elemCount < kClassNum) {
            result.error = QString("%1 输出维度无效。").arg(name_);
            rknn_outputs_release(ctx_, 1, &output);
            return result;
        }

        const float *out = reinterpret_cast<const float *>(output.buf);
        for (int i = 0; i < kClassNum; ++i) {
            result.prob[i] = std::max(0.0f, std::min(1.0f, out[i]));
        }
        result.valid = true;

        rknn_outputs_release(ctx_, 1, &output);
        return result;
    }

private:
    rknn_context ctx_ = 0;
    bool ready_ = false;
    float scale_ = 1.0f;
    QString name_;
};
#endif

}

/** @brief 隔离可选 RKNN 类型依赖的 LivenessWorker 私有实现。 */
struct LivenessWorker::Impl {
#ifdef FACEGATE_WITH_RKNN
    MiniFasnetRknnModel v2;   /**< MiniFASNetV2 模型实例。 */
    MiniFasnetRknnModel v1se; /**< MiniFASNetV1SE 模型实例。 */
#endif
};

/** @brief 创建尚未加载 RKNN 模型的 worker。 */
LivenessWorker::LivenessWorker(QObject *parent) : QObject(parent)
{
    impl_.reset(new Impl());
}

/** @brief 销毁前请求线程停止并等待退出。 */
LivenessWorker::~LivenessWorker()
{
    stop();
}

/**
 * @brief 加载两个 MiniFASNet RKNN 模型，并启动只保留最新请求的工作线程。
 *
 * 两个 RKNN context 由该工作线程持有，且不会被
 * UI 线程并发使用，从而保持原静默活体线程规则。
 */
bool LivenessWorker::start(const AppConfig &config)
{
    stop();
    config_ = config;

#ifdef FACEGATE_WITH_RKNN
    QString error;
    // 依次初始化两个不同裁剪尺度的 MiniFASNet 模型。
    const bool okV2 = impl_->v2.init(config_.miniFasnetV2Path, 2.7f, "MiniFASNetV2", &error);
    const bool okV1 = okV2 && impl_->v1se.init(config_.miniFasnetV1SePath, 4.0f, "MiniFASNetV1SE", &error);
    ready_ = okV2 && okV1;
    emit workerStatus(ready_ ? "模型加载完成。" : error);
#else
    ready_ = false;
    emit workerStatus("RKNN 未启用；请在 qmake 前设置 RKNN_ROOT。");
#endif

    stopRequested_ = false;
    // 无论模型是否加载成功，都启动线程以统一接收停止信号。
    worker_ = std::thread(&LivenessWorker::workerLoop, this);
    return ready_;
}

/** @brief 更新活体阈值等运行配置。 */
void LivenessWorker::updateConfig(const AppConfig &config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

/**
 * @brief 在应用退出前停止活体推理并释放 RKNN context。
 *
 * join() 前会先通知条件变量，使工作线程即使在
 * 没有待处理帧时也能干净退出等待状态。
 */
void LivenessWorker::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = true;
        hasPending_ = false;
    }
    cv_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }

#ifdef FACEGATE_WITH_RKNN
    if (impl_) {
        impl_->v2.release();
        impl_->v1se.release();
    }
#endif

    ready_ = false;
    stopRequested_ = false;
}

/** @return 两个模型和工作线程已经就绪时返回 true。 */
bool LivenessWorker::isReady() const
{
    return ready_;
}

/**
 * @brief 提交一张已匹配人脸截图用于活体检测。
 *
 * 只保留最新请求。该行为与 face_test_threaded.cpp 一致，
 * 避免 RKNN 推理慢于帧率时堆积过期帧并增加延迟。
 */
void LivenessWorker::submit(const VerificationSnapshot &snapshot)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        /*
         * 只保留最新请求。活体推理可能慢于摄像头 FPS，
         * 闸机只关心当前人员，不关心队列中的过期帧。
         */
        pending_ = snapshot;
        hasPending_ = true;
    }
    cv_.notify_one();
}

/**
 * @brief 工作线程循环主体，负责裁剪、缩放、双模型推理和概率融合。
 */
void LivenessWorker::workerLoop()
{
    while (true) {
        VerificationSnapshot request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // 等待新活体请求或退出请求。
            cv_.wait(lock, [this]() { return stopRequested_ || hasPending_; });
            if (stopRequested_) {
                break;
            }
            request = pending_;
            hasPending_ = false;
        }

        LivenessResult result;
        result.snapshot = request;

#ifdef FACEGATE_WITH_RKNN
        std::vector<unsigned char> inputV2;
        std::vector<unsigned char> inputV1;
        // 按两个模型各自 scale 裁剪同一张人脸。
        if (!cropResizeRgbToBgr80(request.image, request.face.rect, impl_->v2.scale(), inputV2) ||
            !cropResizeRgbToBgr80(request.image, request.face.rect, impl_->v1se.scale(), inputV1)) {
            result.valid = false;
            result.message = "裁剪失败。";
            emit resultReady(result);
            continue;
        }

        // 分别执行双模型推理，再对输出概率做平均融合。
        const ModelResult v2 = impl_->v2.infer(inputV2);
        const ModelResult v1 = impl_->v1se.infer(inputV1);
        if (!v2.valid || !v1.valid) {
            result.valid = false;
            result.message = !v2.valid ? v2.error : v1.error;
            emit resultReady(result);
            continue;
        }

        std::array<float, kClassNum> finalProb{{0.0f, 0.0f, 0.0f}};
        for (int i = 0; i < kClassNum; ++i) {
            finalProb[i] = (v2.prob[i] + v1.prob[i]) * 0.5f;
        }

        int label = 0;
        for (int i = 1; i < kClassNum; ++i) {
            if (finalProb[i] > finalProb[label]) {
                label = i;
            }
        }

        result.valid = true;
        result.label = label;
        result.paperScore = finalProb[kPaperIndex];
        result.realScore = finalProb[kRealIndex];
        result.screenScore = finalProb[kScreenIndex];
        float livenessThreshold = 0.8f;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            livenessThreshold = config_.livenessThreshold;
        }
        result.passed = label == kRealIndex && result.realScore >= livenessThreshold;
        result.message = result.passed ? "真人" : "活体未通过";
#else
        result.valid = false;
        result.message = "RKNN 未启用";
#endif

        emit resultReady(result);
    }

    emit workerStatus("活体工作线程已停止");
}
