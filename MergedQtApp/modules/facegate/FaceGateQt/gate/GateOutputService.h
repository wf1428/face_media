/**
 * @file GateOutputService.h
 * @brief 验证通过后的闸机输出抽象。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef GATE_OUTPUT_SERVICE_H
#define GATE_OUTPUT_SERVICE_H

#include <QObject>

/**
 * @brief 验证通过后的闸机输出抽象。
 *
 * 当前实现只发布状态，保留 holdMs 参数用于后续接入继电器或 GPIO 脉冲。
 */
class GateOutputService : public QObject {
    Q_OBJECT

public:
    /** @brief 创建闸机输出服务。 */
    explicit GateOutputService(QObject *parent = nullptr);

public slots:
    /** @brief 请求保持指定毫秒数的开闸脉冲。 */
    void pulseOpen(int holdMs = 500);

signals:
    /** @brief 上报开闸请求或硬件状态。 */
    void gateStatus(const QString &message);
};

#endif
