/**
 * @file gst_player_widget.h
 * @brief 实现 GStreamer 播放管线、三槽帧邮箱和 Qt OpenGL 视频画布。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-04；修改人：Du lin；新增按需tryLock复用NV12邮箱值和UI画布查询，截图不触发GL读回、不延长播放租约、不修改管线/录放。
 */

#ifndef GST_PLAYER_WIDGET_H
#define GST_PLAYER_WIDGET_H

#include <array>
#include <atomic>
#include <functional>

#include <QAtomicInt>
#include <QAtomicInteger>
#include <QByteArray>
#include <QColor>
#include <atomic>
#include <memory>
#include "common/serial_work_queue.h"
#include "common/snapshot_scene.h"
#include <QElapsedTimer>
#include <QImage>
#include <QHash>
#include <QMutex>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QPointer>
#include <QRect>
#include <QScopedPointer>
#include <QTimer>
#include <QWidget>

#include <gst/gst.h>
#include <gst/video/video.h>

/**
 * @brief 由 Qt/OpenGL 完成最终合成的 NV12 视频画布。
 *
 * GStreamer/MPP 只负责解码，NV12 帧由本控件通过 OpenGL ES 完成颜色转换和缩放，
 * 使视频、弹窗、提示条和软件光标处于同一个 Qt 场景。
 */
class VideoHoleWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
public:
    /** @brief 创建 NV12 OpenGL 画布和渲染诊断定时器。 */
    explicit VideoHoleWidget(QWidget *parent = nullptr);
    /** @brief 在有效上下文中释放 GL 资源和待消费帧。 */
    ~VideoHoleWidget() override;

    /** @brief 提交一帧 NV12 数据，并在画布不再引用该帧时调用释放回调。 */
    void setNv12Frame(int width,
                      int height,
                      const QByteArray &nv12Frame,
                      const std::function<void()> &frameConsumedCallback);
    /** @brief 清除待显示帧并释放关联的帧租约。 */
    void clearVideoFrame();
    /** @return 当前画布抓取的RGB快照；原UI抓取耗时单独统计，后台编码不在此范围。
     * @note 沿用原GUI线程调用约束，不因诊断改变抓取方式。
     */
    QImage videoSnapshot();

    /** @brief 兼容旧 TPlayer 画布接口；OpenGL 画布不使用测试颜色。 */
    void setTestColor(const QColor &) {}

protected:
    /** @brief 初始化着色器、纹理和顶点缓冲。 */
    void initializeGL() override;
    /** @brief 上传NV12并绘制；通话期间统计完整UI作用域及嵌套上传耗时，二者不可相加。 */
    void paintGL() override;

private:
    /** @brief 在有效 OpenGL 上下文中释放纹理、缓冲和着色器。 */
    void releaseGlResources();
    /** @brief 执行当前帧消费回调，允许邮箱槽位重新使用。 */
    void releasePendingFrameLease();
    /** @brief 延迟创建并校验 NV12 转换着色器。 */
    bool ensureShaderProgram();

    QScopedPointer<QOpenGLShaderProgram> program_;
    GLuint yTexture_ = 0;
    GLuint uvTexture_ = 0;
    GLuint vertexBuffer_ = 0;
    QByteArray nv12Frame_;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
    int textureWidth_ = 0;
    int textureHeight_ = 0;
    bool textureUploadPending_ = false;
    bool useRedGreenTextures_ = false;
    bool firstFrameLogged_ = false;
    bool firstPaintLogged_ = false;
    bool firstRenderedFrameLogged_ = false;
    std::function<void()> frameConsumedCallback_;

    quint64 submittedFrames_ = 0;
    quint64 paintedFrames_ = 0;
    quint64 uploadedFrames_ = 0;
    quint64 swappedFrames_ = 0;
    qint64 lastSubmitMs_ = 0;
    qint64 lastPaintMs_ = 0;
    qint64 lastSwapMs_ = 0;
    qint64 maxSubmitGapMs_ = 0;
    qint64 maxPaintGapMs_ = 0;
    qint64 maxSwapGapMs_ = 0;
    qint64 maxPaintUs_ = 0;
    qint64 maxUploadUs_ = 0;
};

