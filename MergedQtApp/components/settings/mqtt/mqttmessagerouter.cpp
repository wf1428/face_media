/**
 * @file mqttmessagerouter.cpp
 * @brief 按 method 分派 MQTT 业务消息，并支持拦截器和兜底处理器。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "mqttmessagerouter.h"

/** @brief 创建指定分组的回调处理器。 */
MqttCallbackMessageHandler::MqttCallbackMessageHandler(const QString &group,
                                                       QObject *parent)
    : IMqttMessageHandler(parent),
      group_(group.trimmed())
{
}

/** @brief 注册一个规范化 method 的回调，拒绝空值或重复项。 */
bool MqttCallbackMessageHandler::addMethod(const QString &method,
                                           const MqttRouteCallback &callback)
{
    const QString normalizedMethod = method.trimmed();
    if (normalizedMethod.isEmpty() || !callback
            || callbacks_.contains(normalizedMethod)) {
        return false;
    }

    callbacks_.insert(normalizedMethod, callback);
    return true;
}

/** @return 用于诊断和冲突报告的处理器分组名。 */
QString MqttCallbackMessageHandler::group() const
{
    return group_;
}

/** @return 本处理器声明支持的方法列表。 */
QStringList MqttCallbackMessageHandler::methods() const
{
    return callbacks_.keys();
}

/** @return 对应方法的回调处理成功时返回 true。 */
bool MqttCallbackMessageHandler::handle(const QString &topic,
                                        const QJsonObject &payload,
                                        const QString &method)
{
    const auto callbackIt = callbacks_.constFind(method.trimmed());
    if (callbackIt == callbacks_.constEnd() || !callbackIt.value()) {
        return false;
    }

    return callbackIt.value()(topic, payload, method);
}

/** @brief 创建空路由表。 */
MqttMessageRouter::MqttMessageRouter(QObject *parent)
    : QObject(parent)
{
}

/** @brief 为处理器声明的每个 method 注册精确路由。 */
bool MqttMessageRouter::registerHandler(IMqttMessageHandler *handler)
{
    if (!handler) {
        return false;
    }

    const QStringList declaredMethods = handler->methods();
    if (declaredMethods.isEmpty()) {
        return false;
    }

    QStringList normalizedMethods;
    QSet<QString> uniqueMethods;
    for (const QString &method : declaredMethods) {
        const QString normalizedMethod = method.trimmed();
        if (normalizedMethod.isEmpty()
                || uniqueMethods.contains(normalizedMethod)
                || methodRoutes_.contains(normalizedMethod)) {
            return false;
        }
        uniqueMethods.insert(normalizedMethod);
        normalizedMethods.append(normalizedMethod);
    }

    for (const QString &method : normalizedMethods) {
        methodRoutes_.insert(method, QPointer<IMqttMessageHandler>(handler));
    }
    return true;
}

/** @brief 注册在精确路由之前执行的分组拦截器。 */
bool MqttMessageRouter::registerInterceptor(const QString &group,
                                            QObject *owner,
                                            const Handler &handler)
{
    if (!owner || !handler) {
        return false;
    }

    Route route;
    route.group = group.trimmed();
    route.owner = owner;
    route.handler = handler;
    interceptors_.append(route);
    return true;
}

/** @brief 注册精确路由未处理后执行的分组兜底回调。 */
bool MqttMessageRouter::registerFallbackHandler(const QString &group,
                                                QObject *owner,
                                                const Handler &handler)
{
    if (!owner || !handler) {
        return false;
    }

    Route route;
    route.group = group.trimmed();
    route.owner = owner;
    route.handler = handler;
    fallbackRoutes_.append(route);
    return true;
}

/** @return 任一路由处理消息时返回 true。 */
bool MqttMessageRouter::dispatch(const QString &topic,
                                 const QJsonObject &payload,
                                 const QString &method) const
{
    for (const Route &route : interceptors_) {
        if (invoke(route, topic, payload, method)) {
            return true;
        }
    }

    const auto routeIt = methodRoutes_.constFind(method.trimmed());
    if (routeIt != methodRoutes_.constEnd()) {
        IMqttMessageHandler *handler = routeIt.value().data();
        if (handler && handler->handle(topic, payload, method)) {
            return true;
        }
    }

    for (const Route &route : fallbackRoutes_) {
        if (invoke(route, topic, payload, method)) {
            return true;
        }
    }

    return false;
}

/** @return 存在指定 method 的精确处理器时返回 true。 */
bool MqttMessageRouter::contains(const QString &method) const
{
    return methodRoutes_.contains(method.trimmed());
}

/** @return 指定 method 的处理器分组名。 */
QString MqttMessageRouter::handlerGroup(const QString &method) const
{
    const auto routeIt = methodRoutes_.constFind(method.trimmed());
    if (routeIt == methodRoutes_.constEnd() || routeIt.value().isNull()) {
        return QString();
    }
    return routeIt.value()->group();
}

/** @return owner 有效且回调处理成功时返回 true。 */
bool MqttMessageRouter::invoke(const Route &route,
                               const QString &topic,
                               const QJsonObject &payload,
                               const QString &method)
{
    if (route.owner.isNull() || !route.handler) {
        return false;
    }

    return route.handler(topic, payload, method);
}
