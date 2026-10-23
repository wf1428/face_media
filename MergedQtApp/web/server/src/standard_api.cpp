/**
 * @file standard_api.cpp
 * @brief 标准 HTTP API 与 Qt 本地业务指令的受控适配。
 * @author Du lin
 * @date 2026-10-16
 */

#include "web_server.h"
#include <chrono>
#include <fstream>
#include <thread>
#include <functional>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace deviceweb {
namespace {

/**
 * @brief 递归将受控图像元数据转换为 HTTP Base64，并限制单次图片总量。
 * @param[in,out] budget 剩余允许展开的数据预算，展开时递减。
 * @param[in,out] value 包含受控图片资源引用的结果树，处理后将引用替换为 Base64。
 */
void hydrate(Json &value, size_t &budget)
{
    if (value.is_array()) {
        for (auto &v : value)
            hydrate(v, budget);
        return;
    }
    if (!value.is_object())
        return;

    // 图片只从 Qt 返回的受控路径读取，预算在整个响应中累计扣减。
    if (value.contains("_assetPath")) {
        const auto path = value.at("_assetPath").get<std::string>();
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file || file.tellg() < 0 || static_cast<size_t>(file.tellg()) > budget)
            throw ApiError(413, "图像不可读或单次返回图像过多，请分项查看");
        const auto size = static_cast<size_t>(file.tellg());
        budget -= size;
        std::string bytes(size, '\0');
        file.seekg(0);
        if (size && !file.read(&bytes[0], static_cast<std::streamsize>(size)))
            throw ApiError(503, "图像读取失败");
        const auto target = value.value("_assetTarget", std::string("faceBase64"));
        if (target != "faceBase64" && target != "base64Image")
            throw ApiError(503, "图像类型无效");
        const bool png = bytes.size() >= 8 && bytes.compare(1, 3, "PNG") == 0;
        value.erase("_assetPath");
        value.erase("_assetTarget");
        value[target] = std::string(png ? "data:image/png;base64," : "data:image/jpeg;base64,") +
                        httplib::detail::base64_encode(bytes);
    }
    for (auto &v : value.items())
        hydrate(v.value(), budget);
}

/**
 * @brief 要求 JSON 对象并拒绝客户端传入保留内部字段。
 * @param args 操作对应的结构化参数。
 */
void checkArguments(const Json &args)
{
    if (!args.is_object())
        throw ApiError(400, "请求必须是JSON对象");
    for (const auto &v : args.items())
        if (!v.key().empty() && v.key()[0] == '_')
            throw ApiError(400, "请求包含保留字段");
}

/**
 * @brief 限长解码人脸 Base64，校验字符、填充和解码后的字节数。
 * @param value Base64 文本或带 MIME 头的 Data URL。
 * @return 解码后的原始图片字节；无效编码或超出 512 KiB 时抛出 ApiError。
 */
[[maybe_unused]] std::string imageBytes(const Json &value)
{
    if (!value.is_string())
        throw ApiError(400, "人脸图片必须为Base64文本");
    auto encoded = value.get<std::string>();
    if (encoded.rfind("data:", 0) == 0) {
        const auto comma = encoded.find(',');
        if (comma == std::string::npos)
            throw ApiError(400, "图片编码无效");
        encoded.erase(0, comma + 1);
    }
    if (encoded.empty() || encoded.size() > 700000 || encoded.size() % 4)
        throw ApiError(413, "单张人脸图片最多512KiB");

    // 逐字符解码并验证末尾填充，拒绝非法字符和非零尾部残余位。
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string bytes;
    bytes.reserve(encoded.size() / 4 * 3);
    unsigned valueBits = 0;
    int count = 0;
    size_t padding = 0;
    for (size_t i = 0; i < encoded.size(); ++i) {
        const char c = encoded[i];
        if (c == '=') {
            if (++padding > 2 || i < encoded.size() - 2)
                throw ApiError(400, "图片Base64无效");
            continue;
        }
        const auto pos = alphabet.find(c);
        if (pos == std::string::npos || padding)
            throw ApiError(400, "图片Base64无效");
        valueBits = (valueBits << 6) | unsigned(pos);
        count += 6;
        if (count >= 8) {
            count -= 8;
            bytes.push_back(char((valueBits >> count) & 255));
        }
    }
    if (bytes.empty() || bytes.size() > 512 * 1024)
        throw ApiError(413, "单张人脸图片最多512KiB");
    if (count && ((valueBits & ((1u << count) - 1)) != 0))
        throw ApiError(400, "图片Base64无效");
    return bytes;
}
} // namespace

