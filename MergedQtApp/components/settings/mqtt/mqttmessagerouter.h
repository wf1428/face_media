/**
 * @file mqttmessagerouter.h
 * @brief 按 method 分派 MQTT 业务消息，并支持拦截器和兜底处理器。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

/** @brief MQTT 路由回调签名；返回值表示当前回调是否已处理该消息。 */
using MqttRouteCallback = std::function<bool(const QString &topic,
                                             const QJsonObject &payload,
                                             const QString &method)>;

/** @brief 可按一组 method 处理 MQTT 业务消息的接口。 */
class IMqttMessageHandler : public QObject
{
public:
    /** @brief 创建处理器并交由 Qt 父对象管理。 */
    explicit IMqttMessageHandler(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    /** @brief 允许通过接口指针安全销毁。 */
    virtual ~IMqttMessageHandler() = default;

    /** @return 用于诊断和冲突报告的处理器分组名。 */
    virtual QString group() const = 0;

    /** @return 本处理器声明支持的方法列表。 */
    virtual QStringList methods() const = 0;

    /** @return 已处理消息时返回 true。 */
    virtual bool handle(const QString &topic,
                        const QJsonObject &payload,
                        const QString &method) = 0;
};

/** @brief 用 method 到 std::function 映射实现的通用消息处理器。 */
class MqttCallbackMessageHandler final : public IMqttMessageHandler
{
public:
    /** @brief 创建指定分组的回调处理器。 */
    explicit MqttCallbackMessageHandler(const QString &group,
                                        QObject *parent = nullptr);

    /** @brief 注册一个规范化 method 的回调，拒绝空值或重复项。 */
    bool addMethod(const QString &method, const MqttRouteCallback &callback);

    /** @return 回调处理器分组名。 */
    QString group() const override;
    /** @return 已注册回调的方法列表。 */
    QStringList methods() const override;
    /** @return 对应方法的回调处理成功时返回 true。 */
    bool handle(const QString &topic,
                const QJsonObject &payload,
                const QString &method) override;

private:
    QString group_;
    QHash<QString, MqttRouteCallback> callbacks_;
};

/**
 * @brief MQTT method 路由、拦截器和兜底处理器注册表。
 *
 * 分派顺序为拦截器、精确 method 处理器、兜底处理器；QPointer 自动跳过已销毁 owner。
 */
class MqttMessageRouter : public QObject
{
public:
    /** @brief 路由器对外使用的消息处理回调类型。 */
    using Handler = MqttRouteCallback;

    /** @brief 创建空路由表。 */
    explicit MqttMessageRouter(QObject *parent = nullptr);

    /** @brief 为处理器声明的每个 method 注册精确路由。 */
    bool registerHandler(IMqttMessageHandler *handler);

    /** @brief 注册在精确路由之前执行的分组拦截器。 */
    bool registerInterceptor(const QString &group,
                             QObject *owner,
                             const Handler &handler);

    /** @brief 注册精确路由未处理后执行的分组兜底回调。 */
    bool registerFallbackHandler(const QString &group,
                                 QObject *owner,
                                 const Handler &handler);

    /** @return 任一路由处理消息时返回 true。 */
    bool dispatch(const QString &topic,
                  const QJsonObject &payload,
                  const QString &method) const;

    /** @return 存在指定 method 的精确处理器时返回 true。 */
    bool contains(const QString &method) const;

    /** @return 指定 method 的处理器分组名。 */
    QString handlerGroup(const QString &method) const;

private:
    /** @brief 一个带所有者生命期保护的回调路由。 */
    struct Route
    {
        QString group;          /**< 用于冲突和诊断的路由分组。 */
        QPointer<QObject> owner; /**< 回调所有者销毁后自动失效。 */
        Handler handler;        /**< 实际业务回调。 */
    };

    /** @return owner 有效且回调处理成功时返回 true。 */
    static bool invoke(const Route &route,
                       const QString &topic,
                       const QJsonObject &payload,
                       const QString &method);

    QHash<QString, QPointer<IMqttMessageHandler>> methodRoutes_;
    QVector<Route> interceptors_;
    QVector<Route> fallbackRoutes_;
};
