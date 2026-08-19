/**
 * @file device_config_sync.cpp
 * @brief 加载、校验并持久化 IC 设备运行配置。
 *
 * @author Dulin
 * @date 2026-07-25
 */

#include "device_config_sync.h"
#include "common/sql/dbstore.h"
#include <QDebug>
#include <QMutex>
#include <QMutexLocker>


DeviceConfig DeviceConfigSync::m_cfg;
/** 保护进程内配置快照，数据库连接仍按线程独立管理。 */
static QMutex g_cfgMutex;

/**
 * @brief 返回当前 IC 运行配置快照。
 *
 * @return 当前 DeviceConfig 的线程安全副本。
 *
 * @note 调用方只能使用返回的副本，不能长期持有静态配置对象的引用。
 */
DeviceConfig DeviceConfigSync::cfg()
{
    QMutexLocker locker(&g_cfgMutex);
    return m_cfg;
}

/** @return 指定配置键的字符串值；不存在或失败时使用默认值。 */
static QString getStr(const char* key, const char* defVal) {
    return DbStore::getConfig(key, defVal).toString();
}

/** @return 指定配置键的整数值；不存在或失败时使用默认值。 */
static int getInt(const char* key, int defVal) {
    return DbStore::getConfig(key, defVal).toInt();
}

/**
 * @brief 完成数据库初始化、配置加载、归一化和缺失键补齐。
 *
 * 已存在的数据库值不会被默认值覆盖；补齐后执行 checkpoint 并刷新线程安全快照。
 */
DeviceConfig DeviceConfigSync::bootstrapAndLoad(const QString& dbPath)
{
    // 1) 确保 DB 可用（建表/索引等）
    if (!DbStore::bootstrap(dbPath)) {
        qDebug() << "[DeviceConfigSync] DB bootstrap failed:" << DbStore::lastError();
        // DB 起不来就返回默认配置
        DeviceConfig cfg;
        applyDefaults(cfg);
        return cfg;
    }

    // 2) 从 DB 读取
    DeviceConfig cfg = loadFromDb();

    // 3) 默认值,做一次校验修正
    applyDefaults(cfg);

    // 4) 只补齐 DB 里缺失的 key（不覆盖已有值）
    auto ensure = [&](const char* key, const QVariant& val){
        if (!DbStore::hasConfig(key)) {
            DbStore::setConfig(key, val);
        }
    };

    ensure("message_id", cfg.messageId);
    ensure("register_times", cfg.registerTimes);
    ensure("card_secret", cfg.cardSecret);
    ensure("floor_num", cfg.floorNum);
    ensure("qrcode_secret", cfg.qrSecret);
    ensure("device_name", cfg.deviceName);
    ensure("network_interface", cfg.networkInterface);
    ensure("cardinfo_output", cfg.cardInfoOutput);
    ensure("sector_num", cfg.sectorNum);
    ensure("server_output", cfg.serverOutput);
    ensure("relay_num", cfg.relayNum);
    ensure("relay_times", cfg.relayTimes);
    ensure("ip_static", cfg.ipStatic);
    ensure("ip_gateway", cfg.ipGateway);
    ensure("ip_mask", cfg.ipMask);
    ensure("dns_server", cfg.dnsServer);
    ensure("dns_ip", cfg.dnsIp);
    ensure("dns_gateway", cfg.dnsGateway);
    ensure("dns_mask", cfg.dnsMask);
    ensure("mac", cfg.mac);
    ensure("remote_ip", cfg.remoteIp);

    ensure("floor8_1flag",    (int)cfg.floor8_1flag);     ensure("floor1_8flag",    (int)cfg.floor1_8flag);
    ensure("floor9_16flag",   (int)cfg.floor9_16flag);    ensure("floor17_24flag",  (int)cfg.floor17_24flag);
    ensure("floor25_32flag",  (int)cfg.floor25_32flag);   ensure("floor33_40flag",  (int)cfg.floor33_40flag);
    ensure("floor41_48flag",  (int)cfg.floor41_48flag);   ensure("floor49_56flag",  (int)cfg.floor49_56flag);
    ensure("floor57_64flag",  (int)cfg.floor57_64flag);   ensure("floor65_72flag",  (int)cfg.floor65_72flag);
    ensure("floor73_80flag",  (int)cfg.floor73_80flag);   ensure("floor81_88flag",  (int)cfg.floor81_88flag);
    ensure("floor89_96flag",  (int)cfg.floor89_96flag);   ensure("floor97_104flag", (int)cfg.floor97_104flag);
    ensure("floor105_112flag",(int)cfg.floor105_112flag); ensure("floor113_120flag",(int)cfg.floor113_120flag);

    // 把补齐写入落盘（WAL 下更稳）
    DbStore::checkpoint();


    {
        QMutexLocker locker(&g_cfgMutex);
        m_cfg = cfg;
    }
    return cfg;
}

