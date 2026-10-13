/**
 * @file rpc_client.cpp
 * @brief 使用限时 Unix socket 进行 Qt RPC；二进制数据不在 IPC 内传输。
 * @author Du lin
 * @date 2026-10-09
 */
#include "rpc_client.h"
#include <atomic>
#include <cstring>
#include <chrono>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#endif

namespace deviceweb {
Json RpcClient::call(const std::string &operation,const Json &args,int owner) const {
#ifdef _WIN32
    (void)operation; (void)args; (void)owner;
    throw ApiError(503,"Windows 构建仅供单元检查；生产 RPC 要求 Linux Unix socket");
#else
    static std::atomic<uint64_t> sequence{0};
    const std::string id=std::to_string(++sequence);
    std::string request=Json{{"version",1},{"id",id},{"operation",operation},{"args",args},{"owner",owner}}.dump()+'\n';
    constexpr size_t maxFrame=1024*1024;
    if (request.size()>maxFrame) throw ApiError(413,"IPC 请求超过长度限制");
    if (path_.size()>=sizeof(sockaddr_un::sun_path)) throw ApiError(503,"IPC 路径过长");
    struct Descriptor { int fd; ~Descriptor() { if (fd>=0) ::close(fd); } } socket{::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0)};
    if (socket.fd<0) throw ApiError(503,"无法连接设备业务服务");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(25);
    const auto ready=[&](short events) {
        while (true) {
            const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
            if (remaining<=0) throw ApiError(503,"设备业务服务响应超时，请查询任务状态");
            pollfd descriptor{socket.fd,events,0};
            const int result=::poll(&descriptor,1,static_cast<int>(remaining));
            if (result<0 && errno==EINTR) continue;
            if (result<=0 || descriptor.revents&(POLLERR|POLLNVAL)) throw ApiError(503,"设备业务服务暂不可用");
            if (descriptor.revents&events) return;
            if (descriptor.revents&POLLHUP) throw ApiError(503,"设备业务连接中断");
        }
    };
    sockaddr_un address{}; address.sun_family=AF_UNIX;
    std::memcpy(address.sun_path,path_.c_str(),path_.size()+1);
    if (::connect(socket.fd,reinterpret_cast<sockaddr *>(&address),sizeof(address))!=0) {
        if (errno!=EINPROGRESS) throw ApiError(503,"设备业务服务暂不可用");
        ready(POLLOUT); int error=0; socklen_t size=sizeof(error);
        if (::getsockopt(socket.fd,SOL_SOCKET,SO_ERROR,&error,&size)!=0 || error) throw ApiError(503,"设备业务服务连接失败");
    }
    size_t offset=0;
    while (offset<request.size()) {
        ready(POLLOUT);
        const auto sent=::send(socket.fd,request.data()+offset,request.size()-offset,MSG_NOSIGNAL);
        if (sent<0 && (errno==EAGAIN || errno==EINTR)) continue;
        if (sent<=0) throw ApiError(503,"设备业务请求发送失败");
        offset+=static_cast<size_t>(sent);
    }
    std::string response;
    while (response.find('\n')==std::string::npos) {
        ready(POLLIN); char buffer[8192];
        const auto received=::recv(socket.fd,buffer,sizeof(buffer),0);
        if (received<0 && (errno==EAGAIN || errno==EINTR)) continue;
        if (received<=0) throw ApiError(503,"设备业务响应中断");
        response.append(buffer,static_cast<size_t>(received));
        if (response.size()>maxFrame) throw ApiError(503,"设备业务响应过大");
    }
    Json result=Json::parse(response.substr(0,response.find('\n')),nullptr,false);
    if (!result.is_object() || result.value("version",0)!=1 || result.value("id",std::string{})!=id) throw ApiError(503,"设备业务响应格式错误");
    const int code=result.value("code",500);
    if (code!=200) throw ApiError(code,result.value("message",std::string("设备操作失败")));
    return result.at("data");
#endif
}
}
