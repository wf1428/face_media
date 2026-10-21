# @file CMakeLists.txt
# @brief 独立 C++17 Web daemon 构建；不查找、不链接 Qt 或 SQLite。
# @author Du lin
# @date 2026-10-09

# CMake 3.18 的 FindOpenSSL 支持 OpenSSL 3，避免旧板端 CMake 无法提取其版本。
cmake_minimum_required(VERSION 3.18)

project(device_webd VERSION 1.0 LANGUAGES CXX)

option(DEVICEWEB_TLS "Build HTTPS with OpenSSL 3" ON)
option(DEVICEWEB_TESTS "Build focused Web service tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_library(deviceweb_core src/rpc_client.cpp src/session_store.cpp src/web_server.cpp src/standard_api.cpp)
target_include_directories(deviceweb_core PUBLIC include third_party)
target_link_libraries(deviceweb_core PUBLIC Threads::Threads)

if(DEVICEWEB_TLS)
    find_package(OpenSSL 3.0 REQUIRED)
    target_compile_definitions(deviceweb_core PUBLIC DEVICEWEB_TLS CPPHTTPLIB_OPENSSL_SUPPORT)
    target_link_libraries(deviceweb_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

if(WIN32)
    target_link_libraries(deviceweb_core PUBLIC ws2_32 crypt32)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(deviceweb_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(device-webd src/main.cpp)
target_link_libraries(device-webd PRIVATE deviceweb_core)

install(TARGETS device-webd RUNTIME DESTINATION bin)

if(DEVICEWEB_TESTS)
    enable_testing()
    add_executable(deviceweb_tests tests/session_test.cpp)
    target_link_libraries(deviceweb_tests PRIVATE deviceweb_core)
    add_test(NAME deviceweb_unit COMMAND deviceweb_tests)
endif()
