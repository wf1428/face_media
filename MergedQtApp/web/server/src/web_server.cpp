/**
 * @file web_server.cpp
 * @brief 登录、文件上传/获取/删除、Excel 本地导入 API 的实现。
 * @author Du lin
 * @date 2026-10-09
 */
#include "web_server.h"
#include <array>
#include <fstream>
#include <thread>
#include <regex>
#include <iostream>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#endif

namespace deviceweb {
ServerConfig ServerConfig::load(const std::string &path) {
    std::ifstream file(path); Json data; file>>data;
    ServerConfig config;
    config.bind=data.at("bind"); config.port=data.value("port",443); config.portalPort=data.value("portal_port",80);
    config.origin=data.at("origin"); config.socket=data.at("socket"); config.www=data.at("www");
    config.certificate=data.value("certificate",std::string{}); config.privateKey=data.value("private_key",std::string{});
    config.developmentHttp=data.value("development_http",false);
    config.deviceName=data.value("device_name",std::string("本地设备")); config.deviceId=data.value("device_id",std::string{}); config.wifiName=data.value("wifi_name",std::string{});
    if (!std::regex_match(config.origin,std::regex(R"(https?://[A-Za-z0-9.-]+(:[0-9]{1,5})?)")) || config.bind.empty() || config.socket.empty() || config.www.empty() || config.port<1 || config.port>65535 || config.portalPort<0 || config.portalPort>65535 || config.port==config.portalPort)
        throw std::runtime_error("invalid server configuration");
    if (config.developmentHttp && (config.bind!="127.0.0.1" || config.origin.rfind("http://",0)!=0 || config.portalPort!=0)) throw std::runtime_error("development HTTP requires loopback bind/origin and no portal");
    if (!config.developmentHttp && (config.origin.rfind("https://",0)!=0 || config.certificate.empty() || config.privateKey.empty())) throw std::runtime_error("production HTTPS requires certificate/key and HTTPS origin");
    return config;
}
WebServer::WebServer(ServerConfig config):config_(std::move(config)),rpc_(config_.socket) {
    if (config_.developmentHttp) server_=std::make_unique<httplib::Server>();
    else {
#ifdef DEVICEWEB_TLS
        server_=std::make_unique<httplib::SSLServer>(config_.certificate.c_str(),config_.privateKey.c_str());
#else
        throw std::runtime_error("this build has no TLS; production HTTPS is unavailable");
#endif
    }
    server_->new_task_queue=[]() { return new httplib::ThreadPool(4,16); };
    server_->set_payload_max_length(8ULL*1024*1024*1024);
    server_->set_read_timeout(30,0); server_->set_write_timeout(30,0);
    server_->set_keep_alive_max_count(20);
    if (!server_->is_valid() || !server_->set_mount_point("/",config_.www)) throw std::runtime_error("HTTPS credentials or static directory unavailable");
    routes();
}
Session WebServer::session(const httplib::Request &request) { return sessions_.get(sessionCookie(request.get_header_value("Cookie"))); }
void WebServer::result(httplib::Response &response,const Json &data,int status) {
    response.status=status;
    response.set_header("Cache-Control","no-store");
    response.set_content(Json{{"code",status},{"data",data}}.dump(),"application/json; charset=utf-8");
}
Json WebServer::readJson(const httplib::Request &request,const httplib::ContentReader &reader) {
    if (request.get_header_value("Content-Type").rfind("application/json",0)!=0 || request.get_header_value_u64("Content-Length",0)>131072) throw ApiError(400,"请使用限长 JSON 请求");
    std::string body;
    if (!reader([&](const char *data,size_t size) { if (body.size()+size>131072) return false; body.append(data,size); return true; })) throw ApiError(413,"JSON 请求读取失败或超出限制");
    Json data=Json::parse(body,nullptr,false);
    if (!data.is_object()) throw ApiError(400,"请求 JSON 格式错误");
    return data;
}
void WebServer::routes() {
    server_->set_pre_routing_handler([this](const httplib::Request &request,httplib::Response &response) {
        const std::string host=config_.origin.substr(config_.origin.find("://")+3);
        if (request.get_header_value("Host")!=host) { response.status=421; response.set_content("设备访问地址不正确","text/plain; charset=utf-8"); return httplib::Server::HandlerResponse::Handled; }
        if (request.path.rfind("/api/",0)!=0) return httplib::Server::HandlerResponse::Unhandled;
        try {
            if (request.method!="GET" && request.method!="HEAD") {
                if (request.get_header_value("Origin")!=config_.origin) throw ApiError(403,"请求来源无效");
                if (request.path!="/api/v1/auth/login" && !secureEqual(request.get_header_value("X-CSRF-Token"),session(request).csrf)) throw ApiError(403,"登录校验过期，请刷新页面");
            }
            if (request.path!="/api/v1/auth/login" && request.path!="/api/v1/device/info" && request.path!="/api/v1/health" && request.path!="/api/v1/captive") session(request);
            return httplib::Server::HandlerResponse::Unhandled;
        } catch (const ApiError &e) { response.status=e.status; response.set_content(Json{{"code",e.status},{"message",e.what()}}.dump(),"application/json; charset=utf-8"); return httplib::Server::HandlerResponse::Handled; }
    });
    server_->set_post_routing_handler([](const httplib::Request &,httplib::Response &response) {
        response.set_header("X-Content-Type-Options","nosniff"); response.set_header("Referrer-Policy","no-referrer");
        response.set_header("Content-Security-Policy","default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; object-src 'none'; frame-ancestors 'none'; base-uri 'none'");
    });
    server_->set_exception_handler([](const httplib::Request &,httplib::Response &response,std::exception_ptr error) {
        int code=500; std::string message="设备服务内部错误";
        try { std::rethrow_exception(error); }
        catch (const ApiError &e) { code=e.status; message=e.what(); }
        catch (const std::exception &e) { std::cerr<<"device-webd: "<<e.what()<<'\n'; }
        response.status=code; response.set_header("Cache-Control","no-store");
        response.set_content(Json{{"code",code},{"message",message}}.dump(),"application/json; charset=utf-8");
    });
    server_->set_error_handler([](const httplib::Request &,httplib::Response &response) {
        if (response.body.empty()) response.set_content(Json{{"code",response.status},{"message","请求失败或本版本未接入该操作"}}.dump(),"application/json; charset=utf-8");
    });
    server_->Get("/api/v1/device/info",[this](const auto &,auto &response) { result(response,{{"deviceName",config_.deviceName},{"deviceId",config_.deviceId},{"wifiName",config_.wifiName}}); });
    server_->Get("/api/v1/health",[this](const auto &,auto &response) { result(response,rpc_.call("health")); });
    server_->Get("/api/v1/captive",[this](const auto &,auto &response) { response.set_content(Json{{"captive",false},{"user-portal-url",config_.origin+"/#/login"}}.dump(),"application/captive+json"); });
    server_->Post("/api/v1/auth/login",[this](const httplib::Request &request,httplib::Response &response,const httplib::ContentReader &reader) {
        sessions_.attempt(request.remote_addr);
        const auto user=rpc_.call("auth.login",readJson(request,reader));
        const auto created=sessions_.create(user.at("id"),user.at("username"));
        response.set_header("Set-Cookie","device_session="+created.token+"; Path=/; HttpOnly; SameSite=Strict"+(config_.developmentHttp?"":"; Secure"));
        result(response,{{"username",created.username},{"csrf",created.csrf}});
    });
    server_->Get("/api/v1/auth/session",[this](const auto &request,auto &response) { const auto user=session(request); result(response,{{"username",user.username},{"csrf",user.csrf}}); });
    server_->Post("/api/v1/auth/logout",[this](const auto &request,auto &response,const httplib::ContentReader &reader) {
        readJson(request,reader); sessions_.erase(session(request).token);
        response.set_header("Set-Cookie",std::string("device_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0")+(config_.developmentHttp?"":"; Secure")); result(response,Json::object());
    });
    server_->Post("/api/v1/uploads",[this](const auto &request,auto &response,const httplib::ContentReader &reader) {
        auto data=rpc_.call("uploads.create",readJson(request,reader),session(request).owner); data.erase("part_path"); result(response,data);
    });
    server_->Put(R"(/api/v1/uploads/([a-f0-9-]{36})/content)",[this](const auto &request,auto &response,const httplib::ContentReader &reader) { upload(request,response,reader); });
    server_->Get(R"(/api/v1/uploads/([a-f0-9-]{36}))",[this](const auto &request,auto &response) { result(response,rpc_.call("uploads.status",{{"id",request.matches[1].str()}},session(request).owner)); });
    for (const std::string action:{"commit","cancel"}) server_->Post("/api/v1/uploads/([a-f0-9-]{36})/"+action,[this,action](const auto &request,auto &response,const httplib::ContentReader &reader) {
        readJson(request,reader);
        std::lock_guard<std::mutex> lock(uploadsMutex_);
        if (uploads_.count(request.matches[1].str())) throw ApiError(409,"文件正在传输，请等待连接结束");
        result(response,rpc_.call("uploads."+action,{{"id",request.matches[1].str()}},session(request).owner));
    });
    server_->Get("/api/v1/files",[this](const auto &request,auto &response) { result(response,rpc_.call("files.list",Json::object(),session(request).owner)); });
    server_->Get("/api/v1/persons",[this](const auto &request,auto &response) { result(response,rpc_.call("persons.list",Json::object(),session(request).owner)); });
    server_->Get(R"(/api/v1/persons/([0-9]+))",[this](const auto &request,auto &response) { result(response,rpc_.call("persons.get",{{"id",request.matches[1].str()}},session(request).owner)); });
    server_->Get(R"(/api/v1/files/([a-f0-9-]{36}|[a-f0-9]{64})/content)",[this](const auto &request,auto &response) { download(request,response); });
    server_->Delete(R"(/api/v1/files/([a-f0-9-]{36}|[a-f0-9]{64}))",[this](const auto &request,auto &response,const httplib::ContentReader &reader) {
        readJson(request,reader); const std::string id=request.matches[1];
        std::lock_guard<std::mutex> lock(downloadsMutex_);
        if (downloads_.count(id)) throw ApiError(423,"文件正在下载，请稍后重试");
        result(response,rpc_.call("files.delete",{{"id",id}},session(request).owner));
    });
    server_->Post("/api/v1/imports",[this](const auto &request,auto &response,const httplib::ContentReader &reader) { result(response,rpc_.call("imports.create",readJson(request,reader),session(request).owner)); });
    server_->Get(R"(/api/v1/imports/([a-f0-9-]{36}))",[this](const auto &request,auto &response) { result(response,rpc_.call("imports.status",{{"id",request.matches[1].str()}},session(request).owner)); });
    server_->Post(R"(/api/v1/imports/([a-f0-9-]{36})/start)",[this](const auto &request,auto &response,const httplib::ContentReader &reader) { readJson(request,reader); result(response,rpc_.call("imports.start",{{"id",request.matches[1].str()}},session(request).owner)); });
    server_->Post(R"(/api/v1/imports/([a-f0-9-]{36})/discard)",[this](const auto &request,auto &response,const httplib::ContentReader &reader) { readJson(request,reader); result(response,rpc_.call("imports.discard",{{"id",request.matches[1].str()}},session(request).owner)); });
}

void WebServer::upload(const httplib::Request &request,httplib::Response &response,const httplib::ContentReader &reader) {
#ifdef _WIN32
    (void)request; (void)response; (void)reader; throw ApiError(503,"上传仅在 Linux 板端提供");
#else
    const auto user=session(request); const std::string id=request.matches[1];
    if (request.get_header_value("Content-Type")!="application/octet-stream" || request.has_header("Transfer-Encoding")) throw ApiError(400,"请上传完整原始文件并提供长度");
    {
        std::lock_guard<std::mutex> lock(uploadsMutex_);
        if (!uploads_.insert(id).second) throw ApiError(409,"此任务正在接收，不能重复上传");
    }
    struct ReceivingLease { WebServer *server; std::string id; ~ReceivingLease() { std::lock_guard<std::mutex> lock(server->uploadsMutex_); server->uploads_.erase(id); } } lease{this,id};
    const auto started=rpc_.call("uploads.begin",{{"id",id}},user.owner);
    const auto expected=started.at("size").get<uint64_t>();
    struct Descriptor { int fd=-1; ~Descriptor() { if (fd>=0) ::close(fd); } } output;
    try {
        if (!request.has_header("Content-Length") || request.get_header_value_u64("Content-Length")!=expected) throw ApiError(400,"文件长度与任务声明不一致");
        const std::string path=started.at("part_path");
        output.fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0640);
        if (output.fd<0 || ::flock(output.fd,LOCK_EX|LOCK_NB)!=0) throw ApiError(409,"无法创建临时文件，任务可能正在接收");
        uint64_t received=0,checkpoint=0;
        const auto began=std::chrono::steady_clock::now();
        auto lastSessionCheck=began;
        const bool complete=reader([&](const char *data,size_t size) {
            const auto now=std::chrono::steady_clock::now();
            if (received+size>expected || now-began>std::chrono::hours(2)) return false;
            // 实际传输是活动操作，每分钟刷新空闲期限；绝对期限/注销仍有效。
            if (now-lastSessionCheck>=std::chrono::minutes(1)) { sessions_.get(user.token); lastSessionCheck=now; }
            size_t offset=0;
            while (offset<size) {
                const auto written=::write(output.fd,data+offset,size-offset);
                if (written<0 && errno==EINTR) continue;
                if (written<=0) return false;
                offset+=static_cast<size_t>(written);
            }
            received+=size;
            if (received-checkpoint>=16*1024*1024) { rpc_.call("uploads.progress",{{"id",id}},user.owner); checkpoint=received; }
            return true;
        });
        if (!complete || received!=expected) throw ApiError(400,"上传中断或数据长度不完整");
        if (::fsync(output.fd)!=0) throw ApiError(507,"存储同步失败");
        ::close(output.fd); output.fd=-1;
        result(response,rpc_.call("uploads.received",{{"id",id}},user.owner));
    } catch (...) {
        if (output.fd>=0) { ::close(output.fd); output.fd=-1; }
        try { rpc_.call("uploads.failed",{{"id",id}},user.owner); } catch (...) { /* Qt 失联时保留临时文件，由 Qt 重启恢复流程核对。 */ }
        throw;
    }
#endif
}
void WebServer::download(const httplib::Request &request,httplib::Response &response) {
    const auto user=session(request); const std::string id=request.matches[1];
    std::lock_guard<std::mutex> lock(downloadsMutex_);
    const auto data=rpc_.call("files.get",{{"id",id}},user.owner);
    const std::string path=data.at("path");
    auto file=std::make_shared<std::ifstream>(path,std::ios::binary);
    if (!*file) throw ApiError(404,"文件读取失败");
    const uint64_t size=data.at("bytes").get<uint64_t>();
    ++downloads_[id];
    // 固定下载文件名，不把原始中文/标点名字直接写进 HTTP header。
    response.set_header("Content-Disposition","attachment; filename=\"video"+path.substr(path.find_last_of('.'))+"\"");
    response.set_content_provider(static_cast<size_t>(size),"application/octet-stream",[file,this,token=user.token,lastCheck=std::chrono::steady_clock::now()](size_t offset,size_t length,httplib::DataSink &sink) mutable {
        const auto now=std::chrono::steady_clock::now();
        if (now-lastCheck>=std::chrono::minutes(1)) {
            try { sessions_.get(token); lastCheck=now; } catch (const ApiError &) { return false; }
        }
        std::array<char,64*1024> buffer{}; file->clear(); file->seekg(static_cast<std::streamoff>(offset));
        const auto count=std::min(length,buffer.size()); file->read(buffer.data(),static_cast<std::streamsize>(count));
        const auto actual=file->gcount(); return actual>0 && sink.write(buffer.data(),static_cast<size_t>(actual));
    },[this,id,file](bool) { file->close(); std::lock_guard<std::mutex> guard(downloadsMutex_); if (--downloads_[id]==0) downloads_.erase(id); });
}
bool WebServer::run() {
    std::thread portalThread;
    if (config_.portalPort) {
        portal_.new_task_queue=[]() { return new httplib::ThreadPool(2,8); };
        portal_.set_payload_max_length(4096);
        portal_.set_pre_routing_handler([this](const auto &,auto &response) { response.set_redirect(config_.origin+"/#/login",302); response.set_header("Cache-Control","no-store"); return httplib::Server::HandlerResponse::Handled; });
        if (!portal_.bind_to_port(config_.bind,config_.portalPort)) return false;
        portalThread=std::thread([this]() { portal_.listen_after_bind(); });
    }
    bool ok=false;
    try { ok=server_->listen(config_.bind,config_.port); }
    catch (...) { portal_.stop(); if (portalThread.joinable()) portalThread.join(); throw; }
    portal_.stop(); if (portalThread.joinable()) portalThread.join(); return ok;
}
void WebServer::stop() { server_->stop(); portal_.stop(); }
}
