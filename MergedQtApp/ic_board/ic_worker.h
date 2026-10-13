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
#include <QElapsedTimer>
#include "ic_board.h"

class Rs485DriverPort;
class CardSerial2Reader;
class QrSerialReceiver;
class OfflineQr;
class NetworkAccessService;
class LocalAccessService;
class QTimer;
struct NetworkAccessResult;

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
    /** @return 卡号在任一人员库中存在并已完成对应处理时返回 true。 */
    bool handleStoredCard(const QString &cardId, const QByteArray &rawFrame);
    /** @brief 完成网络人员刷卡结果的 RS485、扣次和事件上报。 */
    void finishNetworkCard(const NetworkAccessResult &result, const QByteArray &rawFrame);
    /** @brief 完成本地人员刷卡结果的 RS485 和扣次。 */
    void finishLocalCard(const NetworkAccessResult &result);
    /** @brief 两个人员库均无卡号时使用 103/115 字节卡内权限。 */
    void finishEmbeddedPermissionCard(const QByteArray &frame);

    NetworkAccessService *networkAccess_ = nullptr;
    LocalAccessService *localAccess_ = nullptr;
    IcSerialConfig io_;              /**< 数据库路径和硬件串口参数。 */
    bool started_ = false;           /**< 防止重复创建硬件对象。 */

    Rs485DriverPort *rs485_ = nullptr;       /**< RS485 收发对象，由 this 托管。 */
    CardSerial2Reader *reader_ = nullptr;    /**< 刷卡器收发及拆帧对象。 */
    QrSerialReceiver *qrReceiver_ = nullptr; /**< 二维码串口报文接收器。 */
    OfflineQr *qr_ = nullptr;                /**< 离线二维码业务解析器。 */
    QTimer *readerHeartbeatTimer_ = nullptr; /**< 每秒检查读头心跳，故障提示限频为30秒。 */
    QElapsedTimer readerHeartbeatElapsed_;   /**< 最近一次收到 &B 帧后的单调计时。 */
    QElapsedTimer readerHeartbeatWarningElapsed_; /**< 最近一次故障提示后的单调计时。 */
    bool readerHeartbeatFault_ = false;      /**< 是否已进入一分钟无心跳故障状态。 */
};

#endif // IC_WORKER_H
