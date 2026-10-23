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

    /**
     * @brief 取得应用生命周期内共享的桥对象。
     * @return 主线程初始化后的桥实例。
     */
    static WebFaceBridge *instance();

signals:

    /**
     * @brief 工作线程请求已有推理 worker 执行新增 Web 防重校验。
     * @param token 贯穿请求与异步结果的任务标识。
     * @param faces 待校验的人脸条目，含人员及资源信息。
     */
    void validationRequested(const QString &token, const QJsonArray &faces);

    /**
     * @brief 将质量和跨库重复校验结果发送回 Web 工作线程。
     * @param token 贯穿请求与异步结果的任务标识。
     * @param accepted 通过质量、防重校验的人脸条目。
     * @param failed 校验失败的人脸条目及原因。
     */
    void
    validationFinished(const QString &token, const QJsonArray &accepted, const QJsonArray &failed);

    /**
     * @brief 人员/人脸实际写入后请求原数据库 worker 刷新图库及管理清单。
     */
    void personnelChanged();

private:

    /**
     * @brief 桥归属应用主线程，单例随应用退出销毁。
     * @param parent Qt 父对象；对象销毁由父子关系管理，空指针表示无父对象。
     */
    explicit WebFaceBridge(QObject *parent) : QObject(parent)
    {
    }
};