/**
 * @brief 基于 GStreamer playbin 的媒体播放器及 Qt 合成输出适配器。
 *
 * 既支持硬件 overlay，也支持 appsink + RGA + OpenGL 的 Qt 合成路径。GStreamer
 * 回调线程只写入三槽帧邮箱，GUI 线程合并投递最新帧，以避免解码线程阻塞。
 */
class GstPlayerWidget : public QWidget
{
    Q_OBJECT
public:
    /** @brief 创建原管线轮询、位置和诊断定时器，并命名供UI长事件定位。
     * @param[in] parent 原QWidget父对象，可为空，不改变所有权。
     */
    explicit GstPlayerWidget(QWidget *parent = nullptr);
    /** @brief 阻止新回调、等待后台停止后释放回调目标；仅最终析构允许等待，不用于正常停止/切源。 */
    ~GstPlayerWidget() override;

    /** @brief 在音频后台线程切换通话独占模式，释放/恢复共享 ALSA 输出。
     * @param[in] exclusive true 丢弃媒体和提示 PCM 并关闭共享 sink；false 恢复现有分支。
     * @return true 已释放声卡或成功提交恢复；false 为共享管线状态切换失败。
     * @note 不触碰视频 playbin；不得从 UI 线程调用；释放时最多等待 1 秒状态确认，切换本身耗时由 GStreamer 决定。
     */
    static bool setCallAudioExclusive(bool exclusive);

    /** @brief 指定承载视频画布或硬件 overlay 的目标控件。 */
    void setVideoOutput(QWidget *widget);
    /** @brief 设置本地路径或网络URL并使旧源失效；耗时停止、切源和预卷在后台串行执行。
     * @param[in] path 原媒体路径或URL，沿用既有校验及状态转换。
     * @note UI立即使旧源失效；后台串行NULL/URI/PAUSED，过期结果不重启定时器或报错。
     */
    void setMedia(const QString &path);
    /** @brief 异步请求播放；与切源/停止串行，预加载或提示音准备未完成时沿原prepared路径继续。 */
    void play();
    /** @brief UI立即撤销播放意图，后台串行暂停并移除普通媒体混音分支。 */
    void pause();
    /** @brief 立即清除画布和播放意图，异步停止；不等待GStreamer。 */
    void stop();
    /** @brief 排队完整释放并重建 playbin；后续 setMedia/play 按序执行。
     * @return true 表示重建请求已受理；异步创建失败通过 errorOccured 报告。
     */
    bool recreatePipeline();
    /** @brief 串行提交当前源的跳转请求，切源后不执行旧请求。
     * @param[in] ms 非负毫秒；负值忽略。
     */
    void seekMs(int ms);

    /** @return playbin 已成功创建时返回 true。 */
    bool isAvailable() const { return playbin_ != nullptr; }
    /** @return 已设置媒体且正在准备、已准备或请求播放时返回 true。 */
    bool hasActiveMedia() const
    {
        return !mediaPath_.isEmpty() && (playRequested_ || prepared_ || playing_);
    }
    /** @return 管线当前处于 PLAYING 状态。 */
    bool isPlaying() const { return playing_; }
    /** @return 当前播放器绑定了视频画布，而不是纯音频提示播放器。 */
    bool hasVideoOutput() const { return !videoWidget_.isNull(); }

