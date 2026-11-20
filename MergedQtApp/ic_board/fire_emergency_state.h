/**
 * @file fire_emergency_state.h
 * @brief 提供消防输入对应的进程级业务状态。
 * @details 状态由 IC 工作线程中的 GPIO 监视器更新，MQTT 等其他线程只读取；
 *          状态不落库，设备启动后始终以 /dev/fire_gpio 的实时电平为准。
 * @author Du lin
 * @date 2026-11-19
 */

#ifndef FIRE_EMERGENCY_STATE_H
#define FIRE_EMERGENCY_STATE_H

namespace FireEmergencyState {

/**
 * @brief 查询设备当前是否处于消防业务状态。
 * @return true 表示 GPIO 已触发消防；false 表示正常梯控状态。
 * @note 本函数可跨线程调用，读取过程不会阻塞调用线程。
 */
bool isActive();

/**
 * @brief 更新消防业务状态。
 * @param[in] active true 表示进入消防，false 表示退出消防。
 * @return 更新前的状态，便于调用者忽略重复电平。
 * @note 仅消防 GPIO 状态机可以调用；平台消息和信号板解析不得修改该状态。
 */
bool exchangeActive(bool active);

} // namespace FireEmergencyState

#endif // FIRE_EMERGENCY_STATE_H
