/**
 * @file main.cpp
 * @brief 独立非 Qt device-webd 入口；配置、信号与服务生命周期。
 * @author Du lin
 * @date 2026-10-09
 */

#include "web_server.h"
#include "console_log.h"
#include <thread>
#include <atomic>
#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#endif

/** @brief 加载部署配置并运行独立 Web 服务。 */
int main(int argc, char **argv)
{
    if (argc != 2) {
        deviceweb::consoleLog(deviceweb::LogLevel::Error, "usage: device-webd /path/to/web.json");
        return 2;
    }
    try {
        deviceweb::consoleLog(deviceweb::LogLevel::Info,
                              std::string("device-webd starting; config=") + argv[1]);
#ifndef _WIN32
        // 用 sigwait 在线程里安全调用 stop，避免异步信号处理器执行非安全库函数。
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        sigaddset(&signals, SIGUSR1);
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);
#endif
        deviceweb::WebServer server(deviceweb::ServerConfig::load(argv[1]));
        std::atomic_bool finished{false};
#ifndef _WIN32
        std::thread stopper([&]() {
            int signal = 0;
            sigwait(&signals, &signal);
            if (!finished.load()) {
                deviceweb::consoleLog(deviceweb::LogLevel::Info,
                                      "Shutdown requested; signal=" + std::to_string(signal));
                server.stop();
            }
        });
#endif
        bool ok = false;
        try {
            ok = server.run();
        } catch (...) {
            finished.store(true);
#ifndef _WIN32
            pthread_kill(stopper.native_handle(), SIGUSR1);
            stopper.join();
#endif
            throw;
        }
        finished.store(true);
#ifndef _WIN32
        pthread_kill(stopper.native_handle(), SIGUSR1);
        stopper.join();
#endif
        deviceweb::consoleLog(ok ? deviceweb::LogLevel::Info : deviceweb::LogLevel::Error,
                              ok ? "device-webd stopped"
                                 : "device-webd stopped after listener failure");
        return ok ? 0 : 1;
    } catch (const std::exception &e) {
        deviceweb::consoleLog(deviceweb::LogLevel::Error,
                              std::string("device-webd startup/run failed: ") + e.what());
        return 1;
    }
}