/**
 * @brief 复用会话并转换标准参数，处理图片暂存、任务结果和 HTTP 返回。
 * @param op 白名单业务操作名。
 * @param request 当前 HTTP 请求，身份由会话校验取得。
 * @param response 待写入的 HTTP 响应。
 * @param args 操作对应的结构化参数。
 */
void WebServer::standardCall(const std::string &op,
                             const httplib::Request &request,
                             httplib::Response &response,
                             Json args)
{
    checkArguments(args);

    // 所有标准业务沿用原会话，并将客户端幂等键传给本地任务层。
    const auto current = session(request);
    const auto key = request.get_header_value("Idempotency-Key");
    if (key.size() > 128)
        throw ApiError(400, "幂等标识过长");
    if (!key.empty())
        args["_key"] = key;
    Json packet;
    if (op == "credential.addSubItem" && args.value("subType", std::string()) == "FACE") {
#ifdef _WIN32
        throw ApiError(503, "实际人脸图片接收需要Linux设备");
#else
        const auto input = args.value("subData", Json::object());
        checkArguments(input);
        for (const auto &item : input.items())
            if (item.key() != "faceBase64" && item.key() != "name")
                throw ApiError(400, "人脸字段无效");
        const auto bytes = imageBytes(input.value("faceBase64", Json()));

        // 先向 Qt 申请受控资源句柄，HTTP 仅向该路径写入已限长的图片。
        const auto staged = rpc_.call("local.face.stage", Json::object(), current.owner).at("data");
        const auto assetId = staged.at("assetId");
        const auto discard = [&]() {
            try {
                rpc_.call("local.face.discard", {{"assetId", assetId}}, current.owner);
            } catch (...) {
            }
        };
        try {
            const auto path = staged.at("path").get<std::string>();
            const int fd =
                ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd < 0)
                throw ApiError(503, "图片暂存文件无法创建");
            size_t offset = 0;
            bool ok = true;
            while (offset < bytes.size()) {
                const auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0) {
                    ok = false;
                    break;
                }
                offset += size_t(n);
            }
            if (ok && ::fsync(fd) != 0)
                ok = false;
            ::close(fd);
            if (!ok)
                throw ApiError(503, "图片写入失败");
            args["subData"] = {{"assetId", assetId}, {"name", input.value("name", std::string())}};
            packet = rpc_.call("local." + op, args, current.owner);
            discard();
        } catch (...) {
            discard();
            throw;
        }
#endif
    } else
        packet = rpc_.call("local." + op, args, current.owner);

    // 截图只在 HTTP 工作线程有限等待；Qt 工作线程通过异步结果完成任务。
    if (op == "device.screenshot") {
        auto data = packet.at("data");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (data.value("state", std::string()) == "PENDING" &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            data =
                rpc_.call("local.operations.get", {{"id", data.at("operationId")}}, current.owner)
                    .at("data");
        }
        if (data.value("state", std::string()) == "APPLIED")
            packet["data"] = data.at("result");
        else if (data.value("state", std::string()) != "PENDING")
            throw ApiError(data.value("code", 503), data.value("message", std::string("截图失败")));
        else
            packet["data"] = data;
    }

    // 口令更新成功后撤销该账号全部会话，并清除当前 Cookie。
    if (op == "auth.changePassword") {
        sessions_.eraseOwner(current.owner);
        response.set_header(
            "Set-Cookie",
            std::string("device_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0") +
                (config_.developmentHttp ? "" : "; Secure"));
    }

    // 将内部资源路径转换为 Base64 后再返回浏览器，避免暴露设备路径。
    size_t budget = 6 * 1024 * 1024;
    hydrate(packet, budget);
    response.status = 200;
    response.set_header("Cache-Control", "no-store");
    response.set_content(Json{{"code", 200},
                              {"message", "ok"},
                              {"data", packet.at("data")},
                              {"meta", packet.value("meta", Json::object())}}
                             .dump(),
                         "application/json; charset=utf-8");
}

/**
 * @brief 登记标准 API 白名单，将路径、参数及业务指令显式对应。
 */