    /** @brief UI更新音量缓存并串行提交playbin属性，预卷/播放前应用当前有效值。
     * @param[in] value01 输入限制到0.0～1.0；0为媒体静音。
     */
    void setVolume(float value01);
    /** @return 当前归一化音量。 */
    float volume() const { return volume01_; }
    /** @brief 为一个播报所有者设置临时压低系数；多个播报重叠时自动叠加。 */
    void setAudioDucking(const QObject *owner, float factor01);
    /** @brief 释放一个播报所有者的临时压低，不影响其他仍在播报的所有者。 */
    void clearAudioDucking(const QObject *owner);
    /** @brief 启用短提示音静音预卷，使共享音频链路在首个语音采样前就绪。 */
    void setPromptAudioMode(bool on);

    /** @brief 改变预加载角色并提交不可合并的旧图销毁屏障。
     * @param[in] on true只准备，false为正常输出；角色不变时不重建。
     * @note UI控件设置保持UI归属，后续setMedia/play排在销毁屏障之后。
     */
    void setPreloadMode(bool on);
    /** @return 管线已完成异步准备时返回 true。 */
    bool isPrepared() const { return prepared_; }

    /** @return 最近视频帧到达的单调时钟毫秒值。 */
    qint64 lastVideoFrameMSecs() const { return lastVideoFrameMs_.loadAcquire(); }
    /** @return 最近音频帧到达的单调时钟毫秒值。 */
    qint64 lastAudioFrameMSecs() const { return lastAudioFrameMs_.loadAcquire(); }
    /** @return 最近一个压缩视频 buffer 进入解码器的单调时钟毫秒值。 */
    qint64 lastDecoderInputMSecs() const { return decoderInputLastBufferMs_.loadAcquire(); }

    /** @brief 设置配置文件指定的播放区域；rect 使用目标控件父窗口坐标。 */
    void setDisplayRect(const QRect &rect);
    /** @brief 异步请求重新计算硬件 overlay 或 Qt 画布区域。 */
    void requestDisplayRectUpdate();
    /** @brief 记录源视频尺寸，用于保持宽高比。 */
    void setSourceVideoSize(int width, int height);
    /** @brief 请求从最新解码样本生成一张快照。 */
    void requestSnapshot();
    /** @return 当前是否由Qt合成视频；UI只读，不查询管线/设备。普通截图可直接包含画布。 */
    bool snapshotIncludesVideo() const { return useQtCompositedVideo(); }
    /** @brief GUI按需复用最新可安全读取的NV12值，不查询管线或等待锁。
     * @return 有效帧持有只读副本引用；锁忙/切源/无帧返回空；不改变正常播放租约。
     * @note 只在GUI调用，已发布但未绘制帧可能比当前画布早一帧；后续解码写入通过QByteArray分离。
     */
    SnapshotVideoFrame snapshotVideoFrame() const;
    /** @return GUI视频画布的借用指针；不交后台，非Qt输出可为空。 */
    QWidget *snapshotVideoWidget() const;

signals:
    /** @brief 媒体正常播放到末尾。 */
    void videoFinished();
    /** @brief 管线或解码发生不可恢复错误。 */
    void errorOccured(const QString &message);
    /** @brief 管线达到可播放的暂停预备状态。 */
    void prepared();
    /** @brief 播放位置或总时长更新，单位毫秒。 */
    void positionChanged(int positionMs, int durationMs);
    /** @brief 视频探针观测到有效帧，参数为单调时钟毫秒值。 */
    void videoFrameArrived(qint64 msecs);
    /** @brief MPP输出持续失速或时间轴异常，需要上层原地重建当前直播管线。 */
    void liveTimelineFaultDetected(const QString &reason);
    /** @brief 音频探针观测到有效帧，参数为单调时钟毫秒值。 */
    void audioFrameArrived(qint64 msecs);
    /** @brief 快照转换成功。 */
    void snapshotReady(const QImage &image);
    /** @brief 快照转换失败并给出原因。 */
    void snapshotFailed(const QString &reason);

protected:
    /** @brief 显示时刷新视频输出区域。 */
    void showEvent(QShowEvent *event) override;
    /** @brief 尺寸变化时刷新视频输出区域。 */
    void resizeEvent(QResizeEvent *event) override;
    /** @brief 位置变化时刷新硬件 overlay 坐标。 */
    void moveEvent(QMoveEvent *event) override;

private slots:
    /** @brief 拉取并处理 GStreamer 总线上的状态、EOS 和错误消息。 */
    void pollBus();
    /** @brief 沿用原周期投递一次后台位置/时长查询；忙碌时跳过，UI只发出仍属于当前源的结果。 */
    void pollPosition();
    /** @brief 周期输出 RTSP/RTP、解码、RGA 和 GUI 端到端诊断数据。 */
    void pollPlaybackDiagnostics();

private:
    /** @brief 三槽帧邮箱中单个槽位的所有权状态。 */
    enum FrameSlotState {
        FrameSlotFree = 0,
        FrameSlotWriting,
        FrameSlotReady,
        FrameSlotReading
    };

