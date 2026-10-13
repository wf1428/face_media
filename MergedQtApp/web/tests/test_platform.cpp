/**
 * @file test_platform.cpp
 * @brief 测试目标专用平台路径替身；所有旧仓储读写都重定向到 QTemporaryDir。
 * @author Du lin
 * @date 2026-10-09
 */
#include "platform/rk3566_platform.h"
#include <QDir>
namespace Rk3566Platform {
/** @return 测试进程初始化的临时根，不使用生产默认路径。 */
QString applicationRoot() { return QString::fromLocal8Bit(qgetenv("WEB_TEST_ROOT")); }
/** @return 临时统一 SQLite。 */
QString databasePath() { return QDir(applicationRoot()).filePath("access_control.db"); }
/** @return 临时静态文件根。 */
QString staticRoot() { return QDir(applicationRoot()).filePath("static"); }
}
