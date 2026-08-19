/**
 * @file signalboard_client.h
 * @brief 信号板工作线程的生命周期封装。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef SIGNALBOARD_CLIENT_H
#define SIGNALBOARD_CLIENT_H

#include <QObject>
#include <QThread>
#include <utility>
#include "common/protocols/signalboard_decoder.h"
#include "common/workers/signalboard_worker.h"

/**
 * @brief 信号板工作线程的生命周期封装。
 *
 * 在独立 QThread 中运行 SignalBoardWorker，并把工作线程产生的状态、在线变化信号
 * 转发给当前对象的使用者。
 */
class SignalBoardClient : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief 保存串口参数并创建尚未启动的客户端。
     * @param portName 信号板串口设备名称。
     * @param baud 串口波特率，单位 bit/s。
     * @param parent Qt 父对象。
     */
    explicit SignalBoardClient(QString portName, int baud, QObject *parent=nullptr)
        : QObject(parent), portName(std::move(portName)), baud(baud) {}

    /** @brief 销毁客户端前停止并回收工作线程。 */
    ~SignalBoardClient() override { stop(); }

public slots:
    /** @brief 创建并启动工作线程；已经启动时不重复创建。 */
    void start();

    /** @brief 请求 worker 停止，等待线程退出后释放线程对象。 */
    void stop();

signals:
    /** @brief 转发一帧信号板数据解析得到的状态。 */
    void stateReceived(SignalBoardState st);

    /** @brief 转发信号板在线状态及状态变化原因。 */
    void onlineChanged(bool online, QString reason);

private:
    QString portName;                  /**< 信号板串口设备名称。 */
    int baud = 9600;                   /**< 串口波特率，单位 bit/s。 */
    QThread *thread = nullptr;         /**< worker 所在线程，stop() 后置空。 */
    SignalBoardWorker *worker = nullptr; /**< 由线程结束信号触发延迟销毁。 */
};


#endif // SIGNALBOARD_CLIENT_H
