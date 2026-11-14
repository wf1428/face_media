/**
 * @file CallManager.cpp
 * @brief 实现双向通话的幂等信令、单会话约束、掉线兜底及异步音频收尾。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-05；修改人：Du lin；移除物理键挂断分支，外呼等待时驱动铃声并提前申请音量焦点，接听或结束时停止铃声。
 */
#include "CallManager.h"
#include "AudioOutputFocus.h"
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
        if (outgoing_) emit outgoingRingtoneChanged(RingtonePhase::Off);
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
        releaseOutput();
        if (state_==Ending) { retire(); callId_.clear(); change(Idle); }
    });
    connect(this,&CallManager::logMessage,this,[](const QString &text) { qInfo().noquote()<<text; });
}
CallManager::~CallManager()
{
    emit outgoingRingtoneChanged(RingtonePhase::Off);
    adapter_->media(false);
    delete adapter_;
    adapter_=nullptr;
    engine_->stopAsync();
    delete engine_;
    releaseOutput();
}
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
void CallManager::change(State state) {
    state_=state;
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
    timeout_.start(30000);
    change(Requesting);
    if (state_!=Requesting) return;
    emit outgoingRingtoneChanged(RingtonePhase::Preparing);
    prepareOutput();
}
void CallManager::answer() { if (state_==Ringing && !outgoing_) startAudio(); }
void CallManager::pressCallButton()
{
    requestCall();
}
void CallManager::hangup() { finish(QStringLiteral("hangup"),true); }
void CallManager::startAudio() {
    if (state_!=Ringing && state_!=Queued) return;
    if (outgoing_) emit outgoingRingtoneChanged(RingtonePhase::HandingOff);
    change(Starting);
    prepareOutput();
    beginAudioIfReady();
}
void CallManager::prepareOutput()
{
    if (outputRequested_) return;
    outputRequested_=true;
    VoiceCall::acquireOutputVolume(this, [this]() {
        outputReady_=true;
        if (state_==Starting) {
            beginAudioIfReady();
        } else if (outgoing_ && (state_==Requesting || state_==Queued || state_==Ringing)) {
            emit outgoingRingtoneChanged(RingtonePhase::Playing);
        }
    });
}
void CallManager::beginAudioIfReady()
{
    if (state_!=Starting || !outputReady_ || enginePending_) return;
    enginePending_=true;
    engine_->begin();
}
void CallManager::releaseOutput()
{
    if (!outputRequested_) return;
    outputRequested_=false;
    outputReady_=false;
    VoiceCall::releaseOutputVolume(this);
}
void CallManager::finish(const QString &reason,bool notify) {
    if (state_==Idle || state_==Ending) return;
    if (outgoing_) emit outgoingRingtoneChanged(RingtonePhase::Off);
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
    if (!enginePending_) { releaseOutput(); retire(); callId_.clear(); change(Idle); }
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
