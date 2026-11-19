/**
 * @file ota_service.h
 * @brief MQTT 6.18 OTA 调度、异步脚本调用与跨重启结果重放。
 * @author Du lin
 * @date 2026-11-18
 */
#pragma once
#include <QObject>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <functional>

/** @brief 单设备 OTA 协调器；全部方法在所属 QObject 线程执行，不在该线程下载或刷写。
 * @details 准备进程通过 stdin 接收一次性凭据，安装交给独立 systemd 单元。
 *          结果保留在共享状态中，连接恢复后按 result→version 顺序重放；本地排队不是平台确认。
 */
class OtaService final : public QObject {
    Q_OBJECT
public:
    /** @brief 可测试的执行环境；生产环境使用默认的固定设备路径。 */
    struct Environment {
        QString script = QStringLiteral("/usr/local/sbin/update_ab_system.sh"); /**< 唯一升级执行器。 */
        QString stateRoot = QStringLiteral("/userdata/var/lib/face-media/ota"); /**< 共享持久状态。 */
        QString versionFile = QStringLiteral("/etc/face-media/firmware-version"); /**< 当前 rootfs 真实版本。 */
        QString systemdRun = QStringLiteral("systemd-run"); /**< 启动独立安装单元。 */
    };
    /** @brief 创建定时轮询器；publisher 只提交现有 IPC 队列，不视为送达确认。
     * @param publisher 线程内调用的 IPC 发布函数，返回是否受理。
     * @param parent Qt 父对象，拥有本模块。
     */
    explicit OtaService(std::function<bool(const QJsonObject &)> publisher, QObject *parent = nullptr);
    /** @brief 注入独立测试目录/工具；生产调用使用上一重载。
     * @param publisher IPC 发布函数。
     * @param environment 测试执行环境，不从 MQTT 或配置加载。
     * @param parent Qt 父对象。
     */
    OtaService(std::function<bool(const QJsonObject &)> publisher, const Environment &environment, QObject *parent = nullptr);
    /** @brief 停止准备进程并释放凭据；已由 systemd 接管的安装继续执行。 */
    ~OtaService() override;
    /** @brief 配置权威主题身份；空值停用 OTA 消息接收，不中断刷写。
     * @param deviceId 从现有 ycLinux event 精确主题取得的设备段。
     */
    void configure(const QString &deviceId);
    /** @brief 同步 Broker 在线状态，重连后重放持久结果。
     * @param online true 表示已连接 Broker。
     */
    void setOnline(bool online);
    /** @brief 处理三种 OTA 下行，其他业务返回 false 以继续原路由。
     * @param topic 已归一化订阅主题。
     * @param payload 已解密对象，taskId 已由后台解析器保留精度。
     * @return true 是 OTA method（错误身份也消费丢弃）；false 为其他协议。
     */
    bool handle(const QString &topic, const QJsonObject &payload);
    /** @return 真实运行版本；缺失时为空，不使用目标版本代替。 */
    QString runningVersion() const;
signals:
    /** @brief 向 GUI 线程发送安全进度/结果文本，不输出到应用日志。
     * @param text 阶段或结果提示，无 FTP 凭据。
     * @param percent 整体百分比 0..100。
     * @param terminal true 表示可关闭的结果提示。
     * @param success true 表示新固件通过健康确认。
     */
    void notice(const QString &text, int percent, bool terminal, bool success);
private:
    /** @brief 读取共享状态并驱动独立安装、进度和有序结果重放。 */
    void poll();
    /** @brief 发送单个业务信封，taskId 作为十进制 long 数值上行。
     * @param method 6.18 上行 method。
     * @param data 已净化的业务数据。
     * @return 当前在线且 IPC 受理时为 true；不代表平台已收到。
     */
    bool publish(const QString &method, const QJsonObject &data);
    /** @brief 读取固定任务根下的 JSON，并保护 taskId long 精度。
     * @param path 内部构造的本地路径。
     * @return 对象；缺失、过大或无效返回空对象。
     */
    QJsonObject read(const QString &path) const;
    /** @brief 取得受控任务文件路径。
     * @param id 已验证的十进制任务号。
     * @return state.json 绝对路径。
     */
    QString taskPath(const QString &id) const;
    /** @brief 启动异步准备脚本；FTP 凭据只写到子进程 stdin。
     * @param data 经过平台信封身份检查的请求。
     */
    void prepare(const QJsonObject &data);
    /** @brief 持久化准备进程无法生成状态时的终态，供重连重放。
     * @param code 6.18 设备错误码。
     * @param message 已净化的错误文本。
     */
    void fail(int code, const QString &message);
    /** @brief 平台查询应答真实版本，可回显任务号。
     * @param id 空为批量盘点，非空为任务查询。
     */
    void reportVersion(const QString &id);
    /** @brief 发布阶段变化/至少5%进度，并更新 GUI。
     * @param state 已净化状态或准备进程事件。
     */
    void progress(const QJsonObject &state);
    /** @brief 根据持久终态重放 result 后再发真实 version，不删除 outbox。
     * @param state 持久终态。
     */
    void replay(const QJsonObject &state);
    std::function<bool(const QJsonObject &)> publisher_; /**< 借用现有 IPC 的提交函数。 */
    Environment environment_; /**< 构造时固定，平台不可覆盖。 */
    QTimer timer_; /**< 所属线程每2秒轮询共享状态。 */
    QProcess *prepare_ = nullptr; /**< 自有准备进程，安装不使用它。 */
    QProcess *launcher_ = nullptr; /**< 自有短时 systemd-run 进程。 */
    QJsonObject request_; /**< 净化后的当前请求，不含 FTP 对象。 */
    QString device_, currentId_, stage_, terminalToken_, launchToken_; /**< 主题身份及幂等标记。 */
    QJsonObject pendingRetry_; /**< 同任务凭据刷新后的最新请求，仅驻留内存，旧准备进程退出后接续。 */
    QByteArray credentialDigest_; /**< 本次 FTP 参数摘要，仅用于识别重复通知，不落盘。 */
    QJsonObject lastError_; /**< 执行器最近的净化失败事件，预检失败时也保留协议错误码。 */
    QByteArray output_; /**< 有界 JSON 行缓冲，不包含请求正文。 */
    int percent_ = -10; /**< 最后已报告整体进度。 */
    bool online_ = false, prepared_ = false, stopping_ = false; /**< Broker 在线、准备完成和析构门。 */
    qint64 lastReplayMs_ = 0; /**< 单调毫秒，终态每30秒重放；未收到平台级确认不清除。 */
};
