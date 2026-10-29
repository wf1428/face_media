/**
 * @file company_play_info.h
 * @brief 管理高清播放信息截图的有界请求队列和独立后台编码任务。
 * @author Du lin
 * @date 2026-10-24
 * @note 最近修改：2026-10-24；修改人：Du lin；移除临时高清诊断日志信号，保留有界队列、后台编码和失败回执接口。
 */
#pragma once

#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QQueue>
#include <QRect>
#include <QTimer>

class CompanySnapshotTask;

/**
 * @brief 串行采集高清截图，保留原始尺寸并限制 JPEG 不超过 1,000,000 字节。
 * @details 本对象在 GUI/MQTT 线程使用，只保留最多四笔请求（含活动请求）。
 *          QWidget 抓取由 captureRequested 交给界面；合成、JPEG 重试、Base64
 *          和完整 IPC JSON 序列化在独立低优先级线程执行，不使用普通截图缓存。
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
    /** @brief 停止受理并取消编码重试，等待当前一次 JPEG 编码返回后释放线程。 */
    ~CompanyPlayInfo() override;

    /**
     * @brief 排队处理一笔已构造的 sendPlayInfoFromCompany IPC publish 信封。
     * @param[in] packet 不含高清图片的状态快照；每笔请求独立保存 id、主题和播放字段。
     * @note 仅在对象线程调用；满队列直接输出小型失败信封，不等待工作线程。
     */
    void enqueue(const QJsonObject &packet);

public slots:
    /**
     * @brief 接收界面抓取的图像，交给独立线程处理；过期 token 或重复结果被忽略。
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
    /** @brief 已在后台完成序列化的换行 IPC 帧，接收方只能非阻塞提交到 mqttd。 */
    void publishReady(const QByteArray &line);

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
    CompanySnapshotTask *task_ = nullptr; /**< 本对象拥有的活动编码线程，完成或析构时回收。 */
};
