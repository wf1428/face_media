/**
 * @file networkpage.h
 * @brief 有线网络 DHCP/静态地址配置与自动恢复页面。
 *
 * @author Dulin
 * @date 2026-07-25
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

/** Qt Designer 生成的页面类型。 */
namespace Ui {
class NetworkPage;
}

class QLineEdit;
class QThread;
struct NetworkRuntimeState;

/** DHCP 自动恢复节流，避免短周期状态同步反复重启客户端。 */
static const qint64 kDhcpAutoRetryIntervalMs = 15000; /**< DHCP 自动恢复最小间隔，单位 ms。 */

/** DHCP 地址存在时的网络健康检查间隔。 */
static const qint64 kDhcpForceRetryIntervalMs = 30000; /**< DHCP 健康检查强制重试间隔，单位 ms。 */

/** @brief 有线网络 DHCP/静态地址配置与自动恢复页面。 */
class NetworkPage : public QWidget
{
    Q_OBJECT

public:
    /** @brief 创建网络配置 UI 和 DHCP 状态同步定时器。 */
    explicit NetworkPage(QWidget *parent = nullptr);

    /** @brief 停止定时器并释放 Qt Designer UI。 */
    ~NetworkPage();

    /** @brief 根据已保存配置执行一次自动联网。 */
    void autoConnectFromSavedConfig();

    /** @brief 从指定目录的 INI 文件回显网络输入。 */
    bool loadInputsFromConfigFile(const QString& cfgDir);

    /** @brief 更新功能开关并决定网络功能是否可用。 */
    void setFeatureSettings(const FeatureSettings &settings);

    /** @brief Web 显式配置有线网络，复用本页工作线程，不弹出设备端对话框。 */
    void applyWebNetworkConfig(const QString &token, const QJsonObject &config);

signals:
    /** @brief 用户取消网络设置并请求返回。 */
    void cancelRequested();
    /** @brief 网络配置已成功应用。 */
    void networkAppliedSuccessfully();

    /** @brief 返回 Web 网络设置的真实应用和配置保存结果。 */
    void webNetworkFinished(const QString &token, const QJsonObject &result,
                            int code, const QString &message);

private slots:
    /** @brief DHCP/静态模式切换时更新输入框状态。 */
    void handleIpModeChanged(int index);
    /** @brief 保存并应用当前网络设置。 */
    void handleApplyClicked();
    /** @brief 有线连接模式切换时刷新可用控件。 */
    void handleConnectionTypeChanged(int index);

protected:
    /** @brief 点击可编辑字段时打开屏幕键盘。 */
    bool eventFilter(QObject *watched, QEvent *event) override;


private:
    /** @brief 使用项目屏幕键盘编辑指定输入框。 */
    void openKeyboardFor(QLineEdit *edit, const QString &title);

    /** @brief 启用或禁用静态 IP 输入字段。 */
    void setIpFieldsEnabled(bool enabled);

    /** @brief 向页面日志区追加文本。 */
    void appendLog(const QString &text);

    /** @brief 将当前输入保存到网络配置文件。 */
    bool saveInputsToConfigFile(const QString& cfgDir);

    /** @brief 执行 DHCP 或静态网络命令，并按需显示结果。 */
    bool applyNetworkConfig(bool showMessage);

    /** @return 在线功能或显式 Web 网络配置要求联网时返回 true。 */
    bool isNetworkFeatureEnabled() const;

    /** @brief 清除配置文件中的静态 IP 字段。 */
    bool clearNetworkConfigFileIp(const QString& cfgDir);

    /** @brief 将 DHCP 获得的运行时地址同步回配置。 */
    void syncDhcpRuntimeStateToConfig();

    /** @brief 按重试节流请求 DHCP 续租或强制重启客户端。 */
    void requestAutoDhcpRenew(const QString &reason, bool forceRestart = false);
    /** @brief 串行执行后台网络任务，仅在主线程回传仍有效的结果。 */
    void runNetworkTask(std::function<void()> work, std::function<void()> completed);
    /** @brief 后台读取系统网络参数，再回显到当前页面。 */
    void refreshUiFromSystem(const QString &iface, std::function<void()> completed = {});
    /** @brief 使用后台采集结果执行原 DHCP 状态同步和重试判断。 */
    void applyDhcpRuntimeState(const QString &iface, const NetworkRuntimeState &state);
    /** @brief 后台执行启动/手动申请 DHCP 的命令及原有地址等待。 */
    void applyDhcpNetworkConfig(const QString &iface, bool showMessage);

private:
    Ui::NetworkPage *ui; /**< Qt Designer 页面对象。 */

    QString configDir_ = Rk3566Platform::netConfigDir();
    QString configFileName_ = "net_cfg.ini";
    QString ifaceName_ = "eth0";

    FeatureSettings featureSettings_;

    QTimer* dhcpSyncTimer_ = nullptr;
    QThread *networkThread_ = nullptr;
    QObject *networkWorker_ = nullptr;
    bool networkTaskPending_ = false;
    bool networkRefreshPending_ = false;
    bool webNetworkEnabled_ = false; /**< 显式 Web 设置允许联网，不改变 MQTT 功能开关。 */
    QString webNetworkToken_; /**< 正在本页共享网络线程执行的 Web 操作。 */
    std::function<void()> deferredNetworkAction_;
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
