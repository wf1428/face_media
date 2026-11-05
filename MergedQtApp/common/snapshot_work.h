/**
 * @file snapshot_work.h
 * @brief 普通和高清截图共用的单一后台串行队列。
 * @author Du lin
 * @date 2026-11-04
 * @note 最近修改：2026-11-04；修改人：Du lin；更新后台职责为视频合成/编码，已绘制UI像素不再回放文字命令，串行队列和退出屏障逻辑保持。
 */
#pragma once
#include "serial_work_queue.h"
#include <QCoreApplication>
namespace SnapshotWork {
/** @return UI所有的截图队列门面；所有run共用一个后台线程，不并发JPEG/NV12处理；UI已绘成像素，不再回放绘图命令。
 * @note 仅UI调用enqueue；prepare/done在UI，run只访问自有图像值。任务须reportSlow=false，不新增日志。
 * 最终应用退出由队列屏障结束在途任务并丢弃待执行项，普通截图/页面销毁不在UI等待。
 */
inline SerialWorkQueue &queue() {
    Q_ASSERT(QCoreApplication::instance() && QThread::currentThread()==QCoreApplication::instance()->thread());
    static auto *instance=[] {
        auto *queue=new SerialWorkQueue(QStringLiteral("media.snapshot"),QCoreApplication::instance());
        // Qt图像资源释放前完成在途视频合成/编码；只在最终应用退出同步收尾，不用于普通截图/页面关闭。
        QObject::connect(QCoreApplication::instance(),&QCoreApplication::aboutToQuit,queue,[queue]{queue->shutdown();},Qt::DirectConnection);
        return queue;
    }();
    return *instance;
}
}
