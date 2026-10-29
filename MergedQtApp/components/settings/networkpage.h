/**
 * @file networkpage.h
 * @brief 有线网络 DHCP/静态地址配置与自动恢复页面。
 *
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-10-27；修改人：Du lin；增加手动应用成功提示的代次及目标连接状态，参数就绪并保存后仅提示“网络设置成功”；DHCP 等待、自动联网和续租不弹成功提示，保留原速率与配置规则。
 */

#ifndef NETWORKPAGE_H
#define NETWORKPAGE_H

#include <components/cursoroverlay/keyboard_dialog.h>
#include "components/features/featuresdialog.h"

#include <QWidget>
#include <QEvent>
#include <QMouseEvent>
#include <QLineEdit>
#include <QTimer>
#include <QJsonObject>
#include <atomic>
#include <functional>
#include <memory>
#include "platform/rk3566_platform.h"
#include "common/network_manager.h"

/** Qt Designer 生成的页面类型。 */
namespace Ui {
class NetworkPage;
}

class QLineEdit;
class QThread;
using NetworkRuntimeState = ManagedNetwork::State;
class QCheckBox;
class QLabel;

/** DHCP 自动恢复节流，避免短周期状态同步反复重启客户端。 */
static const qint64 kDhcpAutoRetryIntervalMs = 15000; /**< DHCP 自动恢复最小间隔，单位 ms。 */

/** @brief 有线网络 DHCP/静态地址配置与自动恢复页面。 */
class NetworkPage : public QWidget
{
    Q_OBJECT

public:
    /**
     * @brief 创建网络 UI、速率选项和两秒 NM 状态采样；服务使能在工作线程执行。
     * @param parent Qt 父对象；对象销毁由父子关系管理，空指针表示无父对象。
     */
    explicit NetworkPage(QWidget *parent = nullptr);

    /**
     * @brief 停止轮询，使旧任务失效并等待网络线程退出，再释放页面。
     */
    ~NetworkPage();

    /**
     * @brief 按原功能权限和 use_config/auto_connect 恢复联网，匹配的 NM 活动连接直接复用。
     */
    void autoConnectFromSavedConfig();

    /**
     * @brief 从 INI 回显网络输入及 link_mode，不直接修改接口。
     * @param cfgDir 网络配置所在目录。
     * @return 配置已成功读取并回显时为 true。
     */
    bool loadInputsFromConfigFile(const QString& cfgDir);

    /**
     * @brief 更新功能权限；无 Web 联网意图的 offline 模式经 NM 异步断开有线。
     * @param settings 设备功能开关快照。
     */
    void setFeatureSettings(const FeatureSettings &settings);

    /**
     * @brief Web 显式配置有线网络，复用本页工作线程，不弹出设备端对话框。
     * @param token 贯穿请求与异步结果的任务标识。
     * @param config Web 提交的网络设置，进入异步网络操作前统一校验。
     * @note GUI 线程受理并排队到专用网络线程；完成后通过 token 回报 Web，禁止在界面线程等待网关查询或续租命令。
     */
    void applyWebNetworkConfig(const QString &token, const QJsonObject &config);

signals:
    /**
     * @brief 用户取消网络设置并请求返回。
     */
    void cancelRequested();
    /**
     * @brief 网络配置已成功应用。
     */
    void networkAppliedSuccessfully();

    /**
     * @brief 返回 Web 网络设置的真实应用和配置保存结果。
     * @param token 贯穿请求与异步结果的任务标识。
     * @param result 处理结果。
     * @param code 业务状态码，成功通常为 200。
     * @param message 可返回调用方的结果或错误说明。
     */
    void webNetworkFinished(const QString &token, const QJsonObject &result,
                            int code, const QString &message);

private slots:
    /**
     * @brief DHCP/静态模式切换时更新输入框状态。
     * @param index 界面选项索引。
     */
    void handleIpModeChanged(int index);
    /**
     * @brief 保存并应用当前网络设置。
     */
    void handleApplyClicked();
    /** @brief 只暂存滑块选择，不调用 NM 或保存配置，等待网络页应用按钮。
     *  @param hundred true 为右侧百兆，false 为左侧千兆；程序回显不触发此槽。 */
    void handleLinkModeClicked(bool hundred);
    /**
     * @brief 有线连接模式切换时刷新可用控件。
     * @param index 界面选项索引。
     */
    void handleConnectionTypeChanged(int index);

protected:
    /**
     * @brief 点击可编辑字段时打开屏幕键盘。
     * @param watched 收到事件的对象。
     * @param event Qt 事件。
     * @return 事件已被本对象处理时为 true，否则沿用 Qt 事件分派。
     */
    bool eventFilter(QObject *watched, QEvent *event) override;


private:
    /**
     * @brief 使用项目屏幕键盘编辑指定输入框。
     * @param edit 接收键盘输入的文本框。
     * @param title 键盘窗口标题。
     */
    void openKeyboardFor(QLineEdit *edit, const QString &title);

