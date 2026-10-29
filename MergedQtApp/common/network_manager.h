/**
 * @file network_manager.h
 * @brief 通过 NetworkManager 统一管理有线 IPv4、DNS、连接恢复及链路速率。
 * @author Du lin
 * @date 2026-10-26
 * @note 最近修改：2026-10-27；修改人：Du lin；说明百兆/千兆协商及旧 auto 兼容、实际速率比较和无载波静态配置的验证边界。
 */
#pragma once
#include <QElapsedTimer>
#include <QJsonObject>
#include <QStringList>
#include <functional>

namespace ManagedNetwork {
/** @brief NM 接口快照；采样失败 available=false，不能据此判定网线断开。 */
struct State {
    QString ip;             /**< 第一条有效 IPv4 地址；尚未取得时为空。 */
    QString mask;           /**< ip 对应的点分十进制掩码。 */
    QString gateway;        /**< 目标接口的 IPv4 默认网关；未配置时为空。 */
    QString dns;            /**< 接口实际 IPv4 DNS，以空格分隔，不读取本地解析器桩地址。 */
    QString uuid;           /**< 活动连接 UUID；无活动连接时为空。 */
    QString type;           /**< NM 接口类型；有线配置只接受 ethernet。 */
    QString error;          /**< 采样失败原因；成功时为空。 */
    int deviceState = 0;    /**< NMDeviceState；40～90 为激活中，100 为已激活。 */
    bool carrierUp = false; /**< NM 报告的物理载波状态。 */
    bool available = false; /**< true 表示本次成功读取 NM 接口状态。 */
    /** @return 已取得有效 IPv4 和掩码时为 true。 */
    bool hasAddress() const;
    /** @return NM 正在激活时为 true；后台不得重复提交激活。 */
    bool activating() const;
};

/** @brief 有界命令结果，不转移任何进程或输出缓冲区所有权。 */
struct CommandResult {
    int code = -1;  /**< 0 为成功；非零包含命令失败、超时和取消。 */
    QString output; /**< UTF-8 标准输出。 */
    QString error;  /**< 标准错误或执行器生成的失败说明。 */
};

/**
 * @brief 每项工作独立持有的 NM 适配器；不启动 DHCP 客户端或写系统解析器文件。
 * @note 阻塞接口仅在工作线程调用，NetworkPage 串行化同一网卡的写操作。
 *       项目连接 autoconnect=false，由 Qt 控制首次激活和载波恢复，NM 负责租约续租。
 */
class Backend {
public:

    using Runner = std::function<CommandResult(const QString &, const QStringList &, int)>;
    using Current = std::function<bool()>;
    /** @brief 绑定任务执行器和代次检查并启动单调预算计时，不访问网卡。
     *  @param runner 可选隔离命令执行器；缺省执行真实固定命令。
     *  @param current 可选代次检查；false 后禁止提交后续命令。 */
    explicit Backend(Runner runner = {}, Current current = {});
    /** @brief 按需使能 NM 服务；由 Qt 的网络线程调用。
     *  @param[out] error 可空失败说明。
     *  @return NM 已运行或由 systemd 启动成功时为 true；不 enable 或停止全局服务。 */
    bool ensureService(QString *error);
    /** @brief 一次读取目标接口的 NM 状态及 IPv4 参数。
     *  @param iface 目标接口。
     *  @return NM 状态，失败 available=false；不重建连接或重启服务。 */
    State query(const QString &iface);
    /**
     * @brief 复用或更新目标有线连接并回读实际 IPv4/速率结果。
     * @param config 已规范化的 iface、dhcp、静态字段和可选 linkMode(缺省 100-full，支持 1000-full/旧 auto)。
     * @param[out] state 非空输出，接收请求提交后的快照；失败时仅供诊断。
     * @param[out] error 可空失败说明。
     * @return NM 保存并受理连接请求时为 true；DHCP 尚无地址也可为 true。
     * @note 相同活动参数与协商速率直接复用；有载波时百兆/千兆须回读到目标全双工速率。
     *       无载波静态配置只提交参数，实际速率须插线后确认。
     *       速率切换可短暂断链，不重启 Qt、系统或 NetworkManager。
     */
    bool apply(const QJsonObject &config, State *state, QString *error);
    /** @brief 仅断开目标有线接口并禁止其自动连接。
     *  @param iface 目标有线接口。
     *  @param[out] error 可空失败说明。
     *  @return 经 NM 禁止接口自动连接并断开成功时为 true；不影响热点及其他接口。 */
    bool disconnect(const QString &iface, QString *error);

private:

    /**
     * @brief 校验代次并限时执行固定命令，参数独立传递，不经 shell。
     * @param tool 固定工具名，由平台定位可执行文件。
     * @param args 独立命令参数，不拼接 shell 文本。
     * @param timeoutMs 单次命令预算，单位毫秒，另受本任务 50 秒总预算约束。
     * @return 成功、非零退出或超时/取消的真实命令结果。
     */
    CommandResult run(const QString &tool, const QStringList &args, int timeoutMs = 5000);
    /** @brief 为 nmcli 固定输出转义和等待预算。
     *  @param args nmcli 子命令及结构化参数。
     *  @param waitSeconds NM 等待时间，单位秒；0 表示只提交异步请求。
     *  @return nmcli 的有界命令结果，0 退出不等同于 IPv4 已生效。 */
    CommandResult nm(const QStringList &args, int waitSeconds = 3);
    /** @brief 校验有线接口并禁止其他配置自动接管。
     *  @param iface 合法目标接口名。
     *  @param[out] error 可空失败说明。
     *  @return 接口为 ethernet、旧 DHCP 已退出且管理权设置成功时为 true。 */
    bool claim(const QString &iface, QString *error);
    /** @brief 复用或创建绑定接口的项目连接。
     *  @param iface 目标有线接口。
     *  @param[out] error 可空失败说明，包含名称冲突及命令错误。
     *  @return 项目连接 UUID；失败返回空文本，不修改其他接口的连接。 */
    QString profile(const QString &iface, QString *error);
    /** @brief 只清退旧 Qt PID 文件确实指向的同接口 DHCP 进程；不再维护独立 DHCP。
     *  @param iface 目标有线接口。
     *  @param[out] error 可空失败说明。
     *  @return 原本无旧进程或 SIGTERM 后两秒内退出时为 true；无法确认进程归属或仍存活时为 false。 */
    bool retireLegacyDhcp(const QString &iface, QString *error);
    /** @brief 最多约八秒等待驱动回报所选全双工速率。
     *  @param iface 目标有线接口。
     *  @param mode 百兆/千兆模式；旧 auto 不限制实际协商速率。
     *  @param[out] error 可空失败说明。
     *  @return 实际速率、双工和载波符合要求时为 true；不支持、超时或取消为 false。 */
    bool verifyLinkMode(const QString &iface, const QString &mode, QString *error);
    Runner runner_;   /**< 本工作独立持有的命令执行器。 */
    Current current_; /**< 可选任务代次及线程退出检查。 */
    QElapsedTimer
        budget_; /**< 本任务单调计时，全部外部命令共用 50 秒预算，低于 Web 的 60 秒等待上限。 */
};
} // namespace ManagedNetwork
