/**
 * @file web_bridge_server.cpp
 * @brief 独立 C++ daemon 与 Qt 的受控本地通信入口；不监听网络端口。
 * @author Du lin
 * @date 2026-10-09
 */

#include "web_bridge_server.h"
#include "web_face_bridge.h"
#include "web_local_runtime.h"
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

constexpr int maxFrame = 1024 * 1024; /**< IPC 限额；二进制上传不经过 IPC。 */

/**
 * @brief 读取环境变量配置，未提供时使用受控默认值。
 * @param key 待查询的配置键名。
 * @param fallback 配置未提供时使用的默认值。
 * @return 配置环境值，未设置时采用平台默认值。
 */
QString setting(const char *key, const QString &fallback)
{
    const auto value = qgetenv(key);
    return value.isEmpty() ? fallback : QString::fromLocal8Bit(value);
}
} // namespace

/**
 * @brief 保存 Qt 业务配置和 Unix socket 路径。
 * @param config 设备受控配置，包含运行所需路径或功能设置。
 * @param socketPath 本机 Unix socket 路径。
 */
WebBridgeServer::WebBridgeServer(const WebServiceConfig &config, const QString &socketPath)
    : config_(config), socketPath_(socketPath)
{
}

/**
 * @brief 启动独立 Web 业务线程并准备本地 socket。
 */
void WebBridgeServer::start()
{
    try {
        // IPC 与业务对象都运行在独立工作线程，不占用 Qt 主界面事件循环。
        service_ = new WebService(config_, this);
        service_->initialize();
        auto *bridge = WebFaceBridge::instance(); // 主线程 WebRuntime 已预创建，线程内只取得指针。
        connect(service_,
                &WebService::faceValidationRequested,
                bridge,
                &WebFaceBridge::validationRequested,
                Qt::QueuedConnection);
        connect(bridge,
                &WebFaceBridge::validationFinished,
                service_,
                &WebService::applyFaces,
                Qt::QueuedConnection);
        connect(service_,
                &WebService::personnelChanged,
                bridge,
                &WebFaceBridge::personnelChanged,
                Qt::QueuedConnection);

        // 只接管未被占用的指定 socket，监听权限限制为当前用户和组。
        if (!QFileInfo(socketPath_).absoluteDir().exists())
            throw WebError(503, QStringLiteral("Web IPC 运行目录尚未由启动服务创建"));

        // 已运行的 listener 不能被 removeServer 无条件覆盖。
        QLocalSocket existing;
        existing.connectToServer(socketPath_);
        if (existing.waitForConnected(100))
            throw WebError(503, QStringLiteral("Web IPC socket 已被其他服务使用"));
        QLocalServer::removeServer(socketPath_);
        server_ = new QLocalServer(this);
        server_->setSocketOptions(QLocalServer::UserAccessOption | QLocalServer::GroupAccessOption);
        server_->setMaxPendingConnections(16);
        connect(server_, &QLocalServer::newConnection, this, &WebBridgeServer::acceptConnections);
        if (!server_->listen(socketPath_))
            throw WebError(503, QStringLiteral("Web IPC socket 无法监听"));
        qInfo() << "[WEB] IPC started" << socketPath_;
    } catch (const std::exception &e) {
        qWarning() << "[WEB] module disabled:" << e.what();
        stop();
    }
}

/**
 * @brief 停止接收并清理 Web 业务线程资源。
 */
void WebBridgeServer::stop()
{
    if (server_) {
        server_->close();
        delete server_;
        server_ = nullptr;
    }
    delete service_;
    service_ = nullptr;
    DbStore::close();
}

/**
 * @brief 接收独立 HTTP 进程发来的本地连接。
 */
