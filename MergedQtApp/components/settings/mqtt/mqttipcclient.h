/**
 * @file mqttipcclient.h
 * @brief 保留UI调用接口的线程安全IPC代理，实际套接字由独立收发线程拥有。
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-04；修改人：Du lin；图片上传增加独立2项预算，保留普通64/紧急16和音频优先；未受理时由截图门面延迟，不增加图片积压或改变单socket/协议。
 */
#pragma once
#include <QObject>
#include <QLocalSocket>
#include <QJsonObject>
#include <memory>
class QThread;
class MqttIpcWorker;
struct MqttIpcShared;
/** @brief UI所有的轻量代理；提交和只读快照可从音频线程调用，不跨线程操作套接字。
 * @details 只有应用退出/销毁代理时等待IPC线程结束；普通连接、收发和断线均不等待。
 *          保留单mqttd/单IPC，消息接受不表示代理或平台已收到。
 */
class MqttIpcClient : public QObject {
    Q_OBJECT
public:
    /** @brief 创建专用IPC线程，套接字在线程启动后创建。
     * @param[in] parent 原代理所有者，通常为UI中的MqttManager。
     */
    explicit MqttIpcClient(QObject *parent=nullptr);
    /** @brief 应用退出时停止IPC线程；不等待后台编解码任务，迟到结果通过共享生命周期门丢弃。 */
    ~MqttIpcClient() override;
    /** @brief 非阻塞请求连接；空路径沿用错误通知。
     * @param[in] path 原Unix域套接字路径或平台本地服务名。
     */
    void connectToServer(const QString &path);
    /** @brief 非阻塞请求断开，清除未提交消息，防止重连后回放旧媒体。 */
    void disconnectFromServer();
    /** @return IPC线程发布的当前连接状态快照。 */
    bool isConnected() const;
    /** @return 原QLocalSocket状态的原子快照。 */
    QLocalSocket::LocalSocketState state() const;
    /** @brief 有界固定槽提交JSON，不等待生命周期锁/分配队列节点/投递事件；大对象序列化在后台完成。
     * @param[in] object 只读值对象，队列通过Qt隐式共享持有，不访问UI。
     * @return true已接受；false断线、队列满、有界竞争失败或正在退出；后续编码超预算或写失败另发errorOccurred。
     */
    bool sendJsonLine(const QJsonObject &object);
    /** @brief 有界固定槽提交完整原始行，后台确保换行；不在UI编码或拷贝正文；IPC最多每5ms取一次已发布请求（实际调度迟到可能更长）。
     * @param[in] line 原始完整JSON或控制行，不允许多条消息交错写入。
     * @return true已接受；false断线、达到有界提交限制或有界竞争失败。
     */
    bool sendRawLine(const QByteArray &line);
    /** @brief 普通截图异步提交，共用2项图片预算。
     * @param[in] object 原publish信封，按隐式共享值接受。
     * @return true已受理；false沿用普通提交失败或图片预算满，调用方保留元数据重试。
     */
    bool sendSnapshotJsonLine(const QJsonObject &object);
    /** @brief 高清完整编码行异步提交，共用2项图片预算。
     * @param[in] line 自有完整IPC行，后台补充换行，原64MiB上限不变。
     * @return true已受理；false断线/普通预算满/图片预算满，调用方保留唯一结果等容量。
     */
    bool sendSnapshotRawLine(const QByteArray &line);
    /** @return 已受理但尚未完成本地IPC写入提交的图片请求数0..2；不代表mqttd解析、Broker确认或网络积压。 */
    int pendingSnapshotPublishes() const;
    /** @brief 提交完整实时JSON行，仅有界复制/CAS，不等待共享锁或投递Qt事件。
     * @param[in] line 调用期间有效的完整行，含末尾换行；内容须由可信音频封装器生成。
     * @param[in] length 1..2048字节，包含换行；不解析或修改协议。
     * @return true已接受，false断线/关闭/超长/紧急16槽满或有界竞争失败。
     * @note 由音频线程调用，IPC后台5ms取走；仍复用单socket及原紧急预算。超长身份可由调用者回退sendJsonLine。
     */
    bool sendRealtimeLine(const char *line,int length);
    /** @return 实际待写/正在写的帧及紧急队列估计字节，不含尚未开始的普通业务。 */
    qint64 pendingWriteBytes() const;
    /** @brief 立即预留通话优先；紧急队列优先，普通大帧按256KiB/s准入预算持续发送。
     * @details 大于4096B普通帧准入间隔至少20ms，按完整字节数计下一次大帧准入债务；不再等挂断。
     *          小帧可越过等待预算的大帧，但不能越过未发送set_config，保留配置/apply依赖。
     *          已开始的单行不能中断或交错，
     *          任意巨帧仍可能造成物理IPC队头阻塞，该接口不保证硬实时。
     * @return 非零预留代次；退出中返回0。新代次覆盖旧代次，旧停止不能解除新通话优先。
     */
    quint64 reserveRealtime();
    /** @brief 仅当前预留代次可解除通话优先；后台先提交停止媒体帧再调用。
     * @param[in] reservation reserveRealtime返回的代次；0及过时代次忽略。
     */
    void releaseRealtime(quint64 reservation);
    /** @return 已编码且仍在普通队列等待准入的>4096B帧数快照；不含尚待编码的请求，仅用于诊断/回归。 */
    int deferredBusinessFrames() const;
signals:
    /** @brief IPC线程确认连接；UI接收者自动排队，不能据此直接操作底层套接字。 */
    void connected();
    /** @brief IPC断线，媒体线程直接收到，UI接收者排队。 */
    void disconnected();
    /** @brief 兼容观察接口，仅普通业务原帧；生产业务应使用packetReceived避免UI再次解析。
     * @param[in] frame 普通业务完整原帧，不含逐帧音频/信用消息。
     */
    void frameReceived(const QByteArray &frame);
    /** @brief 业务值已在后台完成标准/旧格式及payload解析，UI只做原业务分派。
     * @param[in] object 标准化信封，payload若有效已是对象。
     * @param[in] display 小型原RX摘要。
     * @param[in] error 原兼容解析错误，成功为空。
     */
    void packetReceived(const QJsonObject &object,const QString &display,const QString &error);
    /** @brief IPC线程直接发出的小型媒体消息，连接到独立音频线程，完全绕过UI。
     * @param[in] object audio/call_ready/call_sent原信封。
     */
    void mediaFrameReceived(const QJsonObject &object);
    /** @brief 非阻塞提交或套接字错误，不含大包正文。
     * @param[in] err 诊断文本。
     */
    void errorOccurred(const QString &err);
    /** @brief 小型线程/连接诊断。
     * @param[in] msg 诊断文本。
     */
    void logMessage(const QString &msg);
private:
    /** @brief 固定槽接受请求，由IPC线程5ms轮询，不做序列化或事件投递。
     * @param[in] object JSON请求；原始行模式为空。
     * @param[in] line 原始行；JSON模式为空。
     * @param[in] raw true为原始行模式。
     * @param[in] snapshot true额外预留2项图片预算；false保留原普通/音频规则。
     * @return true已接受，false有界队列拒绝。
     */
    bool submit(const QJsonObject &object,const QByteArray &line,bool raw,bool snapshot=false);
    std::shared_ptr<MqttIpcShared> shared_; /**< 跨线程有界提交及原子快照，生命周期锁不进入提交，IPC5ms取槽。 */
    QThread *ioThread_=nullptr; /**< 代理所有的IPC事件循环；仅析构等待。 */
    MqttIpcWorker *worker_=nullptr; /**< IPC线程拥有并在退出时deleteLater，不从UI调用其设备方法。 */
};
