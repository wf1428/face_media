/**
 * @file CallGpioButton.h
 * @brief 用户态读取GPIO40高有效通话键，将消抖后的拨号/取消/挂断动作安全交给UI。
 * @details 文件访问仅在后台线程；可用性代次阻止旧按键在通话结束或重连后补发。
 * @author Du lin
 * @date 2026-10-30
 * @note 最近修改：2026-11-03；修改人：Du lin；明确会话中可再次按下取消/挂断，每次状态刷新均使旧代次失效；消抖与线程资源规则不变。
 */
#pragma once
#include <QThread>
#include <QString>
#include <atomic>
class QFile;

namespace VoiceCall {
/** @brief 单线程使用的高有效按键消抖器；启动、读失败和持续高电平均须先稳定释放。
 * @details 高、低均稳定 60ms 才改变状态；每个稳定低→高序列最多返回一个可用性代次。
 */
class CallButtonDebounce {
public:
    /** @brief 清除采样历史并取消武装；后续必须先看到稳定低电平。 */
    void reset();
    /** @brief 采样电平，并在一次有效按下时返回原可用性代次。
     * @param[in] high true 为高电平按下，false 为低电平释放。
     * @param[in] nowMs 单调递增的本地毫秒值，不能使用可回拨墙上时钟。
     * @param[in] eligibility bit0为允许本次通话按键动作；高位为UI业务状态代次。
     * @return 非零奇数表示按下；0 表示抖动、长按、未武装或按下期间可用性已变化。
     * @note 即使当前不可操作也消费高电平；状态改变后持续按住不会补发或自动挂断。
     */
    quint64 sample(bool high,qint64 nowMs,quint64 eligibility);
private:
    bool candidateKnown_=false; /**< true 表示已记录一段候选电平；reset 后为 false。 */
    bool candidateHigh_=false; /**< 当前候选电平，true 表示按下。 */
    bool stableKnown_=false; /**< true 表示已有消抖完成的稳定状态。 */
    bool stableHigh_=false; /**< 消抖完成的电平，true 表示按下。 */
    bool armed_=false; /**< 仅稳定低电平置 true；高电平和 reset 消费/清除武装。 */
    qint64 candidateSinceMs_=0; /**< 当前候选电平开始时刻，单调毫秒。 */
    quint64 candidateEligibility_=0; /**< 候选按下开始时的可用性代次，变化则丢弃该按下。 */
};
}

/** @brief 使用 sysfs 的 GPIO40 呼叫键线程；QObject 本身仍属于创建它的 UI 线程。
 * @details 后台只导出 GPIO40、设输入/非反相并读取 value；不设输出、不配置上下拉、
 *          不占用原 audio_key 驱动。20ms 采样，错误后 2s 重试；正常调用不等待后台。
 *          启动/恢复时高电平不触发，必须稳定释放；退出保留 GPIO 导出和输入状态。
 */
class CallGpioButton final : public QThread {
    Q_OBJECT
public:
    /** @brief 创建未启动、默认不可呼叫的 GPIO 读取线程，不执行文件访问。
     * @param[in] sysfsRoot GPIO sysfs 根目录；生产为 /sys/class/gpio，测试可用临时目录。
     * @param[in] parent UI 线程的所有者，通常为 CallManager。
     * @note 设置初始可用性后调用一次 start()；不支持运行中再次 start()。
     */
    explicit CallGpioButton(const QString &sysfsRoot=QStringLiteral("/sys/class/gpio"),QObject *parent=nullptr);
    /** @brief 退出时请求停止并等待线程关闭自己的文件；只在对象销毁时等待。 */
    ~CallGpioButton() override;
public slots:
    /** @brief UI线程刷新按键动作可用性；每次调用均推进代次，不访问GPIO。
     * @param[in] available true允许拨号或会话取消/挂断；false用于离线、结束中或等待迟到ACK。
     * @note 即使前后均true也须在业务状态变化时调用，避免旧拨号动作成为新会话的挂断。
     */
    void setCallAvailable(bool available);
    /** @brief 立即取消可用性并请求后台退出；不等待，GPIO 关闭发生在后台。 */
    void stopAsync();
signals:
    /** @brief UI线程核对代次后发出单次按下；接收端按当前状态选择拨号或取消/挂断。
     * @note 自动/直接连接到UI线程的状态感知入口；不直接连接无条件hangup，不模拟KEY3/KEY4。
     */
    void callRequested();
    /** @brief 后台配置、读取失败及恢复日志，最多在错误变化/恢复时输出。
     * @param[in] text 不含音频或密码的 GPIO40 诊断。
     */
    void logMessage(const QString &text);
private:
    /** @brief 后台通知候选按下，内部以排队连接切回 UI。
     * @param[in] eligibility 按下开始时的可用性代次；UI 再核对是否仍有效。
     */
    Q_SIGNAL void pressDetected(quint64 eligibility);
protected:
    /** @brief 后台配置输入并轮询；唯一拥有 value 文件，停止后在本线程关闭。 */
    void run() override;
private:
    /** @brief 后台导出/复用 GPIO40，设输入及 active_low=0，然后打开 value。
     * @param[out] value 成功时为只读无缓冲文件；失败时关闭。
     * @param[out] error 失败原因，成功时调用者不使用。
     * @return true 表示可以开始采样；false 在后台等待下次重试。
     */
    bool openInput(QFile &value,QString &error) const;
    const QString sysfsRoot_; /**< 创建时复制的根路径，后台只读。 */
    std::atomic<quint64> eligibility_{0}; /**< bit0 可用，高位代次；UI 写，后台/排队槽读。 */
};
