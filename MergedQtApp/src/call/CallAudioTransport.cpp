/**
 * @file CallAudioTransport.cpp
 * @brief 实现单 mqttd 的音频十六进制 IPC 封装和发送信用；MQTT 线上仍为 650B 二进制。
 * @author Du lin
 * @date 2026-11-03
 * @note 最近修改：2026-11-10；修改人：Du lin；媒体就绪后监测有效下行 PCM，15 秒无帧关闭媒体并通知原会话清理。
 * @details 原窗口、时序、编码及媒体校验移至独立音频线程；控制通知携带代次，收发不经过UI。
 */
#include "CallAudioTransport.h"
#include "components/settings/mqtt/mqttipcclient.h"
#include <QDateTime>
#include <QUuid>
#include <QThread>
#include <QJsonArray>
#include <QJsonDocument>
#include <charconv>
#include <cstring>
#include <cmath>

namespace {
constexpr qint64 DownlinkTimeoutMs=15000; /**< 无有效下行帧的单调毫秒上限。 */
}

CallAudioTransport::CallAudioTransport(MqttIpcClient *ipc,AudioEngine *engine,QObject *parent)
    :QObject(parent),ipc_(ipc),engine_(engine)
{
    timer_.setParent(this); // 代理构造后moveToThread，成员定时器必须随父对象迁移。
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
    if (!on && active_) reportTransport(true);
    active_=on; credits_=0; window_=1; nextSendAt_=0; lastDownlinkAt_=-1;
    if (on) {
        rxIpc_=rxForwarded_=rxRejected_=txSubmitted_=0; firstRejectReported_=false;
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
                if (lastDownlinkAt_<0) lastDownlinkAt_=pendingSince_;
                window_=qBound(1,obj.value("window").toInt(1),5); credits_=window_;
                emit diagnostics(QStringLiteral("transport ready window=%1").arg(window_));
                emit mediaReady(revision_);
            } else credits_=qMin(window_,credits_+1);
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
    const qint64 receivedAt=AudioEngine::nowMs();
    const qint64 age=receivedAt-qint64(obj.value("arrivalMs").toDouble());
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
    if (lastDownlinkAt_>=0) lastDownlinkAt_=receivedAt;
    engine_->receive(packet); ++rxForwarded_;
    return true;
}
void CallAudioTransport::tick() {
    if (!active_) return;
    const auto now=AudioEngine::nowMs();
    if (lastDownlinkAt_>=0 && now-lastDownlinkAt_>=DownlinkTimeoutMs) {
        emit diagnostics(QStringLiteral("downlink timeout no valid PCM for %1ms").arg(now-lastDownlinkAt_));
        media(false,revision_);
        emit stalled(revision_);
        return;
    }
    if (now-lastReportAt_>=3000) {
        reportTransport(false);
        lastReportAt_=now;
    }
    if (credits_<=0) {
        if (AudioEngine::nowMs()-pendingSince_>500) {
            emit diagnostics(QStringLiteral("transport stalled waiting credit >500ms"));
            active_=false;emit stalled(revision_);
        }
        return;
    }
    if (nextSendAt_>now) return;
    // 最多 5 个 IPC 提交 + mqttd 内有界队列；大业务背压时丢弃过期采集帧。
    if (!ipc_->isConnected()) return;
    const auto gatePendingBytes=ipc_->pendingWriteBytes();
    if (gatePendingBytes>2048) return;
    VoiceCall::Frame frame;
    if(!engine_->takeCaptured(frame)) return;
    const auto packet=VoiceCall::encode(frame,uint32_t(QDateTime::currentMSecsSinceEpoch()));
    if (credits_==window_) pendingSince_=now;
    --credits_; ++txSubmitted_;
    if (!nextSendAt_ || now-nextSendAt_>100) nextSendAt_=now;
    nextSendAt_+=20;
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
    if(!submitted) emit stalled(revision_);
}

void CallAudioTransport::reportTransport(bool final)
{
    const auto totals=QStringLiteral("transport txSubmit=%1 rxIpc=%2 rxForward=%3 rxReject=%4")
        .arg(txSubmitted_).arg(rxIpc_).arg(rxForwarded_).arg(rxRejected_);
    emit diagnostics(final ? totals : totals+QStringLiteral(" credits=%1/%2").arg(credits_).arg(window_));
}

/** @brief 线程启动后初始化原发送轮询。 */
void CallAudioTransport::start() {
    timer_.start();
}
/** @brief 退出时只在所属线程停表并复用原媒体关闭。 */
void CallAudioTransport::shutdown() { timer_.stop();media(false,revision_);QThread::currentThread()->quit(); }

/** @brief 断线的媒体停止在本线程完成，不受UI事件循环迟到影响。 */
void CallAudioTransport::connectionLost() { if(active_) { media(false,revision_);emit stalled(revision_); } }