    /** @brief 解码线程与 GUI 线程之间传递的一帧及其诊断信息。 */
    struct FrameMailboxSlot {
        QByteArray nv12Frame;
        int sourceWidth = 0;
        int sourceHeight = 0;
        int width = 0;
        int height = 0;
        int generation = 0;
        qint64 sampleArrivalMs = 0;
        qint64 publishedMs = 0;
        qint64 rgaElapsedUs = 0;
        quint64 sequence = 0;
        FrameSlotState state = FrameSlotFree;
    };

    enum { kFrameMailboxSlotCount = 3 }; /**< 三槽可吸收短时 GUI 阻塞并允许覆盖旧帧。 */

    /** @brief 延迟创建 playbin、sink、总线和探针。 */
    void ensurePipeline();
    /** @brief 提交不可合并的后台停止/销毁屏障；后继创建须等待完成，普通调用不等待。 */
    void releasePipeline();
    /** @brief 仅最终析构在后台退出后清理尚未移交的引用；普通释放已在后台销毁旧图。 */
    void releasePipelineResources();
    std::unique_ptr<SerialWorkQueue> pipelineQueue_; /**< 每播放器专用串行队列；析构先等待，再释放回调目标。 */
    std::atomic<uint64_t> pipelineFrameRevision_{0}; /**< 后台已配置源对应代次，必须与意图代次相同才放行，避免旧任务重新打开新代次入口。 */
    std::atomic_bool pipelineFramesEnabled_{false}; /**< 停止/切源关闭旧帧入口；新源NULL及URI完成后才放行解码样本。 */
    std::atomic<uint64_t> pipelineRevision_{0}; /**< UI递增的播放意图代次；后台只读，淘汰旧URI/播放请求。 */
    /** @brief 根据平台和配置选择视频 sink。 */
    GstElement *createVideoSink();
    /** @brief 创建 appsink，使解码帧进入 Qt 合成路径。 */
    GstElement *createQtCompositedVideoSink();
    /** @brief 创建把 PCM 转交给进程内共享混音器的音频 sink。 */
    GstElement *createAudioSink();
    /** @brief 设置 sink 的同步、缓存和时延属性。 */
    void configureSink(GstElement *sink, bool video);
    /** @brief 在音视频 pad 上安装到帧时间探针。 */
    void attachFrameProbes();
    /** @brief 移除探针，防止释放管线后继续回调。 */
    void removeFrameProbes();
    /** @brief 将管线标记为已准备并兑现等待中的播放请求。 */
    void markPrepared();
    /** @brief 创建或绑定 Qt 视频画布。 */
    void ensureVideoSurface();
    /** @return 保持源宽高比后位于配置区域内的显示矩形。 */
    QRect fittedVideoRect() const;
    /** @brief 把当前显示矩形同步给硬件 overlay。 */
    void updateVideoOverlay();
    /** @return 当前配置选择 Qt 合成输出时返回 true。 */
    bool useQtCompositedVideo() const;
    /** @return 当前视频输出中的 Qt OpenGL 画布。 */
    VideoHoleWidget *videoCanvas() const;
    /** @brief 清除 Qt 画布帧并归还邮箱槽位。 */
    void clearVideoCanvas();
    /** @brief 将本地路径或 URL 转换为 GStreamer URI。 */
    QString mediaUri(const QString &path) const;
    /** @brief 从缓存的最新 sample 构建快照。 */
    QImage imageFromLastSample(QString *error) const;
    /** @brief 映射一个 GstSample 并转换为 QImage。 */
    QImage imageFromSample(GstSample *sample, QString *error) const;
    /** @brief 为指定代次和尺寸取得可写邮箱槽位。 */
    int acquireFrameMailboxSlot(int width,
                                int height,
                                int generation,
                                int targetBytes,
                                QString *error);
    /** @brief 放弃未发布的写槽并恢复为空闲。 */
    void abandonFrameMailboxSlot(int slotIndex);
    /** @brief 将写完的槽发布为最新就绪帧。 */
    bool publishFrameMailboxSlot(int slotIndex);
    /** @brief 合并多个解码回调，只排队一次 GUI 投递。 */
    void scheduleLatestFrameDelivery();
    /** @brief GUI 线程取得最新帧并提交给 OpenGL 画布。 */
    void deliverLatestFrameToCanvas();
    /** @brief 画布消费完成后按序列号归还读槽。 */
    void finishFrameMailboxRead(int slotIndex, quint64 sequence);
    /** @brief 清空邮箱状态并使遗留帧无效。 */
    void resetFrameMailbox();
    /** @brief 新媒体开始前清空长时运行诊断基线。 */
    void resetPlaybackDiagnostics();
    /** @brief 清除提示音 preroll 引用和本次静音前导状态。 */
    void resetPromptPrerollState();
    /** @brief 在UI计算音量快照，后台串行写入playbin，合并尚未执行的旧音量任务。 */
    void applyEffectiveVolume();
    /** @return UI线程计算的归一化有效音量，包含全部临时压低，供后台状态任务捕获。 */
    float effectiveVolume01() const;