/** @return 逐项从 config 表读取的原始 DeviceConfig。 */
DeviceConfig DeviceConfigSync::loadFromDb()
{
    DeviceConfig cfg;

    cfg.messageId = getInt("message_id", 0);
    cfg.registerTimes = getInt("register_times", 0);

    cfg.cardSecret = getStr("card_secret", "20250101");
    cfg.floorNum   = getInt("floor_num", 1);
    cfg.qrSecret   = getStr("qrcode_secret", "250101");
    cfg.deviceName = getStr("device_name", "YCEE250101V00001");

    cfg.networkInterface = getInt("network_interface", 0);
    cfg.cardInfoOutput   = getInt("cardinfo_output", 0);
    cfg.sectorNum        = getInt("sector_num", 6);
    cfg.serverOutput     = getInt("server_output", 0);

    cfg.relayNum   = getInt("relay_num", 3);
    cfg.relayTimes = getInt("relay_times", 10);

    cfg.ipStatic  = getStr("ip_static",  "192.168.3.112");
    cfg.ipGateway = getStr("ip_gateway", "192.168.3.1");
    cfg.ipMask    = getStr("ip_mask",    "255.255.255.0");

    cfg.dnsServer  = getStr("dns_server",  "192.168.1.1");
    cfg.dnsIp      = getStr("dns_ip",      "192.168.1.110");
    cfg.dnsGateway = getStr("dns_gateway", "192.168.1.1");
    cfg.dnsMask    = getStr("dns_mask",    "255.255.255.0");

    cfg.mac      = getStr("mac",       "B8:6E:3D:00:00:00");
    cfg.remoteIp = getStr("remote_ip", "192.168.3.9");

    cfg.floor8_1flag    = getInt("floor8_1flag",    OCVALUE);
    cfg.floor1_8flag    = getInt("floor1_8flag",    OCVALUE);
    cfg.floor9_16flag   = getInt("floor9_16flag",   OCVALUE);
    cfg.floor17_24flag  = getInt("floor17_24flag",  OCVALUE);
    cfg.floor25_32flag  = getInt("floor25_32flag",  OCVALUE);
    cfg.floor33_40flag  = getInt("floor33_40flag",  OCVALUE);
    cfg.floor41_48flag  = getInt("floor41_48flag",  OCVALUE);
    cfg.floor49_56flag  = getInt("floor49_56flag",  OCVALUE);
    cfg.floor57_64flag  = getInt("floor57_64flag",  OCVALUE);
    cfg.floor65_72flag  = getInt("floor65_72flag",  OCVALUE);
    cfg.floor73_80flag  = getInt("floor73_80flag",  OCVALUE);
    cfg.floor81_88flag  = getInt("floor81_88flag",  OCVALUE);
    cfg.floor89_96flag  = getInt("floor89_96flag",  OCVALUE);
    cfg.floor97_104flag = getInt("floor97_104flag", OCVALUE);
    cfg.floor105_112flag= getInt("floor105_112flag",OCVALUE);
    cfg.floor113_120flag= getInt("floor113_120flag",OCVALUE);

    return cfg;
}

/** @return 0~255 原值；超出单字节范围时回退为全部开放的 OCVALUE。 */
static uint sanitizeFloorFlag(uint v)
{
    return (v <= 255) ? v : OCVALUE;
}

/**
 * @brief 将密钥长度、设备名、继电器参数、开关和楼层权限字节修正到允许范围。
 */
