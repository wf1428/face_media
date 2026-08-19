/**
 * @file ic_mqtt_gateway.h
 * @brief MQTT 在线协议与本地 IC/二维码/RS485 事件之间的双向网关。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef IC_MQTT_GATEWAY_H
#define IC_MQTT_GATEWAY_H

#include <QObject>
#include <QJsonObject>
#include <QTimer>

#include "components/settings/mqtt/mqttmanager.h"
#include "ic_board/ic_offline.h"
#include "ic_board/rs485_driver_port.h"
#include "ic_board/serial_init.h"
#include "ic_board/device_config_sync.h"
#include "common/sql/dbstore.h"

/**
 * @brief MQTT 在线协议与本地 IC/二维码/RS485 事件之间的双向网关。
 *
 * 将硬件事件组装为 MQTT packet，并把服务端开楼层、时间、继电器等命令转换为
 * 本地事件桥请求。在线二维码响应使用 500 ms 超时防止请求永久悬挂。
 */
class IcMqttGateway : public QObject
{
    Q_OBJECT
public:
    /** @brief 创建保活、旧卡片兜底和在线二维码响应定时器。 */
    explicit IcMqttGateway(QObject *parent = nullptr);

    /** @brief 替换 MQTT/硬件配置并按协议模式启停网关。 */
    void applyConfig(const MqttConfig &cfg);

    /** @return 当前消息被 IC 网关识别并处理时返回 true。 */
    bool handleIncoming(const QString &topic, const QJsonObject &payloadObj);

    /** @return 当前配置启用了在线 IC 网关时返回 true。 */
    bool isEnabled() const;

signals:
    /** @brief 请求管理器通过 mqttd 发布已封装 packet。 */
    void publishPacket(const QJsonObject &packet, const QString &tag);
    /** @brief 输出硬件网关处理日志。 */
    void logMessage(const QString &msg);

private slots:
    /** @brief 刷卡通过后按在线或兼容协议上报事件。 */
    void onCardPassed(const QString &cardId, const QString &floor, const QByteArray &rawFrame);
    /** @brief 扫描在线二维码后上报并启动服务端响应超时。 */
    void onOnlineQrScanned(const QString &qrCode);
    /** @brief 解析 RS485 楼层帧并补全等待中的旧协议刷卡事件。 */
    void onRs485FrameReceived(const QByteArray &frame);
    /** @brief 周期发布在线保活事件。 */
    void onKeepAliveTimeout();

private:
    /** @brief 兼容不同服务端 envelope 后的统一消息结构。 */
    struct NormalizedMsg {
        QString method;
        QString id;
        QString time;
        QJsonObject data;
        QJsonObject raw;
    };

    /** @brief 仅一次连接 IcEventBridge 的硬件事件与回执信号。 */
    void ensureBridgeConnected();

    /** @brief 生成协议要求的当前时间字符串。 */
    QString currentTimeString() const;
    /** @brief 生成当前进程内递增且可追踪的消息 ID。 */
    QString nextMessageId() const;
    /** @brief 从事件桥缓存取得当前楼层。 */
    /** @brief 从刷卡原始帧提取卡号文本。 */
    /** @brief 兼容扁平和嵌套 envelope，归一化服务端消息。 */
    NormalizedMsg normalize(const QJsonObject &obj) const;
    /** @brief 把业务 payload 封装为 mqttd publish packet。 */
    QJsonObject buildPublishPacket(const QJsonObject &payload) const;
    /** @brief 创建含公共设备、时间和 method 字段的 payload。 */
    QJsonObject buildBasePayload(const QString &methon) const;

    /** @brief 发布无请求上下文的硬件事件。 */
    void publishEvent(const QString &methon, const QJsonObject &extra, const QString &tag);
    /** @brief 对指定请求 ID 发布成功响应。 */
    void publishSuccess(const QString &methon, const QString &reqId, const QJsonObject &extra, const QString &tag);
    /** @brief 发布交通梯当前楼层状态响应。 */
    void publishTrafficFloorState(const QString &reqId);

    /** @brief 解析服务端时间并同步系统与 RTC。 */
    bool setSystemTime(const QString &t, QString *err) const;
    /** @brief 通过事件桥执行开楼层请求。 */
    bool executeOpenFloor(const QString &floor, QString *err);
    /** @brief 将二维码或原始数据发送到楼层控制链路。 */
    bool executeSendFloorQr(const QString &rawData, const QString &sourceTag, QString *err);
    /** @brief 执行继电器控制命令。 */
    bool executeSetJdq(const QString &cmd, QString *err);

    /** @brief 从服务端 data 的兼容字段中查找卡号。 */
    QString findCardValue(const QJsonObject &data) const;
    /** @brief 用随后到达的楼层补全旧协议刷卡事件并发布。 */
    void publishPendingLegacyCardWithFloor(const QString &floor);
    /** @brief 楼层未及时到达时用当前缓存值发布旧协议刷卡事件。 */
    void publishPendingLegacyCardFallback();

    /** @brief 记录待响应二维码并启动 500 ms 超时。 */
    void armOnlineQrReplyTimeout(const QString &qrCode);
    /** @brief 收到匹配响应后清除二维码等待状态。 */
    void clearOnlineQrReplyTimeout();
    /** @brief 服务端未及时响应时结束等待并记录超时。 */
    void onOnlineQrReplyTimeout();


private:
    MqttConfig cfg_;
    bool bridgeConnected_ = false; /**< 防止重复连接全局事件桥信号。 */
    DeviceConfig deviceCfg_;

    Rs485DriverPort *rs485_ = nullptr;
    CardSerial2Reader *cardReader_ = nullptr;
    QrSerialReceiver *qrReceiver_ = nullptr;
    QTimer *keepAliveTimer_ = nullptr;
    QTimer *legacyCardFallbackTimer_ = nullptr;
    QString pendingLegacyCardId_; /**< 等待下一楼层帧补全的旧协议卡号。 */

    QTimer onlineQrReplyTimer_;
    bool onlineQrReplyPending_ = false; /**< 是否存在等待服务端响应的在线二维码。 */
    QString pendingOnlineQrCode_;       /**< 用于匹配和记录超时的二维码原文。 */
    int onlineQrReplyTimeoutMs_ = 500; /**< 在线二维码响应超时，单位 ms。 */
};

#endif // IC_MQTT_GATEWAY_H