    /** @brief 在同步总线回调中绑定硬件视频窗口句柄。 */
    static GstBusSyncReply busSyncHandler(GstBus *bus, GstMessage *message, gpointer userData);
    /** @brief 记录当前有效源的帧信息；延迟GUI信号按源代次过滤。
     * @param[in] pad GStreamer借用的探针pad；音频探针不使用此参数。
     * @param[in] info GStreamer借用的探针信息，仅buffer参与统计。
     * @param[in] userData 受管线停止/析构屏障保护的播放器指针。
     * @return GST_PAD_PROBE_OK，保持原数据流；不等待GUI。
     */
    static GstPadProbeReturn videoPadProbe(GstPad *pad, GstPadProbeInfo *info, gpointer userData);
    /** @brief 记录当前有效源的帧信息；延迟GUI信号按源代次过滤。
     * @param[in] pad GStreamer借用的探针pad；音频探针不使用此参数。
     * @param[in] info GStreamer借用的探针信息，仅buffer参与统计。
     * @param[in] userData 受管线停止/析构屏障保护的播放器指针。
     * @return GST_PAD_PROBE_OK，保持原数据流；不等待GUI。
     */
    static GstPadProbeReturn audioPadProbe(GstPad *pad, GstPadProbeInfo *info, gpointer userData);
    /** @brief 统计压缩帧及信用失速；故障通知按源代次过滤，避免重建新源。
     * @param[in] pad GStreamer借用pad。
     * @param[in] info GStreamer借用的buffer信息。
     * @param[in] userData 受停止/析构屏障保护的播放器。
     * @return 保持原探针放行/丢弃策略，不等待GUI。
     */
    static GstPadProbeReturn decoderInputPadProbe(GstPad *pad,
                                                  GstPadProbeInfo *info,
                                                  gpointer userData);
    /** @brief 统计视频解码器输出帧及其相对管线时钟的 PTS 偏移。 */
    static GstPadProbeReturn decoderOutputPadProbe(GstPad *pad,
                                                   GstPadProbeInfo *info,
                                                   gpointer userData);
    /** @brief 统计进入 jitterbuffer 前的原始视频 RTP 时间戳、序号和到达节奏。 */
    static GstPadProbeReturn rtpInputPadProbe(GstPad *pad,
                                              GstPadProbeInfo *info,
                                              gpointer userData);
    /** @brief 处理当前源样本；旧代次入口及延迟通知被过滤，不跨线程访问控件。
     * @param[in] sink GStreamer借用的appsink。
     * @param[in] userData 受停止/析构屏障保护的播放器。
     * @return 原GstFlowReturn；无效旧入口返回FLUSHING，提示preroll沿原OK语义。
     */
    static GstFlowReturn appSinkNewSample(GstElement *sink, gpointer userData);
    /** @brief 把音频 appsink 样本送入进程内共享混音器。 */
    static GstFlowReturn sharedAudioAppSinkNewSample(GstElement *sink, gpointer userData);
    /** @brief 用提示音 preroll 的格式向共享混音器预送一段静音。 */
    static GstFlowReturn sharedAudioAppSinkNewPreroll(GstElement *sink, gpointer userData);
    /** @brief 在 source 创建时配置网络源参数。 */
    static void sourceSetup(GstElement *playbin, GstElement *source, gpointer userData);
    /** @brief 在 element 创建时调整解码器和解析器属性。 */
    static void elementSetup(GstElement *playbin, GstElement *element, gpointer userData);
    /** @brief 为 HLS/RTSP H.264 caps 查询补充 AU 对齐要求。 */
    static GstPadProbeReturn h264ParserQueryProbe(GstPad *pad,
                                                  GstPadProbeInfo *info,
                                                  gpointer userData);

private:
    QPointer<QWidget> videoWidget_;
    QPointer<QWidget> videoSurface_;
    QObject *videoEventFilter_ = nullptr;
    QString mediaPath_;