void DeviceConfigSync::applyDefaults(DeviceConfig& cfg)
{
    // card secret
    if (cfg.cardSecret.size() != 8) cfg.cardSecret = "20250101";

    // floor num：
    if (cfg.floorNum == 255 || cfg.floorNum <= 0) cfg.floorNum = 1;

    // qr secret
    if (cfg.qrSecret.size() != 6) cfg.qrSecret = "250101";

    // device name
    if (cfg.deviceName.isEmpty()) cfg.deviceName = "YCEE250101V00001";
    if (cfg.deviceName.size() > 16) cfg.deviceName = cfg.deviceName.left(16);

    // relay defaults
    if (cfg.relayNum == 255 || cfg.relayNum <= 0) cfg.relayNum = 3;
    if (cfg.relayTimes == 255 || cfg.relayTimes <= 0) cfg.relayTimes = 10;

    // switches sanitize（只允许0/1）
    cfg.networkInterface = cfg.networkInterface ? 1 : 0;
    cfg.cardInfoOutput   = cfg.cardInfoOutput ? 1 : 0;
    cfg.serverOutput     = cfg.serverOutput ? 1 : 0;

    // sector
    if (cfg.sectorNum == 255 || cfg.sectorNum <= 0) cfg.sectorNum = 2;

    cfg.floor8_1flag     = sanitizeFloorFlag(cfg.floor8_1flag);
    cfg.floor1_8flag     = sanitizeFloorFlag(cfg.floor1_8flag);
    cfg.floor9_16flag    = sanitizeFloorFlag(cfg.floor9_16flag);
    cfg.floor17_24flag   = sanitizeFloorFlag(cfg.floor17_24flag);
    cfg.floor25_32flag   = sanitizeFloorFlag(cfg.floor25_32flag);
    cfg.floor33_40flag   = sanitizeFloorFlag(cfg.floor33_40flag);
    cfg.floor41_48flag   = sanitizeFloorFlag(cfg.floor41_48flag);
    cfg.floor49_56flag   = sanitizeFloorFlag(cfg.floor49_56flag);
    cfg.floor57_64flag   = sanitizeFloorFlag(cfg.floor57_64flag);
    cfg.floor65_72flag   = sanitizeFloorFlag(cfg.floor65_72flag);
    cfg.floor73_80flag   = sanitizeFloorFlag(cfg.floor73_80flag);
    cfg.floor81_88flag   = sanitizeFloorFlag(cfg.floor81_88flag);
    cfg.floor89_96flag   = sanitizeFloorFlag(cfg.floor89_96flag);
    cfg.floor97_104flag  = sanitizeFloorFlag(cfg.floor97_104flag);
    cfg.floor105_112flag = sanitizeFloorFlag(cfg.floor105_112flag);
    cfg.floor113_120flag = sanitizeFloorFlag(cfg.floor113_120flag);

    // 其它字段（IP/MAC）
}

/** @brief 从已打开数据库重新加载并原子替换内存配置快照。 */
bool DeviceConfigSync::load()
{
    DeviceConfig cfg = loadFromDb();
    applyDefaults(cfg);

    {
        QMutexLocker locker(&g_cfgMutex);
        m_cfg = cfg;
    }

    qInfo() << "[DeviceConfigSync] reloaded:"
            << "floorNum=" << cfg.floorNum
            << "cardSecret=" << cfg.cardSecret
            << "sectorNum=" << cfg.sectorNum;

    return true;
}

namespace {

/** @brief 按 bit7 到 bit0 顺序把一个权限字节追加为 8 个 '0'/'1' 字符。 */
static void appendFlagBits(QByteArray &out, uint flag)
{
    out.append((flag & 0x80) ? '1' : '0');
    out.append((flag & 0x40) ? '1' : '0');
    out.append((flag & 0x20) ? '1' : '0');
    out.append((flag & 0x10) ? '1' : '0');
    out.append((flag & 0x08) ? '1' : '0');
    out.append((flag & 0x04) ? '1' : '0');
    out.append((flag & 0x02) ? '1' : '0');
    out.append((flag & 0x01) ? '1' : '0');
}

/** @return 从 offset 开始的 8 个字符按 bit7~bit0 还原出的权限字节。 */
static uint bitsToFlag(const QByteArray &bits, int offset)
{
    uint v = 0;
    v |= (bits[offset + 0] == '1' ? 0x80 : 0);
    v |= (bits[offset + 1] == '1' ? 0x40 : 0);
    v |= (bits[offset + 2] == '1' ? 0x20 : 0);
    v |= (bits[offset + 3] == '1' ? 0x10 : 0);
    v |= (bits[offset + 4] == '1' ? 0x08 : 0);
    v |= (bits[offset + 5] == '1' ? 0x04 : 0);
    v |= (bits[offset + 6] == '1' ? 0x02 : 0);
    v |= (bits[offset + 7] == '1' ? 0x01 : 0);
    return v;
}

} // namespace


