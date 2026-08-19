/**
 * @file ic_board.h
 * @brief IC 链路数据库及三个串口设备的启动参数。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#ifndef IC_BOARD_H
#define IC_BOARD_H

#include <QObject>
#include <QJsonParseError>
#include <QJsonObject>

#include "common/sql/dbstore.h"
#include "device_config_sync.h"
#include "rs485_driver_port.h"
#include "ic_offline_checker.h"
#include "ic_offline_qr.h"
#include "serial_init.h"
#include "ic_offline.h"
#include "ic_toast.h"
#include "platform/rk3566_platform.h"

/** @brief IC 链路数据库及三个串口设备的启动参数。 */
struct IcSerialConfig {
    QString dbPath = Rk3566Platform::databasePath(); /**< SQLite 数据库路径。 */

    QString qrDev  = Rk3566Platform::qrDevice(); /**< 二维码扫描器串口节点。 */
    int qrBaud     = 9600;                       /**< 二维码串口波特率，bit/s。 */

    QString cardDev = Rk3566Platform::cardDevice(); /**< 刷卡器串口节点。 */
    int cardBaud    = 9600;                         /**< 刷卡器波特率，bit/s。 */

    QString rs485Dev    = Rk3566Platform::rs485Device(); /**< RS485 串口节点。 */
    int rs485Baud       = 9600;                           /**< RS485 波特率，bit/s。 */
    QString rs485DirDev = Rk3566Platform::rs485DirectionDevice(); /**< 可选外部方向控制节点。 */

};

/** @brief 在独立线程中创建和启动完整 IC I/O 链路。 */
class IcIoBootstrap final {
public:

    /**
     * @brief 启动 IC worker，并以 parent 托管线程及 UI 提示生命周期。
     * @param parent 生命周期宿主，同时作为 toast 所属主窗口对象。
     * @param cfg 数据库和串口配置。
     */
    static void startAll(QObject* parent, const IcSerialConfig& cfg = IcSerialConfig());
};

#endif // IC_BOARD_H