    GstElement *playbin_ = nullptr;
    GstElement *videoSink_ = nullptr;
    GstElement *audioSink_ = nullptr;
    GstElement *appSink_ = nullptr;
    GstElement *overlayElement_ = nullptr;
    GstBus *bus_ = nullptr;
    GstPad *videoProbePad_ = nullptr;
    GstPad *audioProbePad_ = nullptr;
    gulong videoProbeId_ = 0;
    gulong audioProbeId_ = 0;

    mutable QMutex gstMutex_;          /**< 保护跨线程访问的 GStreamer 对象和 sample。 */
    mutable QMutex frameMailboxMutex_; /**< 保护三槽邮箱的所有权和序列状态。 */
    QTimer busTimer_;
    QTimer positionTimer_;
    QTimer playbackDiagnosticTimer_;
    QElapsedTimer playbackDiagnosticElapsed_;

    bool prepared_ = false;
    bool playing_ = false;
    bool playRequested_ = false;
    std::atomic_bool preloadMode_{false}; /**< UI设置、解码回调读取的预加载标志。 */
    bool invalidRtcpDestinationWarningLogged_ = false;
    std::atomic_bool shuttingDown_ {false}; /**< 析构期间阻止回调继续投递。 */
    float volume01_ = 1.0f;
    QHash<const QObject *, float> audioDuckingFactors_; /**< 各播报入口独立持有的临时压低系数。 */
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    QRect configuredDisplayRect_;

