/**
 * @file LedFillLight.h
 * @brief /dev/led 人脸识别补光灯控制模块。
 */

#ifndef LEDFILLLIGHT_H
#define LEDFILLLIGHT_H

#include <QObject>
#include <QString>

/**
 * @brief 通过 LED 字符设备控制识别补光灯。
 *
 * 驱动约定写入字符 '1' 开灯、写入字符 '0' 关灯。相同状态重复请求不会
 * 重复访问设备；写入失败时不更新缓存状态，后续请求会自动重试。
 */
class LedFillLight final : public QObject
{
public:
    explicit LedFillLight(const QString &devicePath,
                          QObject *parent = nullptr);
    ~LedFillLight() override;

    /** @brief 打开补光灯。 */
    bool turnOn();

    /** @brief 关闭补光灯。 */
    bool turnOff();

    /** @return 最近一次成功写入的补光灯状态。 */
    bool isOn() const;

private:
    bool applyState(bool on);

    QString devicePath_;
    bool on_ = false;
    bool stateKnown_ = false;
};

#endif // LEDFILLLIGHT_H
