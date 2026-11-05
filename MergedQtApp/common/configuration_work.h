/**
 * @file configuration_work.h
 * @brief 媒体控制配置共享的后台串行队列，避免各写入入口相互覆盖。
 * @author Du lin
 * @date 2026-11-04
 */
#pragma once
#include "serial_work_queue.h"
#include <QCoreApplication>
namespace ConfigurationWork {
/** @return UI线程独占的队列门面；后台线程仅执行值快照的文件操作。
 * @note 只从应用主线程调用；队列由应用对象拥有，最终析构串行完成已排队的值写入并等待线程退出，跳过UI完成回调；普通操作不等待。
 */
inline SerialWorkQueue &queue() {
    Q_ASSERT(QCoreApplication::instance() && QThread::currentThread()==QCoreApplication::instance()->thread());
    static auto *instance=new SerialWorkQueue(QStringLiteral("media.config"),QCoreApplication::instance(),true);
    return *instance;
}
}
