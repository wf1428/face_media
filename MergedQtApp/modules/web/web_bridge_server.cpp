/**
 * @file web_bridge_server.cpp
 * @brief 独立 C++ daemon 与 Qt 的受控本地通信入口；不监听网络端口。
 * @author Du lin
 * @date 2026-10-09
 */
#include "web_bridge_server.h"
#include "web_face_bridge.h"
#include "web_paths.h"
#include "common/sql/web/web_sql.h"
#include "platform/rk3566_platform.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QDebug>

namespace {
constexpr int maxFrame=1024*1024; /**< IPC 限额；二进制上传不经过 IPC。 */
/** @return 配置环境值，未设置时采用平台默认值。 */
QString setting(const char *key,const QString &fallback) { const auto value=qgetenv(key); return value.isEmpty()?fallback:QString::fromLocal8Bit(value); }
}
WebBridgeServer::WebBridgeServer(const WebServiceConfig &config,const QString &socketPath) : config_(config),socketPath_(socketPath) {}

void WebBridgeServer::start() {
    try {
        service_=new WebService(config_,this); service_->initialize();
        auto *bridge=WebFaceBridge::instance(); // 主线程 WebRuntime 已预创建，线程内只取得指针。
        connect(service_,&WebService::faceValidationRequested,bridge,&WebFaceBridge::validationRequested,Qt::QueuedConnection);
        connect(bridge,&WebFaceBridge::validationFinished,service_,&WebService::applyFaces,Qt::QueuedConnection);
        connect(service_,&WebService::personnelChanged,bridge,&WebFaceBridge::personnelChanged,Qt::QueuedConnection);
        if (!QFileInfo(socketPath_).absoluteDir().exists()) throw WebError(503,QStringLiteral("Web IPC 运行目录尚未由启动服务创建"));
        // 已运行的 listener 不能被 removeServer 无条件覆盖。
        QLocalSocket existing; existing.connectToServer(socketPath_);
        if (existing.waitForConnected(100)) throw WebError(503,QStringLiteral("Web IPC socket 已被其他服务使用"));
        QLocalServer::removeServer(socketPath_);
        server_=new QLocalServer(this);
        server_->setSocketOptions(QLocalServer::UserAccessOption | QLocalServer::GroupAccessOption);
        server_->setMaxPendingConnections(16);
        connect(server_,&QLocalServer::newConnection,this,&WebBridgeServer::acceptConnections);
        if (!server_->listen(socketPath_)) throw WebError(503,QStringLiteral("Web IPC socket 无法监听"));
        qInfo()<<"[WEB] IPC started"<<socketPath_;
    } catch (const std::exception &e) { qWarning()<<"[WEB] module disabled:"<<e.what(); stop(); }
}
void WebBridgeServer::stop() {
    if (server_) { server_->close(); delete server_; server_=nullptr; }
    delete service_; service_=nullptr; DbStore::close();
}
void WebBridgeServer::acceptConnections() {
    while (server_ && server_->hasPendingConnections()) {
        auto *socket=server_->nextPendingConnection();
        if (++clients_>16) { --clients_; socket->abort(); socket->deleteLater(); continue; }
        socket->setReadBufferSize(maxFrame+1);
        auto *timeout=new QTimer(socket); timeout->setSingleShot(true); timeout->start(10000);
        connect(timeout,&QTimer::timeout,socket,&QLocalSocket::abort);
        connect(socket,&QLocalSocket::disconnected,this,[this,socket]() { --clients_; socket->deleteLater(); });
        connect(socket,&QLocalSocket::readyRead,this,[this,socket,timeout]() {
            if (socket->property("handled").toBool()) return;
            if (socket->bytesAvailable()>maxFrame) { socket->abort(); return; }
            if (!socket->canReadLine()) return;
            socket->setProperty("handled",true); timeout->stop();
            QJsonParseError parse;
            const auto request=QJsonDocument::fromJson(socket->readLine(maxFrame+1),&parse).object();
            QJsonObject response={{"id",request.value("id")},{"version",1}};
            try {
                if (parse.error!=QJsonParseError::NoError || request.value("version").toInt()!=1 || !request.value("id").isString() || request.value("id").toString().size()>80 || !request.value("args").isObject()) throw WebError(400,QStringLiteral("IPC 请求格式或版本错误"));
                response.insert("data",service_->dispatch(request.value("operation").toString(),request.value("args").toObject(),request.value("owner").toInt()));
                response.insert("code",200);
            } catch (const WebError &e) { response.insert("code",e.status); response.insert("message",e.message); }
            catch (const std::exception &e) { qWarning()<<"[WEB] dispatch failed"<<e.what(); response.insert("code",500); response.insert("message",QStringLiteral("设备业务服务内部错误")); }
            const auto output=QJsonDocument(response).toJson(QJsonDocument::Compact)+'\n';
            if (output.size()>maxFrame) { socket->abort(); return; }
            socket->write(output); socket->disconnectFromServer();
        });
    }
}
WebRuntime::WebRuntime(QObject *parent) : QObject(parent) {
    if (qgetenv("MERGEDQT_WEB_ENABLE")!="1") return;
    WebFaceBridge::instance();
    const WebServiceConfig config={Rk3566Platform::databasePath(),QDir(Rk3566Platform::staticRoot()).filePath("demoResources/images/video"),setting("MERGEDQT_WEB_MEDIA_MOUNT",QStringLiteral("/mnt/UDISK")),setting("MERGEDQT_WEB_IMPORTS",QStringLiteral("/home/cat/web-stage/imports")),setting("MERGEDQT_WEB_FACTORY",QStringLiteral("/home/cat/web-stage/factory-account.json"))};
    server_=new WebBridgeServer(config,setting("MERGEDQT_WEB_SOCKET",QStringLiteral("/run/mergedqt/web.sock")));
    server_->moveToThread(&thread_);
    connect(&thread_,&QThread::started,server_,&WebBridgeServer::start);
    connect(&thread_,&QThread::finished,server_,&QObject::deleteLater);
    thread_.setObjectName(QStringLiteral("web-database-ipc")); thread_.start();
}
WebRuntime::~WebRuntime() {
    if (!thread_.isRunning()) return;
    QMetaObject::invokeMethod(server_,&WebBridgeServer::stop,Qt::BlockingQueuedConnection);
    thread_.quit(); thread_.wait();
}