    std::atomic<quintptr> windowId_ {0}; /**< 硬件 overlay 使用的原生窗口句柄。 */
    std::atomic<int> overlayWidth_ {0};
    std::atomic<int> overlayHeight_ {0};
    QAtomicInt sourceSizeKnown_ {0};
    QAtomicInt guiDeliveryEventPending_ {0};
    QAtomicInt mediaGeneration_ {1};
    QAtomicInt h264AuAlignmentEnabled_ {0};
    QAtomicInt liveRtspMode_ {0};
    QAtomicInt promptAudioMode_ {0};
    QAtomicInt promptAudioPrimed_ {0};
    QAtomicInt promptLeadInQueued_ {0};
    QAtomicInt liveDecoderAuAligned_ {-1};
    QAtomicInt liveDecoderCapsLogged_ {0};
    QAtomicInt liveWaitingForKeyframe_ {0};
    QAtomicInt liveCreditRecoveryPending_ {0};
    QAtomicInt liveCreditOverLimitConsecutiveAus_ {0};
    GstSample *promptPrerollSample_ = nullptr; /**< 提示音 PAUSED 阶段保留的首个 PCM 样本。 */
    QAtomicInteger<qint64> lastVideoFrameMs_ {0};
    QAtomicInteger<qint64> lastAudioFrameMs_ {0};
    QAtomicInteger<qint64> lastVideoSignalMs_ {0};
    QAtomicInteger<qint64> lastAudioSignalMs_ {0};

    QAtomicInteger<quint64> appSinkSamples_ {0};
    QAtomicInteger<quint64> appSinkRgaFrames_ {0};
    QAtomicInteger<quint64> appSinkRgaSkippedFrames_ {0};
    QAtomicInteger<quint64> mailboxPublishedFrames_ {0};
    QAtomicInteger<quint64> mailboxReadyOverwrites_ {0};
    QAtomicInteger<quint64> mailboxNoSlotDrops_ {0};
    QAtomicInteger<quint64> guiDeliveryCoalesced_ {0};
    QAtomicInteger<quint64> guiDeliveredFrames_ {0};
    QAtomicInteger<qint64> appSinkLastSampleMs_ {0};
    QAtomicInteger<qint64> appSinkMaxGapMs_ {0};
    QAtomicInteger<qint64> appSinkMaxRgaUs_ {0};
    QAtomicInteger<qint64> guiMaxQueueLagMs_ {0};
    QAtomicInteger<qint64> guiMaxEndToEndLagMs_ {0};
    QAtomicInt firstRgaSuccessLogged_ {0};
    QAtomicInteger<quint64> qosMessageCount_ {0};
    QAtomicInteger<quint64> qosLastProcessed_ {0};
    QAtomicInteger<quint64> qosLastDropped_ {0};
    QAtomicInteger<qint64> qosMaxLateNs_ {0};
    QString qosLastSource_;             /**< 最近一条 QoS 消息的来源元素。 */
    QAtomicInteger<quint64> jitterVideoTooLateDrops_ {0};
    QAtomicInteger<quint64> jitterVideoLatencyDrops_ {0};
    QAtomicInteger<quint64> jitterAudioTooLateDrops_ {0};
    QAtomicInteger<quint64> jitterAudioLatencyDrops_ {0};
    QAtomicInteger<quint64> jitterUnknownTooLateDrops_ {0};
    QAtomicInteger<quint64> jitterUnknownLatencyDrops_ {0};
    QAtomicInteger<quint64> decoderInputFrames_ {0};
    QAtomicInteger<quint64> decoderInputKeyFrames_ {0};
    QAtomicInteger<quint64> decoderInputCorruptedFrames_ {0};
    QAtomicInteger<quint64> decoderOutputFrames_ {0};
    QAtomicInteger<quint64> decoderOutputCorruptedFrames_ {0};
    QAtomicInteger<qint64> decoderInputLastBufferMs_ {0};
    QAtomicInteger<qint64> decoderInputMaxGapMs_ {0};
    QAtomicInteger<qint64> decoderInputPtsOffsetNs_ {0};
    QAtomicInteger<qint64> decoderInputMaxAbsPtsOffsetNs_ {0};
    QAtomicInt decoderInputPtsOffsetValid_ {0};
    QAtomicInteger<qint64> decoderOutputLastBufferMs_ {0};
    QAtomicInteger<qint64> decoderOutputMaxGapMs_ {0};
    QAtomicInteger<qint64> decoderOutputPtsOffsetNs_ {0};
    QAtomicInteger<qint64> decoderOutputMaxAbsPtsOffsetNs_ {0};
    QAtomicInt decoderOutputPtsOffsetValid_ {0};
    QAtomicInteger<quint64> liveAuReceived_ {0};
    QAtomicInteger<quint64> liveAuAccepted_ {0};
    QAtomicInteger<quint64> liveAuDropped_ {0};
    QAtomicInteger<qint64> liveInFlightAus_ {0};
    QAtomicInteger<qint64> liveMaximumInFlightAus_ {0};
    QAtomicInteger<qint64> liveLatestInputPtsNs_ {0};
    QAtomicInteger<qint64> liveLatestOutputPtsNs_ {0};
    QAtomicInteger<qint64> liveDecoderLagNs_ {0};
    QAtomicInteger<qint64> liveMaximumDecoderLagNs_ {0};
    QAtomicInt liveLatestInputPtsValid_ {0};
    QAtomicInt liveLatestOutputPtsValid_ {0};
    QAtomicInteger<qint64> appSinkPtsOffsetNs_ {0};
    QAtomicInteger<qint64> appSinkMaxAbsPtsOffsetNs_ {0};
    QAtomicInt appSinkPtsOffsetValid_ {0};

