/**
 * @file CallAudioTransport.cpp
 * @brief 实现单 mqttd 的音频十六进制 IPC 封装和发送信用；MQTT 线上仍为 650B 二进制。
 * @author Du lin
 * @date 2026-11-03
 * @note 最近修改：2026-11-04；修改人：Du lin；预先缓存媒体JSON转义前缀，逐帧固定数组编码650B音频并通过无锁IPC槽提交；整数下行确认沿用固定行，异常/超长身份回退原封装；信用、协议和既有统计不变。
 * @details 原窗口、时序、编码及媒体校验移至独立音频线程；控制通知携带代次，收发不经过UI。
 */
#include "CallAudioTransport.h"
#include "PcmDiagnostics.h"
#include "components/settings/mqtt/mqttipcclient.h"
#include <QDateTime>
#include <QUuid>
#include <QThread>
#include <QJsonArray>
#include <QJsonDocument>
#include <charconv>
#include <cstring>
#include <cmath>

CallAudioTransport::CallAudioTransport(MqttIpcClient *ipc,AudioEngine *engine,QObject *parent)
    :QObject(parent),ipc_(ipc),engine_(engine)
{
    timer_.setParent(this); // 代理构造后moveToThread，成员定时器必须随父对象迁移。
    timer_.setObjectName(QStringLiteral("call.audio.send"));
    timer_.setTimerType(Qt::PreciseTimer); timer_.setInterval(5);
    connect(&timer_,&QTimer::timeout,this,&CallAudioTransport::tick);
}
void CallAudioTransport::configure(const QString &deviceId) {
    if (deviceId_==deviceId) return;
    media(false,revision_); deviceId_=deviceId;
}
QString CallAudioTransport::requestTopic() const {
    return deviceId_.isEmpty() ? QString() : QStringLiteral("device/ycLinux/%1/request").arg(deviceId_);
}
void CallAudioTransport::media(bool on,quint64 revision,quint64 reservation) {
    if(revision<revision_) return;
    revision_=revision;
    if(on) reservation_=reservation;
    const auto now=AudioEngine::nowMs();
    if (!on && active_) reportTransport(QStringLiteral("final"),now);
    active_=on; credits_=0; window_=1; nextSendAt_=0;
    if (on) {
        rxIpc_=rxForwarded_=rxRejected_=txSubmitted_=0; firstRejectReported_=false;
        lastCaptureIndex_=txCaptureSkips_=txModeChanges_=txClip_=0;
        lastPcmTail_=txStepMax_=txRms_=txPcmPeak_=txDc_=0;lastProcessingMode_=0;
        takeMaxUs_=submitMaxUs_=takeMaxBeginUs_=submitMaxBeginUs_=0;takeAtMax_={};submitAtMax_={};pcmHash_=wireHash_=0;wireHashMismatches_=0;
        txSubmitAt_=txSubmitGapMaxMs_=txAgeMaxMs_=0;txPcmLastLogMs_=0;txPcmReports_=0;
        readyWait_={};creditWait_={};ipcWait_={};readyWait_.observe(true,now);
        diagnosticReady_=false;diagnosticCreditMin_=-1;lastTickAt_=now;tickGapMaxMs_=0;
        tickGapStartMs_=tickGapEndMs_=0;
        ipcPendingPeakBytes_=0;ipcDisconnectedTicks_=txSendFailures_=0;
    }
    lastReportAt_=AudioEngine::nowMs();
    epoch_=QUuid::createUuid().toString(QUuid::WithoutBraces);
    pendingSince_=AudioEngine::nowMs();
    // 身份转义仅在媒体切换时执行；逐帧只复制固定前缀并写数字/hex。
    const auto quoted=[](const QString &value) { const auto array=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);return array.mid(1,array.size()-2); };
    audioPrefix_=QByteArray("{\"cmd\":\"publish_audio\",\"epoch\":")+quoted(epoch_)+",\"topic\":"+
        quoted(QStringLiteral("device/ycLinux/%1/audio/up").arg(deviceId_))+",\"hex\":\"";
    ackPrefix_=QByteArray("{\"cmd\":\"audio_received\",\"epoch\":")+quoted(epoch_)+",\"token\":";
    if (ipc_->isConnected()) ipc_->sendJsonLine({{"cmd","call_media"},{"enabled",on},{"epoch",epoch_},
        {"topic",deviceId_.isEmpty() ? QString() : QStringLiteral("device/ycLinux/%1/audio").arg(deviceId_)}});
    if(!on) { ipc_->releaseRealtime(reservation_);reservation_=0; }
}
bool CallAudioTransport::handleIpc(const QJsonObject &obj) {
    const QString type=obj.value("type").toString();
    if (type=="call_ready" || type=="call_sent") {
        if (active_ && obj.value("epoch").toString()==epoch_) {
            pendingSince_=AudioEngine::nowMs();
            if (!obj.value("ok").toBool()) { emit diagnostics(QStringLiteral("transport rejected type=%1").arg(type)); emit stalled(revision_); return true; }
            if (type=="call_ready") {
                window_=qBound(1,obj.value("window").toInt(1),5); credits_=window_;
                diagnosticReady_=true;readyWait_.observe(false,AudioEngine::nowMs());
                diagnosticCreditMin_=diagnosticCreditMin_<0 ? credits_ : qMin(diagnosticCreditMin_,credits_);
                creditWait_.observe(false,AudioEngine::nowMs());
                emit diagnostics(QStringLiteral("transport ready window=%1").arg(window_));
                emit mediaReady(revision_);
            } else { credits_=qMin(window_,credits_+1);creditWait_.observe(false,AudioEngine::nowMs()); }
        }
        return true;
    }
    if (type!="audio") return false;
    ++rxIpc_;
    const auto reject=[&](const QString &reason) {
        ++rxRejected_;
        if (!firstRejectReported_) {
            firstRejectReported_=true;
            emit diagnostics(QStringLiteral("rxReject reason=%1 hexChars=%2 head=%3")
                .arg(reason).arg(obj.value("hex").toString().size()).arg(obj.value("hex").toString().left(20)));
        }
    };
    if (!active_ || obj.value("epoch").toString()!=epoch_ ||
        obj.value("topic").toString()!=QStringLiteral("device/ycLinux/%1/audio").arg(deviceId_) ||
        obj.value("retain").toInt()!=0) { reject(QStringLiteral("state/epoch/topic/retain")); return true; }
    // 累计 token 确认释放小窗口，不在 IPC 内囤积无界 PCM。
    const auto token=obj.value("token");const auto number=token.toDouble(-1);
    if(token.isDouble() && number>=0 && number<=9007199254740991.0 && std::floor(number)==number && ackPrefix_.size()+24<=2048) {
        std::array<char,2048> line{};std::memcpy(line.data(),ackPrefix_.constData(),size_t(ackPrefix_.size()));
        auto *end=std::to_chars(line.data()+ackPrefix_.size(),line.data()+2046,quint64(number)).ptr;
        *end++='}';*end++='\n';ipc_->sendRealtimeLine(line.data(),int(end-line.data()));
    } else ipc_->sendJsonLine({{"cmd","audio_received"},{"epoch",epoch_},{"token",token}});
    const qint64 age=AudioEngine::nowMs()-qint64(obj.value("arrivalMs").toDouble());
    if (age<0 || age>200) { reject(QStringLiteral("age=%1ms").arg(age)); return true; }
    const QByteArray hex=obj.value("hex").toString().toLatin1();
    bool validHex=hex.size()==VoiceCall::FrameBytes*2;
    for (const char c:hex) if (!((c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F'))) { validHex=false; break; }
    if (!validHex) { reject(QStringLiteral("hex/length")); return true; }
    const QByteArray packet=QByteArray::fromHex(hex);
    VoiceCall::Frame decoded;
    if (!VoiceCall::decode(reinterpret_cast<const uint8_t *>(packet.constData()),packet.size(),0,decoded)) {
        reject(QStringLiteral("magic/version/size")); return true;
    }
    engine_->receive(packet); ++rxForwarded_;
    return true;
}
void CallAudioTransport::tick() {
    if (!active_) return;
    const auto now=AudioEngine::nowMs();
    if (now-lastTickAt_>tickGapMaxMs_) { tickGapStartMs_=lastTickAt_;tickGapEndMs_=now; }
    tickGapMaxMs_=qMax(tickGapMaxMs_,now-lastTickAt_);lastTickAt_=now;
    const auto pendingBytes=ipc_->pendingWriteBytes();
    ipcPendingPeakBytes_=qMax(ipcPendingPeakBytes_,pendingBytes);
    creditWait_.observe(diagnosticReady_ && credits_<=0,now);
    // 只统计原IPC背压分支能阻止发送的检查；信用不足/未到期不会重复算作IPC阻塞。
    if (credits_<=0 || nextSendAt_>now || !ipc_->isConnected()) ipcWait_.observe(false,now);
    if (now-lastReportAt_>=3000) {
        reportTransport(QStringLiteral("tick"),now);
        lastReportAt_=now;
    }
    if (credits_<=0) {
        if (AudioEngine::nowMs()-pendingSince_>500) {
            emit diagnostics(QStringLiteral("transport stalled waiting credit >500ms"));
            reportTransport(QStringLiteral("stalled"),AudioEngine::nowMs());active_=false;emit stalled(revision_);
        }
        return;
    }
    if (nextSendAt_>now) return;
    // 最多 5 个 IPC 提交 + mqttd 内有界队列；大业务背压时丢弃过期采集帧。
    if (!ipc_->isConnected()) { ++ipcDisconnectedTicks_;return; }
    const auto gatePendingBytes=ipc_->pendingWriteBytes();
    ipcPendingPeakBytes_=qMax(ipcPendingPeakBytes_,gatePendingBytes);
    ipcWait_.observe(gatePendingBytes>2048,now);
    if (gatePendingBytes>2048) return;
    VoiceCall::Frame frame;
    const auto takeResources=ThreadDiagnostics::sample();const auto takeStart=VoiceCall::monotonicUs();
    const bool captured=engine_->takeCaptured(frame);const auto takeUs=VoiceCall::monotonicUs()-takeStart;
    const auto takeDelta=ThreadDiagnostics::delta(takeResources,ThreadDiagnostics::sample(),takeUs);
    if(takeUs>takeMaxUs_) { takeMaxUs_=takeUs;takeMaxBeginUs_=takeStart;takeAtMax_=takeDelta; }
    if(!captured) return;
    const auto packet=VoiceCall::encode(frame,uint32_t(QDateTime::currentMSecsSinceEpoch()));
    pcmHash_=VoiceCall::pcmHash(frame.pcm);wireHash_=2166136261U;
    for(size_t i=VoiceCall::FrameBytes-VoiceCall::Samples*2;i<packet.size();++i) wireHash_=(wireHash_^packet[i])*16777619U;
    if(pcmHash_!=wireHash_) ++wireHashMismatches_;
    if (credits_==window_) pendingSince_=now;
    --credits_; ++txSubmitted_;
    if (diagnosticReady_) diagnosticCreditMin_=qMin(diagnosticCreditMin_,credits_);
    if (!nextSendAt_ || now-nextSendAt_>100) nextSendAt_=now;
    nextSendAt_+=20;
    const auto submitResources=ThreadDiagnostics::sample();const auto submitStart=VoiceCall::monotonicUs();
    bool submitted=false;
    if(audioPrefix_.size()+VoiceCall::FrameBytes*2+34<=2048) {
        std::array<char,2048> line{};std::memcpy(line.data(),audioPrefix_.constData(),size_t(audioPrefix_.size()));
        auto *end=line.data()+audioPrefix_.size();static constexpr char digits[]="0123456789abcdef";
        for(const auto byte:packet) { *end++=digits[byte>>4];*end++=digits[byte&15]; }
        static constexpr char suffix[]="\",\"ageMs\":";std::memcpy(end,suffix,sizeof(suffix)-1);end+=sizeof(suffix)-1;
        end=std::to_chars(end,line.data()+2046,qMax<int64_t>(0,now-frame.localMs)).ptr;*end++='}';*end++='\n';
        submitted=ipc_->sendRealtimeLine(line.data(),int(end-line.data()));
    } else {
        const QByteArray bytes(reinterpret_cast<const char *>(packet.data()),int(packet.size()));
        submitted=ipc_->sendJsonLine({{"cmd","publish_audio"},{"epoch",epoch_},
            {"topic",QStringLiteral("device/ycLinux/%1/audio/up").arg(deviceId_)},
            {"hex",QString::fromLatin1(bytes.toHex())},{"ageMs",double(qMax<int64_t>(0,now-frame.localMs))}});
    }
    const auto submitUs=VoiceCall::monotonicUs()-submitStart;const auto submitDelta=ThreadDiagnostics::delta(submitResources,ThreadDiagnostics::sample(),submitUs);
    if(submitUs>submitMaxUs_) { submitMaxUs_=submitUs;submitMaxBeginUs_=submitStart;submitAtMax_=submitDelta; }
    if(!submitted) { ++txSendFailures_;emit stalled(revision_); }
    else observeSubmitted(frame,now);
    ipcPendingPeakBytes_=qMax(ipcPendingPeakBytes_,ipc_->pendingWriteBytes());
}

void CallAudioTransport::observeSubmitted(const VoiceCall::Frame &frame,int64_t now)
{
    VoiceCall::LevelStats level;level.add(frame.pcm);
    txRms_=level.rms();txPcmPeak_=level.peak;txDc_=level.dc();txClip_+=level.clipped;
    const int boundary=txSubmitAt_ ? std::abs(int(frame.pcm.front())-lastPcmTail_) : 0;
    const auto skipped=txSubmitAt_ && frame.captureIndex>lastCaptureIndex_ ? frame.captureIndex-lastCaptureIndex_-1 : 0;
    const bool changed=txSubmitAt_ && lastProcessingMode_!=frame.processingMode;
    if((changed || skipped || boundary>12000 || level.peak>=12000 || level.clipped) && (txPcmReports_<3 || now-txPcmLastLogMs_>=5000)) {
        ++txPcmReports_;txPcmLastLogMs_=now;
        emit diagnostics(QStringLiteral("[call-tx-pcm] submitMonoMs=%1 index=%2 prevIndex=%3 skipped=%4 seq=%5 mode=%6 prevMode=%7 step=%8 rms=%9 peak=%10 dc=%11 clip=%12 ageMs=%13")
            .arg(now).arg(frame.captureIndex).arg(lastCaptureIndex_).arg(skipped).arg(frame.seq).arg(frame.processingMode).arg(lastProcessingMode_)
            .arg(boundary).arg(level.rms()).arg(level.peak).arg(level.dc()).arg(level.clipped).arg(now-frame.localMs)
            +QStringLiteral(" pcmHash=%1 wireHash=%2 hashMismatches=%3").arg(pcmHash_).arg(wireHash_).arg(wireHashMismatches_));
    }
    if(txSubmitAt_) {
        txStepMax_=std::max(txStepMax_,std::abs(int(frame.pcm.front())-lastPcmTail_));
        txSubmitGapMaxMs_=std::max(txSubmitGapMaxMs_,now-txSubmitAt_);
        if(frame.captureIndex>lastCaptureIndex_) txCaptureSkips_+=frame.captureIndex-lastCaptureIndex_-1;
        txModeChanges_+=lastProcessingMode_!=frame.processingMode;
    }
    lastPcmTail_=frame.pcm.back();lastCaptureIndex_=frame.captureIndex;lastProcessingMode_=frame.processingMode;
    txSubmitAt_=now;txAgeMaxMs_=std::max(txAgeMaxMs_,now-frame.localMs);
}

void CallAudioTransport::reportTransport(const QString &phase,int64_t now)
{
    emit diagnostics(QStringLiteral("transport txSubmit=%1 rxIpc=%2 rxForward=%3 rxReject=%4 credits=%5/%6 phase=%7")
        .arg(txSubmitted_).arg(rxIpc_).arg(rxForwarded_).arg(rxRejected_).arg(credits_).arg(window_).arg(phase)
        +QStringLiteral(" sendTickGapMaxMs=%1 ipcPendingPeakBytes=%2 creditMin=%3 readyWaitMs=%4")
        .arg(tickGapMaxMs_).arg(ipcPendingPeakBytes_).arg(diagnosticCreditMin_).arg(readyWait_.totalAt(now))
        +QStringLiteral(" sendTickGapStartMonoMs=%1 sendTickGapEndMonoMs=%2").arg(tickGapStartMs_).arg(tickGapEndMs_)
        +QStringLiteral(" ipcBackpressureTicks=%1 ipcBackpressureCount=%2 ipcBackpressureMs=%3 ipcBackpressureMaxMs=%4")
        .arg(ipcWait_.ticks).arg(ipcWait_.count).arg(ipcWait_.totalAt(now)).arg(ipcWait_.maxAt(now))
        +QStringLiteral(" creditWaitTicks=%1 creditWaitCount=%2 creditWaitMs=%3 creditWaitMaxMs=%4 ipcDisconnectedTicks=%5 txSendFail=%6")
        .arg(creditWait_.ticks).arg(creditWait_.count).arg(creditWait_.totalAt(now)).arg(creditWait_.maxAt(now))
        .arg(ipcDisconnectedTicks_).arg(txSendFailures_)
        +QStringLiteral(" txCaptureSkips=%1 txModeChanges=%2 txPcmStepMax=%3 txPcmRms=%4 txPcmPeak=%5 txPcmDc=%6 txPcmClip=%7 txSubmitGapMaxMs=%8 txAgeMaxMs=%9 lastCaptureIndex=%10 platformPlayback=unknown")
        .arg(txCaptureSkips_).arg(txModeChanges_).arg(txStepMax_).arg(txRms_).arg(txPcmPeak_).arg(txDc_).arg(txClip_)
        .arg(txSubmitGapMaxMs_).arg(txAgeMaxMs_).arg(lastCaptureIndex_)
        +QStringLiteral(" takeMaxUs=%1 takeBeginMonoUs=%2 takeCpuUs=%3 takeOffCpuUs=%4 takeVCtx=%5 takeIVCtx=%6 submitMaxUs=%7 submitBeginMonoUs=%8 submitCpuUs=%9 submitOffCpuUs=%10 submitVCtx=%11 submitIVCtx=%12 pcmHash=%13 wireHash=%14 hashMismatches=%15")
        .arg(takeMaxUs_).arg(takeMaxBeginUs_).arg(takeAtMax_.cpuUs).arg(takeAtMax_.offCpuUs).arg(takeAtMax_.voluntary).arg(takeAtMax_.involuntary)
        .arg(submitMaxUs_).arg(submitMaxBeginUs_).arg(submitAtMax_.cpuUs).arg(submitAtMax_.offCpuUs).arg(submitAtMax_.voluntary).arg(submitAtMax_.involuntary)
        .arg(pcmHash_).arg(wireHash_).arg(wireHashMismatches_));
}

/** @brief 线程启动后初始化原发送轮询并报告线程身份。 */
void CallAudioTransport::start() {
    timer_.start();emit diagnostics(QStringLiteral("transport audio-thread=%1").arg(quintptr(QThread::currentThreadId()),0,16));
}
/** @brief 退出时只在所属线程停表并复用原媒体关闭。 */
void CallAudioTransport::shutdown() { timer_.stop();media(false,revision_);QThread::currentThread()->quit(); }

/** @brief 断线的媒体停止在本线程完成，不受UI事件循环迟到影响。 */
void CallAudioTransport::connectionLost() { if(active_) { media(false,revision_);emit stalled(revision_); } }
