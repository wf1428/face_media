/**
 * @file signalboard_client.cpp
 * @brief 信号板工作线程的生命周期封装的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "signalboard_client.h"

/** @brief 创建并启动工作线程；已经启动时不重复创建。 */
void SignalBoardClient::start()
{
    if (thread) return;

    qRegisterMetaType<SignalBoardState>("SignalBoardState");

    // 创建线程和 worker
    thread = new QThread();
    worker = new SignalBoardWorker(portName, baud);
    worker->moveToThread(thread);

    connect(thread, &QThread::started, worker, &SignalBoardWorker::start);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);

    // 把 worker 的数据转发出来（跨线程自动 queued）
    connect(worker, &SignalBoardWorker::stateReceived, this, &SignalBoardClient::stateReceived);
    connect(worker, &SignalBoardWorker::onlineChanged, this, &SignalBoardClient::onlineChanged);

    thread->start();
}

/** @brief 请求 worker 停止，等待线程退出后释放线程对象。 */
void SignalBoardClient::stop()
{
    if (!thread) return;
    if (worker) QMetaObject::invokeMethod(worker, "stop", Qt::QueuedConnection);
    thread->quit();
    thread->wait();
    delete thread;
    thread = nullptr;
    worker = nullptr;
}
