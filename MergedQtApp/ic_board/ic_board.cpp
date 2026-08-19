/**
 * @file ic_board.cpp
 * @brief 启动 IC 设备链路工作线程，并桥接主线程 UI 提示。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "ic_board.h"

#include "ic_event_bridge.h"
#include "ic_toast.h"
#include "ic_worker.h"

#include <QMetaObject>
#include <QThread>
#include <QWidget>

namespace {

/**
 * @brief 在 IC worker 所属线程中停止 worker，然后退出线程。
 *
 * @param thread 持有 IC IO 对象的工作线程。
 * @param worker 已移动到工作线程的 IC 链路 worker 对象。
 *
 * @note 该函数在 UI 线程析构流程中触发，不能直接操作串口对象；
 *       串口对象必须在 worker 所属线程内停止。
 */
void stopIcWorkerThread(QThread *thread, IcWorker *worker)
{
    if (!thread) return;

    if (worker && thread->isRunning()) {
        QMetaObject::invokeMethod(worker, "stop", Qt::BlockingQueuedConnection);
    }

    thread->quit();
    thread->wait(3000);
}

} // namespace

/**
 * @brief 在独立 worker 线程中启动全部 IC IO 设备。
 *
 * @param parent 主窗口对象，用于生命周期托管和 UI toast 父对象。
 * @param io 串口设备和数据库路径配置。
 *
 * @note UI 操作仍留在调用方线程；IC 链路的串口、协议和数据库访问
 *       均在 IcWorkerThread 中执行。
 */
void IcIoBootstrap::startAll(QObject* parent, const IcSerialConfig& io)
{
    if (!parent) return;

    auto toast = IcToast::instance(qobject_cast<QWidget*>(parent));

    QObject::connect(IcEventBridge::instance(), &IcEventBridge::toastPassRequested,
                     parent, [toast]() {
        toast->showPass();
    }, static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection));

    QObject::connect(IcEventBridge::instance(), &IcEventBridge::toastFailRequested,
                     parent, [toast](const QString &reason) {
        toast->showFail(reason);
    }, static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection));

    auto *thread = new QThread(parent);
    thread->setObjectName(QStringLiteral("IcWorkerThread"));

    auto *worker = new IcWorker(io);
    worker->moveToThread(thread);

    QObject::connect(thread, &QThread::started,
                     worker, &IcWorker::start, Qt::QueuedConnection);
    QObject::connect(thread, &QThread::finished,
                     worker, &QObject::deleteLater);
    QObject::connect(thread, &QThread::finished,
                     thread, &QObject::deleteLater);

    QObject::connect(parent, &QObject::destroyed,
                     thread, [thread, worker]() {
        stopIcWorkerThread(thread, worker);
    });

    thread->start();
}
