/**
 * @file IPresenceSensor.h
 * @brief 人员存在检测器抽象接口。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef IPRESENCESENSOR_H
#define IPRESENCESENSOR_H

#include <QObject>
#include <QString>

/**
 * @brief 人员存在检测器抽象接口。
 *
 * 具体驱动负责产生“检测到人员”“人员离开”和故障信号，模式控制器只依赖该接口，
 * 不感知传感器是调试输入还是真实硬件。
 */
class IPresenceSensor : public QObject
{
    Q_OBJECT

public:
    /** @brief 创建传感器接口对象并交由 Qt 父对象管理。 */
    explicit IPresenceSensor(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    /** @brief 允许通过接口指针安全销毁具体传感器。 */
    ~IPresenceSensor() override = default;

    /** @return 传感器成功开始采集时返回 true。 */
    virtual bool start() = 0;

    /** @brief 停止采集并撤销已安装的系统资源。 */
    virtual void stop() = 0;

    /** @return 传感器当前正在采集时返回 true。 */
    virtual bool isRunning() const = 0;

signals:
    /** @brief 通知控制器已检测到人员。 */
    void presenceDetected();

    /** @brief 通知控制器人员已离开。 */
    void presenceLost();

    /** @brief 上报传感器无法启动或运行异常。 */
    void sensorFault(const QString &message);
};

#endif // IPRESENCESENSOR_H
