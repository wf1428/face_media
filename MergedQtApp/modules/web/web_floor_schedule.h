/** @file web_floor_schedule.h
 *  @brief Web独立定时梯控；兼容既有规则格式、存储和分钟去重，不依赖MQTT模块。
 */
#pragma once

#include <QObject>
#include <QDateTime>
#include <QJsonObject>
#include <functional>

/** @brief Web 独立规则执行器，读写共享规则及分钟标记，不依赖 MQTT 对象。 */
class WebFloorSchedule final : public QObject
{
public:
    /** @brief 发送完整分层板帧；参数依次为原始字节及来源标识，返回是否受理。 */
    using SendFrame = std::function<bool(const QByteArray &, const QString &)>;

    /**
     * @brief 仅在Web启用时创建，发送回调连接公共RS485出口。
     * @param parent Qt 父对象；对象销毁由父子关系管理，空指针表示无父对象。
     * @param sendFrame 公共 RS485 发送回调，返回是否受理本次发送。
     */
    explicit WebFloorSchedule(QObject *parent, SendFrame sendFrame);

    /**
     * @brief 校验、保存或删除共享规则，完成后返回Web任务结果。
     * @param action 规则操作：set、delete 或 resend。
     * @param data 本次操作的数据对象。
     * @param[out] status 接收本次操作的业务状态码。
     * @param[out] error 可选输出，接收失败说明。
     * @return 成功时返回已保存规则及可写状态；失败返回空对象，并设置 status 和 error。
     * @note 在对象所属线程调用；baseSchedules 不匹配当前快照时拒绝覆盖并返回 409。
     */
    QJsonObject apply(const QString &action, const QJsonObject &data,
                      int *status, QString *error);

    /**
     * @brief 按设备本地时间执行到期规则；与原执行器共用已执行分钟标记。
     * @param now 设备本地时间；默认取当前时间。
     * @note 每 5 秒检查设备本地分钟；先事务保存状态及共享分钟标记，再调用公共串口出口，避免两套入口重复执行。
     */
    void checkDue(const QDateTime &now = QDateTime::currentDateTime());

private:
    SendFrame sendFrame_; /**< 公共 RS485 出口，在对象所属线程调用。 */
};
