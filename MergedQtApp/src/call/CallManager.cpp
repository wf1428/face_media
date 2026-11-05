/**
 * @file CallManager.cpp
 * @brief 实现双向通话的幂等信令、单会话约束、掉线兜底及异步音频收尾。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-03；修改人：Du lin；保留原状态机及UI诊断；退出时先停止音频调度代理再释放借用的引擎，普通呼叫/挂断仍异步。
 */
#include "CallManager.h"
#include "AudioOutputFocus.h"
#include "common/debug/ui_performance.h"
#include <QJsonValue>
#include <QDebug>

CallManager::CallManager(MqttIpcClient *ipc,QObject *parent):QObject(parent)
{
    VoiceCall::initializeOutputVolume();
    engine_=new AudioEngine(this); adapter_=new MqttCallAdapter(ipc,engine_,this);
    timeout_.setSingleShot(true);
    connect(&timeout_,&QTimer::timeout,this,[this]() { finish(QStringLiteral("ring_timeout"),true); });
    connect(adapter_,&MqttCallAdapter::diagnostics,this,[this](const QString &text) {
        emit logMessage(QStringLiteral("[call] %1 %2").arg(callId_,text));
    });
    connect(adapter_,&MqttCallAdapter::stalled,this,[this]() { finish(QStringLiteral("disconnect"),true); });
    connect(engine_,&AudioEngine::ready,this,[this]() {
        if (state_!=Starting || !available()) { engine_->stopAsync(); return; }
        adapter_->media(true);
    });
    connect(adapter_,&MqttCallAdapter::mediaReady,this,[this]() {
        if (state_!=Starting || !available()) return;
        if (!outgoing_ && !report(QStringLiteral("call.answer"))) { finish("disconnect",false); return; }
        timeout_.stop(); change(Talking); engine_->setTalking(true);
    });
    connect(engine_,&AudioEngine::failed,this,[this](const QString &error) {
        emit logMessage(QStringLiteral("[call] %1 error=%2").arg(callId_,error));
        finish(QStringLiteral("audio_error"),true);
    });
    connect(engine_,&AudioEngine::statistics,this,[this](const QString &text) {
        emit logMessage(QStringLiteral("[call] %1 %2").arg(callId_,text));
    });
    connect(engine_,&QThread::finished,this,[this]() {
        // ready/failed/finished 均排队到同一 UI 线程，不在音频线程析构 QThread。
        enginePending_=false;
        if (state_!=Ending && state_!=Idle) finish(QStringLiteral("audio_error"),true);
        VoiceCall::releaseOutputVolume(this);
        if (state_==Ending) { retire(); callId_.clear(); change(Idle); }
    });
    connect(this,&CallManager::logMessage,this,[](const QString &text) { qInfo().noquote()<<text; });
}
CallManager::~CallManager() { adapter_->media(false); delete adapter_; adapter_=nullptr; engine_->stopAsync(); delete engine_; VoiceCall::releaseOutputVolume(this); }
void CallManager::configure(const QString &deviceId) {
    if (adapter_->deviceId()!=deviceId) { finish("route_changed",true); adapter_->configure(deviceId); }
    change(state_);
}
void CallManager::setOnline(bool online) {
    if (online_==online) return;
    online_=online;
    if (!online_) finish(QStringLiteral("disconnect"),false);
    change(state_);
}
bool CallManager::handleIpc(const QJsonObject &obj) { return adapter_->handleIpc(obj); }
/** @brief 保持原状态通知顺序，并仅向诊断模块传递通话标签。
 * @param[in] state 原目标状态；Idle请求诊断异步最终汇总，其他状态不延迟业务处理。
 */