    /**
     * @brief 启用或禁用静态 IP 输入字段。
     * @param enabled 是否启用相关界面或功能。
     */
    void setIpFieldsEnabled(bool enabled);

    /**
     * @brief 向页面日志区追加文本。
     * @param text 待显示的日志文本。
     */
    void appendLog(const QString &text);

    /**
     * @brief 保存后台采样的 network IPv4 字段；不提交待应用速率或覆盖 INI 速率键。
     * @param cfgDir 网络配置所在目录。
     * @return 现有普通配置文件同步成功为 true；路径异常或写失败为 false，不报告设置成功。
     */
    bool saveInputsToConfigFile(const QString& cfgDir);

    /**
     * @brief 执行 DHCP 或静态网络命令，并按需显示结果。
     * @param showMessage 是否向设备界面显示操作结果；成功提示等参数就绪并保存后再显示。
     * @return 本次设置被校验并受理时为 true；后台结果另行回报。
     */
    bool applyNetworkConfig(bool showMessage);

    /**
     * @brief 合并功能配置与显式 Web 设置判断是否允许联网。
     * @return 在线功能或显式 Web 网络配置要求联网时返回 true。
     */
    bool isNetworkFeatureEnabled() const;

    /**
     * @brief 清除配置文件中的静态 IP 字段。
     * @param cfgDir 网络配置所在目录。
     * @return 原配置中的网络地址清理成功时为 true。
     */
    bool clearNetworkConfigFileIp(const QString& cfgDir);

    /**
     * @brief 将 DHCP 获得的运行时地址同步回配置。
     * @note 轮询仅提交后台采样请求；已有网络任务时合并或延期，界面线程不执行阻塞 NM 命令。
     */
    void syncDhcpRuntimeStateToConfig();

    /** @brief NM 空闲且无地址时按 15 秒节流提交激活；NM 申请地址期间只等待。
     *  @param reason 自动恢复原因，用于日志。 */
    void requestAutoNetworkActivation(const QString &reason);
    /**
     * @brief 串行执行后台网络任务，仅在主线程回传仍有效的结果。
     * @param work 在专用网络线程串行执行的工作，不能直接刷新控件。
     * @param completed 结果回到 GUI 线程后的完成回调。
     * @note 同一时刻只允许一个后台网络任务；配置代次变化时丢弃过期回调并保留未应用的速率选择，完成后再处理延期操作。
     */
    void runNetworkTask(std::function<void()> work, std::function<void()> completed);
    /**
     * @brief 在工作线程按需启动 NM 并读取接口参数，再回显当前页面。
     * @param iface 目标有线网卡名称。
     * @param completed 结果回到 GUI 线程后的完成回调。
     */
    void refreshUiFromSystem(const QString &iface, std::function<void()> completed = {});
    /**
     * @brief 使用后台采集结果执行原 DHCP 状态同步和重试判断。
     * @param iface 目标有线网卡名称。
     * @param state 工作线程采集的当前网卡状态。
     */
    void applyDhcpRuntimeState(const QString &iface, const NetworkRuntimeState &state);
    /** @brief 在网络线程统一应用 DHCP/静态及待应用速率，成功后保存 INI；速率变更失败尝试恢复原 INI。
     *  @param config 已校验的目标接口、IPv4 及速率配置。
     *  @param showMessage 是否显示本地结果提示；DHCP 等待只挂起成功提示，不弹中间状态。 */
    void applyManagedNetworkConfig(const QJsonObject &config, bool showMessage);
    /** @brief 经 NM 异步断开目标有线接口并清空离线缓存，不停止全局服务。
     *  @param iface 由配置确定的有线接口。 */
    void clearManagedNetwork(const QString &iface);
    /** @brief 读取 INI 权威速率，后台刷新保留用户未应用选择。
     *  @param discardSelection true 表示配置加载/启动恢复时丢弃待应用选择；false 保留已有编辑。
     *  @return 读取/百兆缺省初始化成功为 true；失败时保留可信值并记录原因。 */
    bool loadSavedLinkMode(bool discardSelection = false);
    /** @brief 回显当前待应用选择和左右文字状态，不提交任何网络命令。 */
    void updateLinkModeControl();
    /** @brief 参数已保存且目标连接就绪时，消费当前手动应用的一次成功提示。
     *  @param iface 本次采样的有线接口。
     *  @param state 实际接口快照；必须已激活且有有效 IPv4 与掩码。
     *  @pre 调用方已确认当前参数保存成功；仅在 GUI 线程调用。
     *  @note 不要求可选网关/DNS 非空，不检查互联网；代次或连接不匹配时不弹窗。 */
    void showNetworkSuccessIfReady(const QString &iface, const NetworkRuntimeState &state);

private:
    Ui::NetworkPage *ui; /**< Qt Designer 页面对象。 */