void WebBridgeServer::acceptConnections()
{
    // 每个连接只处理一帧有限长 JSON，上传字节始终走 HTTP 文件流。
    while (server_ && server_->hasPendingConnections()) {
        auto *socket = server_->nextPendingConnection();
        if (++clients_ > 16) {
            --clients_;
            socket->abort();
            socket->deleteLater();
            continue;
        }
        socket->setReadBufferSize(maxFrame + 1);
        auto *timeout = new QTimer(socket);
        timeout->setSingleShot(true);
        timeout->start(10000);
        connect(timeout, &QTimer::timeout, socket, &QLocalSocket::abort);
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            --clients_;
            socket->deleteLater();
        });

        // 读到完整一行后才校验协议版本和参数并交给业务层。
        connect(socket, &QLocalSocket::readyRead, this, [this, socket, timeout]() {
            if (socket->property("handled").toBool())
                return;
            if (socket->bytesAvailable() > maxFrame) {
                socket->abort();
                return;
            }
            if (!socket->canReadLine())
                return;
            socket->setProperty("handled", true);
            timeout->stop();
            QJsonParseError parse;
            const auto request =
                QJsonDocument::fromJson(socket->readLine(maxFrame + 1), &parse).object();
            QJsonObject response = {{"id", request.value("id")}, {"version", 1}};
            try {
                if (parse.error != QJsonParseError::NoError ||
                    request.value("version").toInt() != 1 || !request.value("id").isString() ||
                    request.value("id").toString().size() > 80 || !request.value("args").isObject())
                    throw WebError(400, QStringLiteral("IPC 请求格式或版本错误"));
                response.insert("data",
                                service_->dispatch(request.value("operation").toString(),
                                                   request.value("args").toObject(),
                                                   request.value("owner").toInt()));
                response.insert("code", 200);
            } catch (const WebError &e) {
                response.insert("code", e.status);
                response.insert("message", e.message);
            } catch (const std::exception &e) {
                qWarning() << "[WEB] dispatch failed" << e.what();
                response.insert("code", 500);
                response.insert("message", QStringLiteral("设备业务服务内部错误"));
            }
            const auto output = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
            if (output.size() > maxFrame) {
                socket->abort();
                return;
            }
            socket->write(output);
            socket->disconnectFromServer();
        });
    }
}

/**
 * @brief 按环境配置创建 Qt Web IPC 运行对象。
 * @param parent Qt 父对象；对象销毁由父子关系管理，空指针表示无父对象。
 */
WebRuntime::WebRuntime(QObject *parent) : QObject(parent)
{
    if (qgetenv("MERGEDQT_WEB_ENABLE") != "1")
        return;
    WebFaceBridge::instance();
    WebLocalRuntime::instance(); // create bridge on UI thread before the Web worker
    const WebServiceConfig config = {
        Rk3566Platform::databasePath(),
        QDir(Rk3566Platform::staticRoot()).filePath("demoResources/images/video"),
        setting("MERGEDQT_WEB_MEDIA_MOUNT", QStringLiteral("/mnt/UDISK")),
        setting("MERGEDQT_WEB_IMPORTS", QStringLiteral("/home/cat/web-stage/imports")),
        setting("MERGEDQT_WEB_FACTORY", QStringLiteral("/home/cat/web-stage/factory-account.json")),
        Rk3566Platform::netConfigPath(),
        setting("MERGEDQT_WEB_HOTSPOT_HELPER",
                QStringLiteral("/usr/local/sbin/portal-network.sh")),
        setting("MERGEDQT_WEB_READY_FILE", QStringLiteral("/run/mergedqt/web-ready"))};
    server_ = new WebBridgeServer(
        config,
        setting("MERGEDQT_WEB_SOCKET", QStringLiteral("/run/mergedqt/web.sock")));
    server_->moveToThread(&thread_);
    connect(&thread_, &QThread::started, server_, &WebBridgeServer::start);
    connect(&thread_, &QThread::finished, server_, &QObject::deleteLater);
    thread_.setObjectName(QStringLiteral("web-database-ipc"));
    thread_.start();
}

/**
 * @brief 停止 IPC 服务并释放运行对象。
 */
WebRuntime::~WebRuntime()
{
    if (!thread_.isRunning())
        return;
    QMetaObject::invokeMethod(server_, &WebBridgeServer::stop, Qt::BlockingQueuedConnection);
    thread_.quit();
    thread_.wait();
}
