/**
 * @file ic_worker.h
 * @brief IC 链路工作线程对象。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef IC_WORKER_H
#define IC_WORKER_H

#include <QObject>
#include "ic_board.h"

class Rs485DriverPort;
class CardSerial2Reader;
class QrSerialReceiver;
class OfflineQr;
class NetworkAccessService;

/**
 * @brief IC 链路工作线程对象。
 *
 * 在所属非 UI 线程内创建并管理 RS485、刷卡器和二维码串口，根据运行时协议模式
 * 执行离线或在线兼容判定，并通过 IcEventBridge 与主线程交互。
 */
class IcWorker : public QObject
{
    Q_OBJECT
public:
    /** @brief 保存 I/O 配置；硬件对象在 start() 所在线程中延迟创建。 */
    explicit IcWorker(const IcSerialConfig &io, QObject *parent = nullptr);

    /** @brief 销毁前关闭串口和当前线程数据库连接。 */
    ~IcWorker() override;

public slots:
    /** @brief 初始化数据库配置并启动 RS485、刷卡器和二维码链路。 */
    void start();

    /** @brief 关闭全部串口及数据库；必须在 worker 所属线程执行。 */
    void stop();

private:
    NetworkAccessService *networkAccess_ = nullptr;
    IcSerialConfig io_;              /**< 数据库路径和硬件串口参数。 */
    bool started_ = false;           /**< 防止重复创建硬件对象。 */

    Rs485DriverPort *rs485_ = nullptr;       /**< RS485 收发对象，由 this 托管。 */
    CardSerial2Reader *reader_ = nullptr;    /**< 刷卡器收发及拆帧对象。 */
    QrSerialReceiver *qrReceiver_ = nullptr; /**< 二维码串口报文接收器。 */
    OfflineQr *qr_ = nullptr;                /**< 离线二维码业务解析器。 */
};

#endif // IC_WORKER_H
