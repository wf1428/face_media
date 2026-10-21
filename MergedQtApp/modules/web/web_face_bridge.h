/**
 * @file web_face_bridge.h
 * @brief Web 本地人脸校验与现有推理线程之间的异步桥；不涉及 MQTT。
 * @author Du lin
 * @date 2026-10-09
 */

#pragma once

#include <QObject>
#include <QJsonArray>

/** @brief 在主线程创建的单例，只传递小型图片清单/特征，不传文件字节。 */
class WebFaceBridge final : public QObject {
    Q_OBJECT

public:

    /** @return 主线程初始化后的桥实例。 */
    static WebFaceBridge *instance();

signals:

    /** @brief 工作线程请求已有推理 worker 执行新增 Web 防重校验。 */
    void validationRequested(const QString &token, const QJsonArray &faces);

    /** @brief 将质量和跨库重复校验结果发送回 Web 工作线程。 */
    void
    validationFinished(const QString &token, const QJsonArray &accepted, const QJsonArray &failed);

    /** @brief 人员/人脸实际写入后请求原数据库 worker 刷新图库及管理清单。 */
    void personnelChanged();

private:

    /** @brief 桥归属应用主线程，单例随应用退出销毁。 */
    explicit WebFaceBridge(QObject *parent) : QObject(parent)
    {
    }
};