void CallManager::change(State state) {
    state_=state;
    UiPerformance::setCallContext(state_==Idle ? QString() : callId_.isEmpty() ? QStringLiteral("pending") : callId_);
    const QStringList names={"空闲","正在呼叫物业","等待物业接听（排队中）","等待接听","正在连接语音","通话中","正在结束通话"};
    emit logMessage(QStringLiteral("[call] callId=%1 state=%2").arg(callId_).arg(int(state_)));
    emit changed(state_==Idle && cancelledRequest_ ? QStringLiteral("等待上一呼叫结束") :
        state_==Idle && !available() ? QStringLiteral("语音通话未连接") : names.at(int(state_)));
}
bool CallManager::report(const QString &method,const QString &reason) {
    if (callId_.isEmpty()) return false;
    QJsonObject data{{"callId",callId_}};
    if (!reason.isEmpty()) data.insert("reason",reason);
    return adapter_->event(method,data);
}
void CallManager::retire() {
    if (!callId_.isEmpty() && !retired_.contains(callId_)) retired_.append(callId_);
    while (retired_.size()>32) retired_.removeFirst();
}
void CallManager::requestCall() {
    if (!canRequestCall()) return;
    outgoing_=true;
    if (!adapter_->event(QStringLiteral("call.request"),{})) return;
    change(Requesting); timeout_.start(30000);
}
void CallManager::answer() { if (state_==Ringing && !outgoing_) startAudio(); }
void CallManager::pressCallButton() {
    if (state_==Idle) requestCall();
    else if (state_!=Ending) hangup();
}
void CallManager::hangup() { finish(QStringLiteral("hangup"),true); }
void CallManager::startAudio() {
    if (state_!=Ringing && state_!=Queued) return;
    change(Starting);
    VoiceCall::acquireOutputVolume(this,[this]() {
        if (state_!=Starting || enginePending_) return;
        enginePending_=true;engine_->begin();
    });
}
void CallManager::finish(const QString &reason,bool notify) {
    if (state_==Idle || state_==Ending) return;
    engine_->setTalking(false); adapter_->media(false); timeout_.stop();
    if (state_==Requesting && callId_.isEmpty() && reason!=QStringLiteral("busy")) {
        cancelledRequest_=true;
        const quint64 generation=++cancellationGeneration_;
        // 协议无 request 取消关联字段；暂禁新主叫，迟到 ACK 可补发挂断。
        QTimer::singleShot(30000,this,[this,generation]() {
            if (generation!=cancellationGeneration_) return;
            cancelledRequest_=false; change(state_);
        });
    }
    if (notify && online_) report(QStringLiteral("call.hangup"),reason);
    change(Ending); engine_->stopAsync();
    if (!enginePending_) { VoiceCall::releaseOutputVolume(this); retire(); callId_.clear(); change(Idle); }
}
bool CallManager::handleSignal(const QString &topic,const QJsonObject &payload)
{
    const QString method=payload.value("method").toString();
    if (!method.startsWith("call.")) return false;
    if (!available() || topic!=adapter_->requestTopic() ||
        payload.value("deviceId").toString()!=adapter_->deviceId() ||
        payload.value("id").toString().isEmpty() || !payload.value("data").isObject()) return true;
    const QJsonObject data=payload.value("data").toObject();
    const QString id=data.value("callId").toString().trimmed();
    if (id.size()>128 || retired_.contains(id)) return true;
    if (method=="call.start") {
        if (id.isEmpty()) return true;
        const QString requestId=payload.value("id").toString();
        if (seenStarts_.contains(requestId)) return true;
        seenStarts_.append(requestId); if (seenStarts_.size()>32) seenStarts_.removeFirst();
        if (state_!=Idle || cancelledRequest_) {
            if (id!=callId_) adapter_->event("call.hangup",{{"callId",id},{"reason","busy"}});
            return true;
        }
        callId_=id; outgoing_=false; change(Ringing);
        report(QStringLiteral("call.ringing"));
        const int timeout=qBound(1000,data.value("timeoutMs").toInt(30000),30000);
        timeout_.start(timeout);
        // 平台来电默认接听；沿用可取消异步准备，媒体就绪后才报告 answer。
        startAudio();
    } else if (method=="call.requestAck") {
        const QString ack=data.value("state").toString();
        if (cancelledRequest_ && state_==Idle) {
            if (!id.isEmpty()) { adapter_->event("call.hangup",{{"callId",id},{"reason","hangup"}});
                retired_.append(id); while (retired_.size()>32) retired_.removeFirst(); }
            cancelledRequest_=false; ++cancellationGeneration_; change(state_); return true;
        }
        if (!outgoing_ || (state_!=Requesting && state_!=Queued && state_!=Ringing)) return true;
        if (ack=="busy" && state_==Requesting) { finish("busy",false); return true; }
        if (id.isEmpty() || (!callId_.isEmpty() && id!=callId_) || (ack!="queued" && ack!="ringing")) return true;
        if (state_==Ringing && ack=="queued") return true;
        callId_=id; change(ack=="queued" ? Queued : Ringing);
        // 保持从 request 起的原始超时，重复 ACK 不延长占用。
    } else if (method=="call.answer") {
        if (!id.isEmpty() && id==callId_ && outgoing_ && (state_==Ringing || state_==Queued)) startAudio();
    } else if (method=="call.stop") {
        if (!id.isEmpty() && id==callId_) finish(data.value("reason").toString("hangup"),false);
    }
    return true;
}
