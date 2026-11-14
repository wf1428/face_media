/**
 * @file device_feature_switches.h
 * @brief 读取 net_cfg.ini 的摄像头及分路声音开关，保存后台声音设置。
 * @author Du lin
 * @date 2026-11-10
 * @note 最近修改：2026-11-10；修改人：Du lin；声音设置成功后原子更新运行状态，摄像头仍按开机快照。
 */
#pragma once
#include "platform/rk3566_platform.h"
#include <QSettings>
#include <atomic>

/** @brief UI 线程的声音开关更新通知。 */
class DeviceSoundChanges final : public QObject {
    Q_OBJECT
signals:
    /** @brief 三路声音配置已成功保存并发布给运行中的服务。 */
    void changed();
};

/** @brief 摄像头和三类音频的独立配置，缺失或非法均关闭。 */
struct DeviceFeatureSwitches {
    bool cameraScreenshot=false; /**< 是否授权摄像头截图协议。 */
    bool broadcastAudio=false; /**< 是否允许定时广播播放及计划控制。 */
    bool icCardAudio=false; /**< 是否启用 IC 卡结果提示音。 */
    bool faceAudio=false; /**< 是否启用刷脸结果提示音。 */

    /** @brief 读取一个严格开关，只有 true 或 1 才启用。
     * @param[in] settings 已打开的 INI。
     * @param[in] key 完整组名及字段名。
     * @return 已解析且设置文件无错误时的启用状态。
     */
    static bool readFlag(const QSettings &settings,const QString &key) {
        const auto value=settings.value(key).toString().trimmed().toLower();
        return settings.status()==QSettings::NoError && (value=="true" || value=="1");
    }

    /** @brief 从指定文件读取；解析失败则整份快照关闭。
     * @param[in] path 运行时 net_cfg.ini 路径。
     * @return 当前文件的开关快照。
     */
    static DeviceFeatureSwitches read(const QString &path=Rk3566Platform::netConfigPath()) {
        if(path.isEmpty())return {};
        const QSettings settings(path,QSettings::IniFormat);
        const DeviceFeatureSwitches values{
            readFlag(settings,QStringLiteral("cameraScreenshot/enabled")),
            readFlag(settings,QStringLiteral("sound/broadcastEnabled")),
            readFlag(settings,QStringLiteral("sound/icCardEnabled")),
            readFlag(settings,QStringLiteral("sound/faceEnabled"))
        };
        return settings.status()==QSettings::NoError ? values : DeviceFeatureSwitches{};
    }

    /** @return 业务启动首次读取的只读快照。 */
    static const DeviceFeatureSwitches &boot() {
        static const auto values=read();
        return values;
    }

    /** @return 当前运行状态；IC 工作线程可读取，摄像头沿用开机值。 */
    static DeviceFeatureSwitches current() {
        auto values=boot();
        const auto bits=soundBits().load();
        values.broadcastAudio=(bits&1)!=0;
        values.icCardAudio=(bits&2)!=0;
        values.faceAudio=(bits&4)!=0;
        return values;
    }

    /** @return UI 线程使用的进程内声音变更通知对象。 */
    static DeviceSoundChanges &soundChanges() {
        static DeviceSoundChanges changes;
        return changes;
    }

    /** @brief INI 保存成功后在 UI 线程应用声音开关并通知播放服务。 */
    void applySound() const {
        const auto next=encodeSound(*this);
        if(soundBits().exchange(next)!=next) emit soundChanges().changed();
    }

    /** @brief 仅保存声音三个字段，保留摄像头及其他 INI 项。
     * @param[in] path 运行时 net_cfg.ini 路径。
     * @return 同步写入成功为 true，路径无效或读写失败为 false。
     */
    bool saveSound(const QString &path=Rk3566Platform::netConfigPath()) const {
        if(path.isEmpty())return false;
        QSettings settings(path,QSettings::IniFormat);
        settings.sync();
        if(settings.status()!=QSettings::NoError)return false;
        settings.setValue(QStringLiteral("sound/broadcastEnabled"),broadcastAudio);
        settings.setValue(QStringLiteral("sound/icCardEnabled"),icCardAudio);
        settings.setValue(QStringLiteral("sound/faceEnabled"),faceAudio);
        settings.sync();
        return settings.status()==QSettings::NoError;
    }

private:
    /** @brief 压缩三路声音为单个原子值，读者不会看到部分更新。 */
    static unsigned encodeSound(const DeviceFeatureSwitches &values) {
        return unsigned(values.broadcastAudio) | (unsigned(values.icCardAudio)<<1) |
               (unsigned(values.faceAudio)<<2);
    }
    /** @return 当前声音开关位图，初值来自开机快照。 */
    static std::atomic_uint &soundBits() {
        static std::atomic_uint bits{encodeSound(boot())};
        return bits;
    }
};
