/**
 * @file live_stream_probe.h
 * @brief 在网络事件线程内管理播放器重试和网络告警的直播源探测。
 * @author Du lin
 * @date 2026-11-13
 * @note 最近修改：2026-11-14；修改人：Du lin；记录播放器和告警各自的连续失败地址，仅用于日志去重。
 */
#pragma once

#include <QObject>
#include <QString>
#include <functional>
#include <optional>

class QNetworkAccessManager;

/**
 * @brief 两个调用方共享一个 HTTP 管理器，各自保留一条可取消的探测。
 * @details 在所属网络事件线程内构建、执行和销毁请求。结果回调在同一线程执行，
 * 调用方负责向 UI 排队投递值和过滤已取消代次。取消只清理指定调用方的探测。
 */
class LiveStreamProbe final : public QObject
{
public:
    /** @brief 选择调用方原有协议规则和超时。 */
    enum class Purpose {
        PlaybackRetry, /**< 判定能否尝试切回前台直播。 */
        NetworkWarning /**< 判定是否需要提示直播源异常。 */
    };

    /** @brief 单轮探测的结果，供跨线程传值。 */
    struct Result {
        bool available = false; /**< true 表示通过对应调用方的原有检查。 */
        bool linkDown = false; /**< true 表示播放重试时检测到有线链路断开。 */
        QString detail; /**< 成功、失败或跳过的原因。 */
    };

    using Completion = std::function<void(Result)>; /**< 网络线程内至多调用一次；取消后不调用。 */

    /** @brief 创建随父对象在网络线程中使用的探测服务。
     * @param[in] parent 所有者；网络请求延迟到 start 创建。
     */
    explicit LiveStreamProbe(QObject *parent = nullptr);

    /** @brief 根据 URL 协议和扩展名推断直播类型。
     * @param[in] url 可带空白的直播源地址。
     * @return 已知类型，无法推断时为 live。
     */
    static QString kindFromUrl(const QString &url);

    /** @brief 替换指定调用方的探测并启动原协议检查。
     * @param[in] purpose 调用方及对应的探测策略。
     * @param[in] url 待探测的直播源地址。
     * @param[in] kind 业务协议类型，可空；空值按原规则从 URL 推断。
     * @param[in] completion 探测结果的回调，在网络线程调用。
     */
    void start(Purpose purpose, const QString &url, const QString &kind, Completion completion);

    /** @brief 取消指定调用方的请求与超时；保留共享 HTTP 管理器。
     * @param[in] purpose 要取消的调用方。
     */
    void cancel(Purpose purpose);

private:
    class Request;
    /** @brief 定位指定调用方当前在途请求的槽位，槽位可空。
     * @param[in] purpose 请求所属调用方。
     * @return 对网络线程独占的非拥有指针槽位的引用。
     */
    Request *&active(Purpose purpose);

    QNetworkAccessManager *httpManager_ = nullptr; /**< QObject 子对象；首次 HTTP 请求创建，供两路复用。 */
    Request *playback_ = nullptr; /**< 当前播放重试请求，由 QObject 父子关系拥有。 */
    Request *warning_ = nullptr; /**< 当前告警请求，由 QObject 父子关系拥有。 */
    std::optional<QString> playbackLoggedFailure_; /**< 网络线程持有；已记录失败的播放地址，恢复或换源后清空。 */
    std::optional<QString> warningLoggedFailure_; /**< 网络线程持有；已记录失败的告警地址，恢复或换源后清空。 */
};