/** @return 按旧 Core 字段顺序拼接的 128 位楼层权限字符串。 */
QByteArray DeviceConfigSync::floorFlags128Bits()
{
    const DeviceConfig c = cfg();
    QByteArray bits;
    bits.reserve(128);

    appendFlagBits(bits, c.floor8_1flag);
    appendFlagBits(bits, c.floor1_8flag);
    appendFlagBits(bits, c.floor9_16flag);
    appendFlagBits(bits, c.floor17_24flag);
    appendFlagBits(bits, c.floor25_32flag);
    appendFlagBits(bits, c.floor33_40flag);
    appendFlagBits(bits, c.floor41_48flag);
    appendFlagBits(bits, c.floor49_56flag);
    appendFlagBits(bits, c.floor57_64flag);
    appendFlagBits(bits, c.floor65_72flag);
    appendFlagBits(bits, c.floor73_80flag);
    appendFlagBits(bits, c.floor81_88flag);
    appendFlagBits(bits, c.floor89_96flag);
    appendFlagBits(bits, c.floor97_104flag);
    appendFlagBits(bits, c.floor105_112flag);
    appendFlagBits(bits, c.floor113_120flag);

    return bits;
}


/** @return 每 4 个权限位编码为一个大写十六进制字符后的 32 位文本。 */
QString DeviceConfigSync::floorFlagsHex32()
{
    const QByteArray bits = floorFlags128Bits();
    QByteArray out;
    out.reserve(32);

    for (int i = 0; i < 128; i += 4) {
        int v = 0;
        v |= (bits[i + 0] == '1' ? 8 : 0);
        v |= (bits[i + 1] == '1' ? 4 : 0);
        v |= (bits[i + 2] == '1' ? 2 : 0);
        v |= (bits[i + 3] == '1' ? 1 : 0);
        out.append("0123456789ABCDEF"[v]);
    }

    return QString::fromLatin1(out);
}


/**
 * @brief 将 128 位权限字符串分组保存并刷新内存快照。
 * @param bits128 恰好 128 个字符的位图；实现仅把字符 '1' 视为开放。
 * @return 长度正确并完成保存流程时返回 true。
 */
bool DeviceConfigSync::saveFloorFlags128Bits(const QByteArray &bits128)
{
    if (bits128.size() != 128) {
        qWarning() << "[DeviceConfigSync] saveFloorFlags128Bits invalid size:" << bits128.size();
        return false;
    }

    DeviceConfig c = cfg();

    c.floor8_1flag     = bitsToFlag(bits128,   0);
    c.floor1_8flag     = bitsToFlag(bits128,   8);
    c.floor9_16flag    = bitsToFlag(bits128,  16);
    c.floor17_24flag   = bitsToFlag(bits128,  24);
    c.floor25_32flag   = bitsToFlag(bits128,  32);
    c.floor33_40flag   = bitsToFlag(bits128,  40);
    c.floor41_48flag   = bitsToFlag(bits128,  48);
    c.floor49_56flag   = bitsToFlag(bits128,  56);
    c.floor57_64flag   = bitsToFlag(bits128,  64);
    c.floor65_72flag   = bitsToFlag(bits128,  72);
    c.floor73_80flag   = bitsToFlag(bits128,  80);
    c.floor81_88flag   = bitsToFlag(bits128,  88);
    c.floor89_96flag   = bitsToFlag(bits128,  96);
    c.floor97_104flag  = bitsToFlag(bits128, 104);
    c.floor105_112flag = bitsToFlag(bits128, 112);
    c.floor113_120flag = bitsToFlag(bits128, 120);

    DbStore::setConfig("floor8_1flag",     (int)c.floor8_1flag);
    DbStore::setConfig("floor1_8flag",     (int)c.floor1_8flag);
    DbStore::setConfig("floor9_16flag",    (int)c.floor9_16flag);
    DbStore::setConfig("floor17_24flag",   (int)c.floor17_24flag);
    DbStore::setConfig("floor25_32flag",   (int)c.floor25_32flag);
    DbStore::setConfig("floor33_40flag",   (int)c.floor33_40flag);
    DbStore::setConfig("floor41_48flag",   (int)c.floor41_48flag);
    DbStore::setConfig("floor49_56flag",   (int)c.floor49_56flag);
    DbStore::setConfig("floor57_64flag",   (int)c.floor57_64flag);
    DbStore::setConfig("floor65_72flag",   (int)c.floor65_72flag);
    DbStore::setConfig("floor73_80flag",   (int)c.floor73_80flag);
    DbStore::setConfig("floor81_88flag",   (int)c.floor81_88flag);
    DbStore::setConfig("floor89_96flag",   (int)c.floor89_96flag);
    DbStore::setConfig("floor97_104flag",  (int)c.floor97_104flag);
    DbStore::setConfig("floor105_112flag", (int)c.floor105_112flag);
    DbStore::setConfig("floor113_120flag", (int)c.floor113_120flag);

    DbStore::checkpoint();
    {
        QMutexLocker locker(&g_cfgMutex);
        m_cfg = c;
    }

    qInfo() << "[DeviceConfigSync] floor flags updated, hex32 =" << floorFlagsHex32();
    return true;
}
