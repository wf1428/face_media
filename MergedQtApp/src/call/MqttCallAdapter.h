/**
 * @file MqttCallAdapter.h
 * @brief 保留UI通话接口的轻量代理，独立音频调度线程直接与IPC线程交换媒体。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-03；修改人：Du lin；录放接口/状态机不变，将原发送及媒体信用逻辑迁入CallAudioTransport，代次过滤迟到通知，普通调用不等待。
 */
#pragma once
#include "CallAudioTransport.h"
#include <QThread>
class MqttIpcClient;
/** @brief UI所有的通话代理；后台媒体完全绕过UI，业务状态通知仍回到UI。 */
class MqttCallAdapter:public QObject {
    Q_OBJECT
public:
    /** @brief 创建独立音频事件循环，IPC必须比本对象更长寿。
     * @param[in] ipc 借用线程安全IPC代理。
     * @param[in] engine 借用线程安全音频队列，必须在本代理销毁后才释放。
     * @param[in] parent UI所有者。
     */
    MqttCallAdapter(MqttIpcClient *ipc,AudioEngine *engine,QObject *parent=nullptr);
    /** @brief 仅应用退出/销毁时等待音频调度线程退出；正常挂断异步。 */
    ~MqttCallAdapter() override;
    /** @brief 设置原主题身份，异步同步后台。
     * @param[in] deviceId 原精确设备标识。
     */
    void configure(const QString &deviceId);
    /** @brief 构造小型通话事件并非阻塞交IPC线程编码发送。
     * @param[in] method 原call.*事件方法。
     * @param[in] data 原data对象。
     * @return true已接受；false断线、无路由或有界拒绝，不表示平台接收。
     */
    bool event(const QString &method,const QJsonObject &data);
    /** @brief 异步切换媒体并递增控制代次，旧ready/stalled不影响新通话。
     * @param[in] on true立即开启音频优先并排队启用媒体；false排队停止媒体，后台提交停止帧后解除优先。
     */
    void media(bool on);
    /** @brief 兼容手工投递媒体信封；正常IPC已直达后台，无需UI转发。
     * @param[in] object 原小型媒体JSON。
     * @return true属于媒体且已排队；false交其他业务处理。
     */
    bool handleIpc(const QJsonObject &object);
    /** @return 当前精确request主题。 */
    QString requestTopic() const;
    /** @return UI线程维护的当前设备标识，不跨线程读取后台QString。 */
    QString deviceId() const { return deviceId_; }
signals:
    /** @brief 后台原诊断，UI只显示小文本。
     * @param[in] text 原统计。
     */
    void diagnostics(const QString &text);
    /** @brief 当前代次后台已收到ready，控制器可执行原接通流程。 */
    void mediaReady();
    /** @brief 当前代次后台不可继续，控制器沿用原异步结束。 */
    void stalled();
private:
    MqttIpcClient *ipc_; /**< 借用线程安全代理，生命周期覆盖后台。 */
    QThread *audioThread_;CallAudioTransport *transport_; /**< 独立调度事件循环及其对象，退出先停后台再释放引擎。 */
    QString deviceId_;quint64 revision_=0; /**< UI身份及递增媒体代次，避免跨线程直接读取成员。 */
};
