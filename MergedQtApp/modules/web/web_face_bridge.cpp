/**
 * @file web_face_bridge.cpp
 * @brief Web 异步桥生命周期管理；首次创建必须在应用主线程。
 * @author Du lin
 * @date 2026-10-09
 */

#include "web_face_bridge.h"
#include <QCoreApplication>
#include <QThread>

/**
 * @brief 返回供 Web 人脸校验使用的桥接实例。
 * @return 应用生命周期内共享的桥对象指针。
 */
WebFaceBridge *WebFaceBridge::instance()
{
    static WebFaceBridge *bridge = []() {
        Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
        return new WebFaceBridge(QCoreApplication::instance());
    }();
    return bridge;
}
