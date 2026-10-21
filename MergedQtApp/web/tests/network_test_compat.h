/**
 * @file network_test_compat.h
 * @brief 仅供 Windows 隔离测试编译 Linux 网络页，禁止实际发送进程信号。
 * @author Du lin
 * @date 2026-10-20
 */

#pragma once
#ifdef _WIN32
#include <cerrno>
#include <sys/types.h>
#ifndef SIGKILL
#define SIGKILL 9
#endif

/** @brief Windows 不执行板端 DHCP 进程操作，始终报告目标进程不存在。 */
inline int kill(pid_t, int)
{
    errno = ESRCH;
    return -1;
}
#endif