void WebServer::standardRoutes()
{

    // 公开身份读取和登录单独适配；其余路由统一走会话保护。
    server_->Get("/api/device/info", [this](const auto &, auto &response) {
        auto info = rpc_.call("device.info");
        info["deviceIp"] = config_.bind;
        result(response, info);
    });
    server_->Post(
        "/api/device/verify-password",
        [this](const auto &request, auto &response, const httplib::ContentReader &reader) {
            sessions_.attempt(request.remote_addr);
            const Json args = readJson(request, reader);
            if (!args.contains("username") || !args.at("username").is_string() ||
                args.at("username").get<std::string>().empty())
                throw ApiError(400, "请输入账号");
            const auto user = rpc_.call("auth.login", args);
            const auto created = sessions_.create(user.at("id"), user.at("username"));
            response.set_header("Set-Cookie",
                                "device_session=" + created.token +
                                    "; Path=/; HttpOnly; SameSite=Strict" +
                                    (config_.developmentHttp ? "" : "; Secure"));
            result(
                response,
                {{"authenticated", true}, {"username", created.username}, {"csrf", created.csrf}});
        });
    server_->Post(
        "/api/device/settings/update",
        [this](const auto &request, auto &response, const httplib::ContentReader &reader) {
            const Json args = readJson(request, reader);
            checkArguments(args);
            for (const auto &v : args.items())
                if (v.key() != "deviceName" && v.key() != "wifiName" && v.key() != "wifiPassword")
                    throw ApiError(400, "设备设置字段无效");
            if (args.contains("wifiPassword") && !args.at("wifiPassword").is_null() &&
                !args.at("wifiPassword").is_string())
                throw ApiError(400, "热点密码字段无效");
            if (!args.contains("wifiName") || !args.at("wifiName").is_string())
                throw ApiError(400, "请输入SSID");
            if (args.contains("deviceName") && args.at("deviceName") != args.at("wifiName"))
                throw ApiError(400, "设备名须与SSID一致");
            Json wifi{{"ssid", args.at("wifiName")}};
            if (args.contains("wifiPassword") && args.at("wifiPassword").is_string())
                wifi["password"] = args.at("wifiPassword");
            result(response, rpc_.call("device.wifi.save", wifi, session(request).owner));
        });

    // GET 路由只转换路径及分页参数，业务结果统一由 standardCall 解包。
    const std::pair<const char *, const char *> gets[] = {
        {"/api/device/overview", "device.overview"},
        {"/api/device/capabilities", "device.capabilities"},
        {"/api/device/network", "device.networkConfig.get"},
        {"/api/person/list", "person.list"},
        {"/api/person/detail/([0-9]+)", "person.detail"},
        {"/api/credential/getByPerson/([0-9]+)", "credential.get"},
        {"/api/operations/([a-f0-9-]{36})", "operations.get"}};
    for (const auto &route : gets) {
        const std::string op = route.second;
        server_->Get(route.first, [this, op](const auto &request, auto &response) {
            Json args = Json::object();
            if (request.matches.size() > 1)
                args[op == "credential.get" ? "personId" : "id"] = request.matches[1].str();
            if (op == "person.list")
                for (const auto &name : {"offset", "limit"})
                    if (request.has_param(name))
                        args[name] = request.get_param_value(name);
            standardCall(op, request, response, args);
        });
    }

    // POST 表包含查询型 POST 与写入型 POST；两者都沿用来源和 CSRF 校验。
    const std::pair<const char *, const char *> posts[] = {
        {"/api/device/change-password", "auth.changePassword"},
        {"/api/device/network", "device.networkConfig.set"},
        {"/api/device/screenshot", "device.screenshot"},
        {"/api/device/reboot", "device.reboot"},
        {"/api/device/linux-reboot", "device.linux-reboot"},
        {"/api/person/add", "person.add"},
        {"/api/credential/add", "credential.add"},
        {"/api/credential/addSubItem", "credential.addSubItem"},
        {"/api/credential/removeSubItem", "credential.removeSubItem"},
        {"/api/person/faceRegisterResult", "person.faceRegisterResult"},
        {"/api/personFloor/save", "personFloor.save"},
        {"/api/elevator/floorLimit/set", "elevator.floorLimit.set"},
        {"/api/elevator/floorLimit/get", "elevator.floorLimit.get"},
        {"/api/elevator/floorSchedule/set", "elevator.floorSchedule.set"},
        {"/api/elevator/floorSchedule/get", "elevator.floorSchedule.get"},
        {"/api/elevator/floorSchedule/delete", "elevator.floorSchedule.delete"},
        {"/api/elevator/floorSchedule/resend", "elevator.floorSchedule.resend"},
        {"/api/elevator/elevatorMode/set", "elevator.elevatorMode.set"},
        {"/api/elevator/elevatorMode/get", "elevator.elevatorMode.get"},
        {"/api/elevator/screenSleep/set", "elevator.screenSleep.set"},
        {"/api/elevator/screenSleep/get", "elevator.screenSleep.get"},
        {"/api/device/airControl/set", "device.airControl.set"},
        {"/api/device/airControl/get", "device.airControl.get"},
        {"/api/device/getPlayInfo", "device.getPlayInfo"},
        {"/api/device/videoControl", "device.videoControl"}};
    for (const auto &route : posts) {
        const std::string op = route.second;
        server_->Post(
            route.first,
            [this, op](const auto &request, auto &response, const httplib::ContentReader &reader) {
                Json args;
                if (op == "credential.addSubItem") {
                    if (request.get_header_value("Content-Type").rfind("application/json", 0) != 0)
                        throw ApiError(400, "请使用JSON请求");
                    std::string body;
                    if (!reader([&](const char *data, size_t n) {
                            if (body.size() + n > 1024 * 1024)
                                return false;
                            body.append(data, n);
                            return true;
                        }))
                        throw ApiError(413, "凭证请求超过1MiB");
                    args = Json::parse(body, nullptr, false);
                    if (args.is_discarded())
                        throw ApiError(400, "JSON无效");
                } else
                    args = readJson(request, reader);
                standardCall(op, request, response, args);
            });
    }
    const std::pair<const char *, const char *> puts[] = {
        {"/api/person/update", "person.update"},
        {"/api/credential/update", "credential.update"}};
    for (const auto &route : puts) {
        const std::string op = route.second;
        server_->Put(
            route.first,
            [this, op](const auto &request, auto &response, const httplib::ContentReader &reader) {
                standardCall(op, request, response, readJson(request, reader));
            });
    }
    const std::pair<const char *, const char *> deletes[] = {
        {"/api/person/delete", "person.delete"},
        {"/api/personFloor/delete", "personFloor.delete"}};
    for (const auto &route : deletes) {
        const std::string op = route.second;
        server_->Delete(
            route.first,
            [this, op](const auto &request, auto &response, const httplib::ContentReader &reader) {
                standardCall(op, request, response, readJson(request, reader));
            });
    }
    server_->Get("/api/playFile/list", [this](const auto &request, auto &response) {
        auto files = rpc_.call("files.list", Json::object(), session(request).owner).at("files");
        for (auto &file : files) {
            file["size"] = std::to_string(file.value("bytes", uint64_t(0)) / 1024) + " KiB";
            file["duration"] = nullptr;
        }
        result(response, files);
    });
    server_->Delete(
        "/api/playFile/delete",
        [this](const auto &request, auto &response, const httplib::ContentReader &reader) {
            const Json args = readJson(request, reader);
            const auto id = args.value("id", args.value("fileId", std::string()));
            std::lock_guard<std::mutex> lock(downloadsMutex_);
            if (downloads_.count(id))
                throw ApiError(423, "文件正在下载");
            auto out = rpc_.call("files.delete", {{"id", id}}, session(request).owner);
            out["id"] = id;
            result(response, out);
        });
    server_->Post(
        "/api/upload/video",
        [this](const auto &request, auto &response, const httplib::ContentReader &reader) {
            standardUpload(request, response, reader);
        });
    server_->Post(
        "/api/device/configPackage/upload",
        [this](const auto &request, auto &response, const httplib::ContentReader &reader) {
            standardUpload(request, response, reader, "resource_package");
        });
}

