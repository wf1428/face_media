/**
 * @file GateOutputService.cpp
 * @brief 验证通过后的闸机输出抽象的实现。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "GateOutputService.h"

#include <QTimer>

/** @brief 创建闸机输出服务。 */
GateOutputService::GateOutputService(QObject *parent) : QObject(parent)
{
}

/** @brief 请求保持指定毫秒数的开闸脉冲。 */
void GateOutputService::pulseOpen(int holdMs)
{
    emit gateStatus("闸机开门信号已拉起");
    QTimer::singleShot(holdMs, this, [this]() {
        emit gateStatus("闸机开门信号已释放");
    });
}
