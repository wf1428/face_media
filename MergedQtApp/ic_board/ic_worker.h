/**
 * @file ic_worker.h
 * @brief IC 链路工作线程对象。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-20；修改人：Du lin；将消防电平切换改为非阻塞定时状态机：
 *       每次已执行变化后冷却 1000 ms，解除消防前再确认低电平稳定 500 ms，避免连续下发两条 132 字节帧。
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
class FireGpioMonitor;
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
    /** @brief 初始化数据库配置，并启动 RS485、刷卡器、二维码及非阻塞消防定时状态机。 */
    void start();

    /** @brief 停止消防定时器、关闭全部串口及数据库；必须在 worker 所属线程执行。 */
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

    /**
     * @brief 记录消防 GPIO 原始状态并驱动非阻塞定时状态机。
     * @param[in] active true 表示采样为高电平，false 表示采样为低电平。
     * @note 本方法只在 IC 工作线程执行；等待过程使用 QTimer，不阻塞 UI 或 IC 事件循环。
     */
    void handleFireStateChanged(bool active);

    /**
     * @brief 根据当前原始电平、消防状态和定时器状态决定下一步动作。
     * @note 1000 ms 冷却期间只记录最新电平；消防中检测到低电平后启动 500 ms 退出确认。
     */
    void evaluateFireState();

    /** @brief 发送全开放帧、置位消防状态并启动 1000 ms 状态变化冷却。 */
    void enterFireState();

    /** @brief 低电平稳定 500 ms 后恢复数据库梯控配置并启动 1000 ms 冷却。 */
    void completeFireExit();

    NetworkAccessService *networkAccess_ = nullptr;
    LocalAccessService *localAccess_ = nullptr;
    IcSerialConfig io_;              /**< 数据库路径和硬件串口参数。 */
    bool started_ = false;           /**< 防止重复创建硬件对象。 */

    Rs485DriverPort *rs485_ = nullptr;       /**< RS485 收发对象，由 this 托管。 */
    CardSerial2Reader *reader_ = nullptr;    /**< 刷卡器收发及拆帧对象。 */
    QrSerialReceiver *qrReceiver_ = nullptr; /**< 二维码串口报文接收器。 */
    OfflineQr *qr_ = nullptr;                /**< 离线二维码业务解析器。 */
    QTimer *readerHeartbeatTimer_ = nullptr; /**< 每秒检查读头心跳，故障提示限频为30秒。 */
    FireGpioMonitor *fireMonitor_ = nullptr; /**< 单路消防 GPIO 监视器，由 this 托管并运行在 IC 工作线程。 */
    QTimer *fireTransitionCooldownTimer_ = nullptr; /**< 已执行消防变化后的1000 ms非阻塞冷却定时器。 */
    QTimer *fireExitConfirmTimer_ = nullptr; /**< 解除消防前确认低电平持续500 ms的单次定时器。 */
    bool fireRawStateKnown_ = false; /**< 是否已经取得GPIO有效样本；false时不执行状态转换。 */
    bool fireRawActive_ = false; /**< 最近GPIO原始电平；true为高电平，false为低电平。 */
    QElapsedTimer readerHeartbeatElapsed_;   /**< 最近一次收到 &B 帧后的单调计时。 */
    QElapsedTimer readerHeartbeatWarningElapsed_; /**< 最近一次故障提示后的单调计时。 */
    bool readerHeartbeatFault_ = false;      /**< 是否已进入一分钟无心跳故障状态。 */
};

#endif // IC_WORKER_H