    QAtomicInteger<quint64> rtpVideoRawPackets_ {0};
    QAtomicInteger<quint64> rtpVideoTimestampChanges_ {0};
    QAtomicInteger<quint64> rtpVideoTimestampAdvanceTicks_ {0};
    QAtomicInteger<quint64> rtpVideoArrivalAdvanceUs_ {0};
    QAtomicInteger<quint64> rtpVideoSequenceGaps_ {0};
    QAtomicInteger<quint64> rtpVideoSequenceReorders_ {0};
    QAtomicInteger<quint64> rtpVideoTimestampBackwards_ {0};
    QAtomicInteger<quint64> rtpVideoTimestampJumps_ {0};
    QAtomicInteger<quint64> rtpVideoSsrcChanges_ {0};
    QAtomicInteger<quint32> rtpVideoLastTimestamp_ {0};
    QAtomicInteger<quint32> rtpVideoLastSequence_ {0};
    QAtomicInteger<quint32> rtpVideoLastSsrc_ {0};
    QAtomicInteger<quint32> rtpVideoLastPayloadType_ {0};
    QAtomicInteger<qint64> rtpVideoLastTimestampArrivalUs_ {0};
    QAtomicInteger<qint64> rtpVideoClockRate_ {0};
    QAtomicInt rtpVideoPreviousPacketValid_ {0};
    int timelineFaultConsecutiveIntervals_ = 0;

    quint64 diagnosticLastAppSinkSamples_ = 0;
    quint64 diagnosticLastRgaFrames_ = 0;
    quint64 diagnosticLastRgaSkippedFrames_ = 0;
    quint64 diagnosticLastMailboxOverwrites_ = 0;
    quint64 diagnosticLastMailboxNoSlotDrops_ = 0;
    quint64 diagnosticLastGuiDeliveredFrames_ = 0;
    quint64 diagnosticLastRtpPushed_ = 0;
    quint64 diagnosticLastRtpLost_ = 0;
    quint64 diagnosticLastRtpLate_ = 0;
    quint64 diagnosticLastRtpDuplicates_ = 0;
    quint64 diagnosticLastSinkRendered_ = 0;
    quint64 diagnosticLastSinkDropped_ = 0;

    std::array<FrameMailboxSlot, kFrameMailboxSlotCount> frameMailboxSlots_; /**< 帧邮箱固定槽位。 */
    int frameMailboxWidth_ = 0;
    int frameMailboxHeight_ = 0;
    int frameMailboxGeneration_ = 0;
    int latestReadyFrameSlot_ = -1;
    quint64 frameMailboxSequence_ = 0;
};

#endif // GST_PLAYER_WIDGET_H
