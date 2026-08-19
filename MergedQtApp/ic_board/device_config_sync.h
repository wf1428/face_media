/**
 * @file device_config_sync.h
 * @brief IC 设备运行配置快照。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#pragma once

#include <QString>
#include <QByteArray>
#include "common/sql/dbstore.h"

/** 楼层权限字节的默认值：8 位全部开放。 */
#define OCVALUE 255

/**
 * @brief IC 设备运行配置快照。
 *
 * 字段从 SQLite config 表加载，并由 DeviceConfigSync 进行长度、范围和默认值归一化。
 */
struct DeviceConfig {
    int messageId = 0;       /**< 兼容旧协议的消息序号。 */
    int registerTimes = 0;   /**< 已登记卡数量兼容计数。 */

    QString cardSecret = "20250101"; /**< 离线卡密钥，固定 8 字节。 */
    int floorNum = 1;                /**< 当前设备楼层号，必须大于 0。 */
    QString qrSecret = "250101";     /**< 离线二维码密钥，固定 6 字节。 */
    QString deviceName = "YCEE250101V00001"; /**< 设备名，最长 16 个字符。 */

    int networkInterface = 0; /**< 网络接口开关，归一化为 0 或 1。 */
    int cardInfoOutput = 0;   /**< 卡片信息输出开关，归一化为 0 或 1。 */
    int sectorNum = 6;        /**< 卡片扇区编号，非法值回退为 2。 */
    int serverOutput = 0;     /**< 服务器输出开关，归一化为 0 或 1。 */

    int relayNum = 3;         /**< 默认继电器编号。 */
    int relayTimes = 10;      /**< 默认继电器动作次数或持续参数。 */

    QString ipStatic = "192.168.3.112";  /**< 静态 IP 地址。 */
    QString ipGateway = "192.168.3.1";   /**< 静态网络网关。 */
    QString ipMask = "255.255.255.0";    /**< 静态网络掩码。 */

    QString dnsServer = "192.168.1.1";   /**< DNS 服务器地址。 */
    QString dnsIp = "192.168.1.110";     /**< DNS 网络兼容配置的本机地址。 */
    QString dnsGateway = "192.168.1.1";  /**< DNS 网络兼容配置的网关。 */
    QString dnsMask = "255.255.255.0";   /**< DNS 网络兼容配置的掩码。 */

    QString mac = "B8:6E:3D:00:00:00";  /**< 设备 MAC 配置。 */
    QString remoteIp = "192.168.3.9";    /**< 远端服务地址。 */

    // 每个字段保存 8 个楼层的开放位，1 表示允许；名称沿用旧 Core 配置键。
    uint floor8_1flag   = OCVALUE; uint floor1_8flag    = OCVALUE; uint floor9_16flag    = OCVALUE; uint floor17_24flag   = OCVALUE;
    uint floor25_32flag = OCVALUE; uint floor33_40flag  = OCVALUE; uint floor41_48flag   = OCVALUE; uint floor49_56flag   = OCVALUE;
    uint floor57_64flag = OCVALUE; uint floor65_72flag  = OCVALUE; uint floor73_80flag   = OCVALUE; uint floor81_88flag   = OCVALUE;
    uint floor89_96flag = OCVALUE; uint floor97_104flag = OCVALUE; uint floor105_112flag = OCVALUE; uint floor113_120flag = OCVALUE;

};

/**
 * @brief IC 设备配置的数据库同步与楼层位图转换器。
 *
 * 内部静态快照由互斥锁保护；cfg() 返回副本，避免调用方跨线程持有共享引用。
 */
class DeviceConfigSync {
public:
    /**
     * @brief 初始化数据库、加载配置、补齐缺失键并刷新内存快照。
     * @return 归一化后的配置；数据库初始化失败时返回归一化默认值。
     */
    static DeviceConfig bootstrapAndLoad(const QString& dbPath);

    /** @brief 运行期间从已初始化数据库重新加载配置快照。 */
    static bool load();

    /** @return 当前配置的线程安全副本。 */
    static DeviceConfig cfg();

    /** @return 按旧配置字段顺序拼接的 128 个 '0'/'1' 权限位。 */
    static QByteArray floorFlags128Bits();

    /** @return 128 位权限位图对应的 32 位大写十六进制字符串。 */
    static QString floorFlagsHex32();

    /** @brief 将恰好 128 个权限位分组写回数据库并刷新内存快照。 */
    static bool saveFloorFlags128Bits(const QByteArray& bits128);

private:
    static DeviceConfig  m_cfg; /**< 由实现文件互斥锁保护的进程内配置快照。 */

    /** @return 从数据库逐项读取的配置，尚未执行归一化。 */
    static DeviceConfig loadFromDb();

    /** @brief 修正密钥长度、开关范围和非法特殊值。 */
    static void applyDefaults(DeviceConfig& cfg);
};