    QString configDir_ = Rk3566Platform::netConfigDir();
    QString configFileName_ = "net_cfg.ini";
    QString ifaceName_ = "eth0";
    QCheckBox *linkModeSwitch_ =
        nullptr; /**< 工程共用动画滑块；左千兆(false)、右百兆(true)，仅暂存选择。 */
    QLabel *gigabitRateLabel_ = nullptr; /**< 速率滑块左侧标签，由 Qt 父子关系持有。 */
    QLabel *hundredRateLabel_ = nullptr; /**< 速率滑块右侧标签，由 Qt 父子关系持有。 */
    QString linkMode_ =
        "100-full"; /**< 最近从 net_cfg.ini 读取或保存成功的模式；兼容旧 auto，缺少键默认百兆。 */
    QString selectedLinkMode_ = "100-full"; /**< 界面待应用值；只有点击应用才传给 NM 并持久化。 */
    bool linkModeSelectionDirty_ =
        false;                 /**< true 表示待应用选择不同于最近 INI 保存值；后台不覆盖该编辑。 */
    QString lastNetworkError_; /**< 最近一次 NM 采样失败原因，用于抑制每两秒重复日志。 */
    quint64 pendingNetworkSuccessGeneration_ =
        0; /**< 手动应用待提示的配置代次；0 表示无待提示，配置变化后失效。 */
    QString pendingNetworkSuccessIface_; /**< 待提示请求的有线接口，不接受其他网卡的就绪状态。 */
    QString
        pendingNetworkSuccessUuid_; /**< 待提示请求的 NM 活动连接 UUID，防止其他 profile 触发成功提示。 */

    FeatureSettings featureSettings_;

    QTimer* dhcpSyncTimer_ = nullptr; /**< 仅发起后台采样的 GUI 线程轮询器。 */
    QThread *networkThread_ = nullptr; /**< 串行执行 NM 查询及配置命令的专用线程。 */
    QObject *networkWorker_ = nullptr; /**< 移入专用网络线程的排队任务接收对象。 */
    bool networkTaskPending_ = false; /**< 有网络任务尚未回到 GUI 线程，禁止重入。 */
    bool networkRefreshPending_ = false; /**< 重复状态刷新被合并，当前任务完成后补采样。 */
    bool webNetworkEnabled_ = false; /**< 显式 Web 设置允许联网，不改变 MQTT 功能开关。 */
    QString webNetworkToken_; /**< 正在本页共享网络线程执行的 Web 操作。 */
    std::function<void()> deferredNetworkAction_; /**< 现有任务结束后再受理的手动设置或清理操作。 */
    std::shared_ptr<std::atomic<quint64>> networkGeneration_ =
            std::make_shared<std::atomic<quint64>>(0); /**< 模式/配置变化使旧任务失效。 */

    QString lastIp_;
    QString lastMask_;
    QString lastGateway_;
    QString lastDns_;

    bool lastCarrierUp_ = false;          /**< 上一次检测到的网线状态。 */
    bool dhcpAutoApplying_ = false;       /**< 防止自动 DHCP 流程重入。 */
    bool dhcpHadValidIp_ = false;         /**< 本轮 carrier 在线期间是否取得过有效地址。 */
    qint64 lastDhcpAutoTryMs_ = 0;        /**< 上一次自动 DHCP 时间，单位 ms。 */

};

#endif // NETWORKPAGE_H
