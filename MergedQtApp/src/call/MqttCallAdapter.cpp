/**
 * @file MqttCallAdapter.cpp
 * @brief 将通话控制排队至独立音频线程，媒体与IPC直接交互，不依赖UI服务。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-03；修改人：Du lin；原媒体实现迁至CallAudioTransport，代理保留信令接口；定时器及媒体ACK均在音频线程，代次校验异步控制通知。
 */
#include "MqttCallAdapter.h"
#include "components/settings/mqtt/mqttipcclient.h"
#include <QDateTime>
#include <QUuid>
MqttCallAdapter::MqttCallAdapter(MqttIpcClient *ipc,AudioEngine *engine,QObject *parent)
    :QObject(parent),ipc_(ipc),audioThread_(new QThread),transport_(new CallAudioTransport(ipc,engine))
{
    audioThread_->setObjectName(QStringLiteral("call.audio.transport"));transport_->moveToThread(audioThread_);
    connect(audioThread_,&QThread::started,transport_,&CallAudioTransport::start);
    connect(audioThread_,&QThread::finished,transport_,&QObject::deleteLater);
    connect(ipc_,&MqttIpcClient::mediaFrameReceived,transport_,&CallAudioTransport::handleIpc,Qt::QueuedConnection);
    connect(ipc_,&MqttIpcClient::disconnected,transport_,[transport=transport_]() { transport->connectionLost(); },Qt::QueuedConnection);
    connect(transport_,&CallAudioTransport::diagnostics,this,&MqttCallAdapter::diagnostics);
    connect(transport_,&CallAudioTransport::mediaReady,this,[this](quint64 revision) { if(revision==revision_) emit mediaReady(); });
    connect(transport_,&CallAudioTransport::stalled,this,[this](quint64 revision) { if(revision==revision_) emit stalled(); });
    audioThread_->start();
}
MqttCallAdapter::~MqttCallAdapter()
{
    auto *transport=transport_;
    QMetaObject::invokeMethod(transport,[transport]() { transport->shutdown(); },Qt::QueuedConnection);
    audioThread_->wait();delete audioThread_;
}
void MqttCallAdapter::configure(const QString &deviceId)
{
    if(deviceId_==deviceId) return;
    media(false);deviceId_=deviceId;
    auto *transport=transport_;QMetaObject::invokeMethod(transport,[transport,deviceId]() { transport->configure(deviceId); },Qt::QueuedConnection);
}
QString MqttCallAdapter::requestTopic() const { return deviceId_.isEmpty() ? QString() : QStringLiteral("device/ycLinux/%1/request").arg(deviceId_); }
bool MqttCallAdapter::event(const QString &method,const QJsonObject &data)
{
    if(deviceId_.isEmpty() || !ipc_->isConnected() || ipc_->pendingWriteBytes()>64*1024) return false;
    QJsonObject payload{{"method",method},{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"deviceId",deviceId_},{"data",data},{"time",QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")}};
    return ipc_->sendJsonLine({{"cmd","publish"},{"topic",QStringLiteral("device/ycLinux/%1/event").arg(deviceId_)},
        {"payload",payload},{"qos",2},{"retain",false}});
}
void MqttCallAdapter::media(bool on)
{
    const auto revision=++revision_;const auto reservation=on ? ipc_->reserveRealtime() : 0;auto *transport=transport_;
    QMetaObject::invokeMethod(transport,[transport,on,revision,reservation]() { transport->media(on,revision,reservation); },Qt::QueuedConnection);
}
bool MqttCallAdapter::handleIpc(const QJsonObject &object)
{
    const auto type=object.value("type").toString();if(type!="audio" && type!="call_ready" && type!="call_sent") return false;
    auto *transport=transport_;QMetaObject::invokeMethod(transport,[transport,object]() { transport->handleIpc(object); },Qt::QueuedConnection);return true;
}
