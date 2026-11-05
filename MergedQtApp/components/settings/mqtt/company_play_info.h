/**
 * @file company_play_info.h
 * @brief 管理高清播放信息截图的有界请求队列、共享后台编码和上传背压。
 * @author Du lin
 * @date 2026-10-24
 * @note 最近修改：2026-11-04；修改人：Du lin；高清输入改为GUI直接绘制的原DPR像素和NV12值；取消命令回放约定，原尺寸/JPEG上限、共享后台、有界上传及四请求门控保持。
 */
#pragma once

#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QQueue>
#include <QRect>
#include <QTimer>
#include "common/snapshot_scene.h"
#include <functional>
#include <atomic>
#include <memory>



/**
 * @brief 串行采集高清截图，保留原始尺寸并限制 JPEG 不超过 1,000,000 字节。
 * @details 本对象在 GUI/MQTT 线程使用，只保留最多四笔请求（含活动请求）。
 *          GUI像素/视频值由captureRequested交给界面；视频转换、合成、JPEG 重试、Base64
 *          和完整 IPC JSON 序列化在共享截图后台线程执行，不使用普通截图缓存。
 *          抓取失败、超时或满队列时仍回显原信封，图片为空，原因写入本次 deviceLog。
 */
class CompanyPlayInfo final : public QObject {
    Q_OBJECT
public:
    /**
     * @brief 初始化三秒抓取超时和有界串行调度。
     * @param[in] parent 按 QObject 规则拥有本对象，可为空。
     */
    explicit CompanyPlayInfo(QObject *parent = nullptr);
    /** @brief 取消待执行/重试与UI回调；在途只读编码自行收尾，不在UI等待。 */
    ~CompanyPlayInfo() override;

    /**
     * @brief 排队处理一笔已构造的 sendPlayInfoFromCompany IPC publish 信封。
     * @param[in] packet 不含高清图片的状态快照；每笔请求独立保存 id、主题和播放字段。
     * @note 仅在对象线程调用；满队列直接输出小型失败信封，不等待工作线程。
     */
    void enqueue(const QJsonObject &packet);

    /** @brief 设置GUI非阻塞上传入口；false表示容量忙，保留唯一已编码结果，50ms重试。
     * @param[in] publisher 值提交回调；true已处理该结果，空回调沿用publishReady观察接口。
     * @note 仅GUI调用；不缓存新图片队列，上传忙时不启动下一请求抓取。
     */
    void setPublisher(std::function<bool(const QByteArray &)> publisher) { publisher_=std::move(publisher); }
public slots:
    /** @brief GUI交接已绘制的原尺寸UI像素及NV12值，后台仅转换视频/合成/编码，不访问控件。
     * @param[in] token 当前本地抓取令牌，旧/重复忽略。
     * @param[in] scene GUI直接绘制的像素/视频值；抓取失败时生成原协议失败回执。
     */
    void submitScene(const QString &token,const SnapshotScene &scene);
    /**
     * @brief 接收界面抓取的图像，交给共享截图队列；过期token或重复结果忽略。
     * @param[in] token captureRequested 发出的本地唯一令牌，不是协议 id。
     * @param[in] uiImage 完整 UI 原始像素图；空值表示抓取失败。
     * @param[in] videoImage 可为空的视频帧；非空时在 UI 下层按 videoRect 合成。
     * @param[in] videoRect 视频目标矩形，单位为 uiImage 的实际像素。
     * @param[in] reason 非空表示抓取失败，本次不编码图片。
     */
    void submitFrame(const QString &token, const QImage &uiImage, const QImage &videoImage,
                     const QRect &videoRect, const QString &reason = QString());

signals:
    /** @brief 异步请求 GUI 抓取当前画面；token 必须随结果原样回传。 */
    void captureRequested(const QString &token);
    /** @brief 原协议换行IPC帧；未设置publisher时由接收方非阻塞提交，设置后仅观察已处理结果，避免重复上传。
     * @param[in] line 后台编码结果，或满队列时的小型空图失败回执。 */
    void publishReady(const QByteArray &line);
    /** @brief 满请求队列的原协议小型失败回执，不含图片，不占图片预算。
     * @param[in] line 保留原ID/主题的空图失败信封；生产接线使用普通非阻塞IPC提交。
     */
    void overflowReplyReady(const QByteArray &line);

private:
    /** @brief 单笔请求的独立状态；不借用 MQTT 服务或 UI 对象。 */
    struct Request {
        QString token;       /**< 本地唯一令牌，用于拒绝超时后的迟到结果。 */
        QJsonObject packet;  /**< 含原请求 id 的 publish 信封；仅图像和失败说明会被补入。 */
    };
    /** @brief 无活动请求时开始队首抓取，并启动三秒超时。 */
    void startNext();

    QQueue<Request> pending_; /**< 等待队列；与 active_ 合计最多四笔，由对象线程读写。 */
    Request active_;         /**< 当前抓取或编码中的请求；空 token 表示空闲。 */
    QTimer captureTimer_;    /**< 仅限制 GUI 抓取等待，编码期间停止，单位毫秒。 */
    /** @brief 共用后台提交入口，只按值捕获，不等待GUI。
     * @param[in] token 当前本地令牌，旧/重复忽略。
     * @param[in] scene 可空的自有UI像素/视频值。
     * @param[in] useScene true合成UI像素及NV12，false使用兼容图像值。
     * @param[in] uiImage 兼容UI图像，useScene为true时忽略。
     * @param[in] videoImage 兼容视频后备图。
     * @param[in] videoRect 视频实际像素区域。
     * @param[in] reason 非空为已有抓取失败原因。
     */
    void queueFrame(const QString &token,const SnapshotScene &scene,bool useScene,const QImage &uiImage,
                    const QImage &videoImage,const QRect &videoRect,const QString &reason);
    /** @brief GUI尝试有界上传；忙时保留单个结果并延迟，不开始下一次抓取。 */
    void tryPublish();
    bool processing_=false;QByteArray readyLine_; /**< GUI独占在途门/唯一待上传结果；不增加完整图片积压队列。 */
    QTimer uploadTimer_;std::function<bool(const QByteArray &)> publisher_; /**< GUI的50ms重试及非阻塞提交回调。 */
    std::shared_ptr<std::atomic_bool> cancelled_=std::make_shared<std::atomic_bool>(false); /**< 析构取消后台步骤间重试，无GUI join。 */
};