/**
 * @brief 分块接收标准 multipart 视频或资源包，不把整个文件读入内存。
 * @param request 当前 HTTP 请求，身份由会话校验取得。
 * @param response 待写入的 HTTP 响应。
 * @param reader 流式请求体读取器。
 * @param kind 资源或任务类型，用于选择目录、配额和校验规则。
 */
void WebServer::standardUpload(const httplib::Request &request,
                               httplib::Response &response,
                               const httplib::ContentReader &reader,
                               const std::string &kind)
{
#ifdef _WIN32
    (void)request;
    (void)response;
    (void)reader;
    (void)kind;
    throw ApiError(503, "实际文件流上传需要Linux设备");
#else
    const auto user = session(request);
    if (!request.is_multipart_form_data())
        throw ApiError(400, "请使用multipart上传视频");

    // 文件描述符和接收租约采用作用域清理，异常路径也不会遗留占用。
    struct File {
        int fd = -1;

        /** @brief 在异常或提前返回时关闭尚未释放的上传文件描述符。 */
        ~File()
        {
            if (fd >= 0)
                ::close(fd);
        }
    } file;

    struct ReceivingLease {
        WebServer *server;
        std::string id;

        /** @brief 退出接收流程时移除上传租约，避免任务永久显示为传输中。 */
        ~ReceivingLease()
        {
            if (id.empty())
                return;
            std::lock_guard<std::mutex> lock(server->uploadsMutex_);
            server->uploads_.erase(id);
        }
    } lease{this, {}};

    std::string id, field, partPath;
    Json fields = Json::object();
    uint64_t received = 0, expected = 0;
    uint64_t checkpoint = 0;
    auto lastProgress = std::chrono::steady_clock::now();
    bool sawFile = false;

    // 客户端 size 可用于预留预算，最终发布仍以实测接收长度为准。
    const auto declared = [](const std::string &size) {
        try {
            size_t used = 0;
            const auto n = std::stoull(size, &used);
            if (used != size.size() || !n || size[0] == '-')
                throw std::invalid_argument("size");
            return n;
        } catch (...) {
            throw ApiError(400, "视频大小无效");
        }
    };
    try {

        // multipart 头负责创建原上传任务，数据回调只分块写盘，不缓存整段视频。
        const bool read = reader(
            [&](const httplib::MultipartFormData &part) {
                field = part.name;
                if (field == "file") {
                    if (sawFile)
                        throw ApiError(400, "一次只上传一个视频");
                    sawFile = true;
                    if (fields.contains("size"))
                        expected = declared(fields.at("size"));
                    else if (request.has_header("Content-Length"))
                        expected = request.get_header_value_u64("Content-Length");
                    else
                        throw ApiError(400, "请提供Content-Length或在file之前提供size");
                    auto task =
                        rpc_.call("uploads.create",
                                  {{"kind", kind}, {"name", part.filename}, {"size", expected}},
                                  user.owner);
                    id = task.at("id");
                    {
                        std::lock_guard<std::mutex> lock(uploadsMutex_);
                        if (!uploads_.insert(id).second)
                            throw ApiError(409, "任务正在接收");
                        lease.id = id;
                    }
                    auto started = rpc_.call("uploads.begin", {{"id", id}}, user.owner);
                    partPath = started.at("part_path");
                    file.fd = ::open(partPath.c_str(),
                                     O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                                     0660);
                    if (file.fd < 0)
                        throw ApiError(503, "无法创建视频暂存文件");
                    if (::flock(file.fd, LOCK_EX | LOCK_NB) != 0)
                        throw ApiError(409, "视频暂存文件正在使用");
                } else {
                    if ((field != "size" && field != "name" && field != "duration") ||
                        fields.contains(field))
                        throw ApiError(400, "上传字段无效或重复");
                    fields[field] = "";
                }
                return true;
            },
            [&](const char *data, size_t n) {
                if (field != "file") {
                    auto &value = fields[field].get_ref<std::string &>();
                    if (value.size() + n > 1024)
                        return false;
                    value.append(data, n);
                    return true;
                }
                if (file.fd < 0 || received + n > expected)
                    return false;
                size_t offset = 0;
                while (offset < n) {
                    const auto written = ::write(file.fd, data + offset, n - offset);
                    if (written < 0 && errno == EINTR)
                        continue;
                    if (written <= 0)
                        return false;
                    offset += size_t(written);
                }
                received += n;
                // 与原始字节上传共用 Qt 任务进度，兼容标准 multipart 上传入口。
                const auto now = std::chrono::steady_clock::now();
                if (received - checkpoint >= 16 * 1024 * 1024 ||
                    now - lastProgress >= std::chrono::seconds(1)) {
                    rpc_.call("uploads.progress", {{"id", id}}, user.owner);
                    checkpoint = received;
                    lastProgress = now;
                }
                return true;
            });
        if (!read || !sawFile || !received ||
            (fields.contains("size") && declared(fields.at("size")) != received))
            throw ApiError(400, "视频传输长度不完整");
        if (::fsync(file.fd) != 0)
            throw ApiError(503, "视频数据同步失败");
        ::close(file.fd);
        file.fd = -1;

        // 同步文件后按实测字节更新任务，再复用原接收、内容校验和原子发布。
        rpc_.call(kind == "resource_package" ? "local.resource.received" : "local.media.received",
                  {{"id", id}, {"size", received}},
                  user.owner);
        rpc_.call("uploads.received", {{"id", id}}, user.owner);
        auto published = rpc_.call("uploads.commit", {{"id", id}}, user.owner);
        if (kind == "resource_package") {
            result(response, published);
            return; // 通过 /api/v1/uploads/<id> 查询解压及目录替换结果。
        }
        const auto files = rpc_.call("files.list", Json::object(), user.owner).at("files");
        for (const auto &item : files)
            if (item.value("name", std::string()) ==
                published.value("original_name", std::string())) {
                result(response, item);
                return;
            }
        throw ApiError(503, "视频已发布，请刷新列表核对文件");
    } catch (...) {
        if (!id.empty())
            try {
                rpc_.call("uploads.failed", {{"id", id}}, user.owner);
            } catch (...) {
            }
        throw;
    }
#endif
}
} // namespace deviceweb
