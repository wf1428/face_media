/**
 * @file mqttipcclient.cpp
 * @brief 独立IPC事件循环、有界增量组帧、后台业务编解码与线程安全代理。
 * @author Du lin
 * @date 2026-07-25
 * @note 最近修改：2026-11-04；修改人：Du lin；普通/高清图片共用2项额外准入预算，后台完整写入/取消时归还，拒绝不占预算；音频紧急优先、原有普通字节节流/配置屏障/协议不变。
 */
#include "mqttipcclient.h"
#include "mqttpacketparser.h"
#include "common/fixed_mailbox.h"
#include <array>
#include <cstring>
#include <QThread>
#include <QThreadPool>
#include <QRunnable>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QPointer>
#include <atomic>
#include <deque>
#include <mutex>
#include <functional>
#include <algorithm>
#include <vector>

namespace {
constexpr qint64 MaxBytes=64LL*1024*1024; /**< 原接收上限，亦作为待业务消费的总字节预算。 */
constexpr int MaxNormal=64,MaxUrgent=16,RealtimeLimit=4096; /**< 普通/紧急提交槽数及普通大帧统计阈值（不是拒绝上限）。 */
/** @brief 不访问QObject的短生命周期工作任务，由线程池释放。 */
class Task final:public QRunnable {
public:
    /** @brief 保存纯值任务；对象地址由共享生命周期门保护。
     * @param[in] action 后台动作，不访问UI。
     */
    explicit Task(std::function<void()> action):action_(std::move(action)) {}
    /** @brief 在线程池执行一次动作，不等待UI。 */
    void run() override { action_(); }
private:
    std::function<void()> action_; /**< 自有值捕获，任务结束释放。 */
};
/** @brief 实时单行自有固定值；只跨音频调度/IPC线程，不含动态内存。 */
struct RealtimeLine {
    std::array<char,2048> bytes{};int length=0; /**< 自有完整JSON行及有效字节数1..2048，包含换行。 */
    quint64 connection=0; /**< 接受时连接代次；重连后旧值只释放预算，不写socket。 */
};
/** @brief 在共享16槽预算内有界预留；与普通紧急信令共用上限。
 * @param[in,out] count 共享已预留槽数，成功以CAS加1，拒绝不增计数。
 * @return true预留成功；false已满或有界竞争失败，不等待其他生产者。
 */
bool reserveUrgent(std::atomic_int &count) {
    auto value=count.load();for(int attempt=0;attempt<MaxUrgent+2 && value<MaxUrgent;++attempt)
        if(count.compare_exchange_strong(value,value+1)) return true;
    return false;
}
/** @brief 一次有界提交，正文仅隐式共享；编码前不在UI遍历JSON。 */
struct Request {
    QJsonObject json;QByteArray raw,encoded; /**< 自有值；encoded仅后台写，原请求正文不可变。 */
    bool isRaw=false,urgent=false; /**< 原始模式及媒体/信令优先级。 */
    bool snapshot=false; /**< true占用2项图片预算，完整写入或取消后归还；不改变线上信封。 */
    bool configBarrier=false; /**< set_config未发送前，后续普通apply等不能越过它；媒体仍走优先队列。 */
    qint64 queuedAtMs=0; /**< IPC线程接收请求的单调毫秒，供队列年龄诊断。 */
    quint64 connection=0; /**< 提交时连接代次，reset后迟到发布不得发往新连接。 */
    quint64 id=0; /**< 提交序号，用于后台编码结果定位，不依赖可变队首。 */
    qint64 cost=0; /**< 紧急队列估计字节；非紧急为0，不计入音频背压。 */
};
}
/** @brief 固定槽跨线程提交与原子预算；生命周期锁不进入发送/预留路径。 */
struct MqttIpcShared {
    std::mutex mutex; /**< 只保护后台任务结果投递与QObject生命周期；普通/实时提交均不获取。 */
    FixedMailbox<Request,MaxNormal+MaxUrgent> requests; /**< 普通/小信令固定交接槽，动态正文只隐式共享，IPC消费时释放。 */
    FixedMailbox<RealtimeLine,MaxUrgent> realtimeLines; /**< 固定实时行，后台消费；普通JSON不占用这些存储。 */
    QObject *target=nullptr;std::atomic_bool closing{false};std::atomic<quint64> serial{0}; /**< target仅在mutex下检查/投递；closing后拒绝新提交。 */
    std::atomic_int snapshots{0}; /**< 图片已受理数，包含待编码/待发/写入中，最多2项。 */
    std::atomic_int state{int(QLocalSocket::UnconnectedState)},normal{0},urgent{0},deferred{0}; /**< 连接状态及已接受请求槽数。 */
    std::atomic<qint64> wireBytes{0},urgentBytes{0},rawBytes{0}; /**< 正在写的帧/Qt待写、紧急估计及普通原始排队字节。 */
    std::atomic<quint64> connection{0},reservation{0}; /**< 连接及媒体预留代次，0无优先；release以CAS拒绝旧代次，不接触生命周期锁。 */
};
namespace {
/** @brief 在短生命周期锁内投递结果；对象析构先清除target，任务从不等待UI。
 * @param[in] shared 共享门，任务持有直到结束。
 * @param[in] result 在目标IPC线程执行的动作。
 */
void postResult(const std::shared_ptr<MqttIpcShared> &shared,std::function<void()> result)
{
    std::lock_guard<std::mutex> lock(shared->mutex);
    if(shared->target && !shared->closing) QMetaObject::invokeMethod(shared->target,std::move(result),Qt::QueuedConnection);
}
}
/** @brief 套接字唯一所有者；所有成员除shared外只在IPC线程访问。 */
class MqttIpcWorker final:public QObject {
    Q_OBJECT
public:
    /** @brief 保存跨线程门；套接字延迟至initialize创建。
     * @param[in] shared 代理共享状态。
     */
    explicit MqttIpcWorker(std::shared_ptr<MqttIpcShared> shared):shared_(std::move(shared)) {}
    /** @brief 取消任务投递门，不等待线程池；其迟到结果自动丢弃。 */
    ~MqttIpcWorker() override { std::lock_guard<std::mutex> lock(shared_->mutex);shared_->target=nullptr; }
    /** @brief 仅在IPC线程创建套接字、状态信号及诊断定时器。 */
    void initialize() {
        clock_.start();normalTimer_=new QTimer(this);normalTimer_->setSingleShot(true);
        connect(normalTimer_,&QTimer::timeout,this,&MqttIpcWorker::pump);
        socket_=new QLocalSocket(this);socket_->setReadBufferSize(256*1024);
        connect(socket_,&QLocalSocket::stateChanged,this,[this](QLocalSocket::LocalSocketState state) { shared_->state.store(int(state)); });
        connect(socket_,&QLocalSocket::connected,this,[this]() { emit connected();pump(); });
        connect(socket_,&QLocalSocket::disconnected,this,[this]() { reset();emit disconnected(); });
        connect(socket_,QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::error),this,[this](QLocalSocket::LocalSocketError) { emit error(socket_->errorString()); });
        connect(socket_,&QLocalSocket::readyRead,this,&MqttIpcWorker::scheduleRead);
        connect(socket_,&QLocalSocket::bytesWritten,this,[this](qint64) { schedulePump(); });
        auto *timer=new QTimer(this);timer->setInterval(3000);
        connect(timer,&QTimer::timeout,this,[this]() {
            if((shared_->reservation.load()!=0)) emit log(QStringLiteral("[mqtt-io] thread=%1 normal=%2 urgent=%3 deferred=%4 wireBytes=%5 rxPendingBytes=%6 businessBusy=%7")
                .arg(quintptr(QThread::currentThreadId()),0,16).arg(shared_->normal.load()).arg(shared_->urgent.load())
                .arg(shared_->deferred.load()).arg(shared_->wireBytes.load()).arg(rxPending_).arg(busy_)
                +QStringLiteral(" normalOldestMs=%1 normalSent=%2 normalWireMaxMs=%3 normalFrameMaxBytes=%4")
                .arg(normal_.empty() ? 0 : clock_.elapsed()-normal_.front().queuedAtMs).arg(normalSent_).arg(normalWireMaxMs_).arg(normalFrameMaxBytes_));
        });timer->start();
        auto *realtimeTimer=new QTimer(this);realtimeTimer->setTimerType(Qt::PreciseTimer);realtimeTimer->setInterval(5);
        connect(realtimeTimer,&QTimer::timeout,this,[this] {
            const bool priority=shared_->reservation.load()!=0;
            if(!shared_->realtimeLines.empty() || !shared_->requests.empty() || priority!=polledRealtime_) { polledRealtime_=priority;pump(); }
        });realtimeTimer->start();
        emit log(QStringLiteral("[mqtt-io] ready thread=%1 chunkBytes=32768 mediaBypass=1 parser=worker-task").arg(quintptr(QThread::currentThreadId()),0,16));
    }
    /** @brief 原连接行为在IPC线程执行；新路径取消旧连接及其未提交数据。
     * @param[in] path 原服务路径。
     */
    void connectTo(const QString &path) {
        if(!socket_) initialize();
        if(path.trimmed().isEmpty()) { emit error(QStringLiteral("socket 路径为空"));return; }
        if(socket_->state()==QLocalSocket::ConnectedState) { emit connected();return; }
        if(socket_->state()==QLocalSocket::ConnectingState) { emit log(QStringLiteral("IPC 正在连接中…"));return; }
        reset();socket_->abort();emit log(QStringLiteral("连接 mqttd IPC：%1").arg(path.trimmed()));socket_->connectToServer(path.trimmed());
    }
    /** @brief 立即取消本地未发数据，不等待断开完成。 */
    void disconnectNow() { reset();if(socket_) socket_->abort(); }
    /** @brief 代理退出时取消生命周期门和套接字，然后结束本事件循环。 */
    void shutdown() {
        { std::lock_guard<std::mutex> lock(shared_->mutex);shared_->closing=true;shared_->target=nullptr; }
        disconnectNow();QThread::currentThread()->quit();
    }
    /** @brief 业务接收者已返回，释放一帧预算并继续后台解析。
     * @param[in] connection 发出该业务值时的连接代次。
     * @param[in] bytes 原完整帧长度，字节。
     */
    void consumed(quint64 connection,qint64 bytes) {
        if(connection!=shared_->connection.load()) return;
        rxPending_=qMax<qint64>(0,rxPending_-bytes);uiPending_=false;startTask();
    }
    /** @brief IPC轮询/写完成后有界接收固定槽、安排编码并推进单帧；不在此函数解析大JSON。 */
    void pump() {
        pumpPending_=false;
        Request submitted;
        for(int n=0;n<MaxNormal+MaxUrgent && shared_->requests.pop(submitted);++n) {
            if(submitted.connection!=shared_->connection.load()) { release(submitted);continue; }
            submitted.queuedAtMs=clock_.elapsed();
            (submitted.urgent ? urgent_ : normal_).push_back(std::move(submitted));
        }
        RealtimeLine line;
        for(int n=0;n<MaxUrgent && shared_->realtimeLines.pop(line);++n) {
            if(line.connection!=shared_->connection.load() || !socket_ || socket_->state()!=QLocalSocket::ConnectedState) {
                shared_->urgent.fetch_sub(1);shared_->urgentBytes.fetch_sub(1536);continue;
            }
            Request request;request.urgent=true;request.cost=1536;request.queuedAtMs=clock_.elapsed();
            request.encoded=QByteArray(line.bytes.data(),line.length);urgent_.push_back(std::move(request));
        }
        if(!socket_ || socket_->state()!=QLocalSocket::ConnectedState) return;
        // 媒体请求有协议/信用约束，最大数KB，可在IPC线程编码；普通JSON一律由工作任务编码。
        if(!urgent_.empty() && urgent_.front().encoded.isEmpty()) {
            auto &request=urgent_.front();request.encoded=request.isRaw ? request.raw : QJsonDocument(request.json).toJson(QJsonDocument::Compact);
            if(!request.encoded.endsWith('\n')) request.encoded.append('\n');
        }
        startTask();
        shared_->deferred.store((shared_->reservation.load()!=0) ? int(std::count_if(normal_.begin(),normal_.end(),
            [](const Request &r) { return !r.encoded.isEmpty() && r.encoded.size()>RealtimeLimit; })) : 0);
        if(wire_.isEmpty()) {
            if(!urgent_.empty() && !urgent_.front().encoded.isEmpty()) { current_=std::move(urgent_.front());urgent_.pop_front(); }
            else {
                auto next=normal_.begin();
                if((shared_->reservation.load()!=0) && clock_.elapsed()<nextNormalMs_) {
                    next=normal_.end();
                    for(auto item=normal_.begin();item!=normal_.end();++item) {
                        if(!item->encoded.isEmpty() && item->encoded.size()<=RealtimeLimit) { next=item;break; }
                        if(item->configBarrier) break; // 沿用配置依赖屏障，不能先apply再发送配置。
                    }
                    if(next==normal_.end() && !normal_.empty() && !normalTimer_->isActive()) {
                        const auto remaining=qMax<qint64>(1,nextNormalMs_-clock_.elapsed());
                        normalTimer_->start(int(qMin<qint64>(2147483647,remaining)));
                    }
                }
                if(next==normal_.end() || next->encoded.isEmpty()) { updatePending();return; }
                current_=std::move(*next);normal_.erase(next);
            }
            wire_=current_.encoded;wireOffset_=0;wireStartMs_=clock_.elapsed();
            if(!current_.urgent) {
                normalFrameMaxBytes_=qMax<qint64>(normalFrameMaxBytes_,wire_.size());
                // 大帧完整发出再计字节债务，至少让出一个20ms周期；小业务不消耗该预算。
                // 单IPC单行无法在帧内抢占；该预算限制连续大帧，不能承诺任意巨帧零阻塞。
                if(wire_.size()>RealtimeLimit) nextNormalMs_=(shared_->reservation.load()!=0) ? clock_.elapsed()+qMax<qint64>(20,(qint64(wire_.size())*1000+262143)/262144) : 0;
            }
        }
        // 单行不能与音频交错；16KiB一个写片，Qt待写积压时由bytesWritten继续。
        if(socket_->bytesToWrite()<=2048 && wireOffset_<wire_.size()) {
            const auto written=socket_->write(wire_.constData()+wireOffset_,qMin<qint64>(16384,wire_.size()-wireOffset_));
            if(written<0) { emit error(QStringLiteral("IPC write 失败：%1").arg(socket_->errorString()));disconnectNow();return; }
            wireOffset_+=written;
            if(wireOffset_==wire_.size()) { if(!current_.urgent) { ++normalSent_;normalWireMaxMs_=qMax(normalWireMaxMs_,clock_.elapsed()-wireStartMs_); }release(current_);current_={};wire_.clear();wireOffset_=0;schedulePump(); }
            else if(written>0 && socket_->bytesToWrite()<=2048) schedulePump();
        }
        updatePending();
    }
signals:
    /** @brief 线程内连接通知；代理仅转发。 */
    void connected();
    /** @brief 线程内断开通知；媒体接收者无需等待UI。 */
    void disconnected();
    /** @brief 原错误与有界拒绝诊断。
     * @param[in] text 小型错误。
     */
    void error(const QString &text);
    /** @brief 线程与队列诊断。
     * @param[in] text 小型文本。
     */
    void log(const QString &text);
    /** @brief 小媒体信封直达音频线程。
     * @param[in] object 校验为媒体类型的小型JSON。
     */
    void media(const QJsonObject &object);
    /** @brief 完成后台解析的单个业务值；代理消费完成后才放行下一业务值。
     * @param[in] object 标准化信封。
     * @param[in] display 原RX摘要。
     * @param[in] errorText 解析错误。
     * @param[in] raw 原始业务帧，兼容观察者使用。
     * @param[in] connection 连接代次，拒绝重连前旧结果。
     */
    void business(const QJsonObject &object,const QString &display,const QString &errorText,const QByteArray &raw,quint64 connection);
private:
    /** @brief 将一个请求的有界槽位归还，正文引用在锁外释放。
     * @param[in] request 已提交或已取消的请求。
     */
    void release(const Request &request) {
        if(request.snapshot) shared_->snapshots.fetch_sub(1);
        if(request.urgent) { shared_->urgent.fetch_sub(1);shared_->urgentBytes.fetch_sub(request.cost); }
        else { shared_->normal.fetch_sub(1);if(request.isRaw) shared_->rawBytes.fetch_sub(request.raw.size()); }
    }
    /** @brief 取消连接代次、收发队列和异步任务结果；不重置业务协议。 */
    void reset() {
        if(normalTimer_) normalTimer_->stop();
        nextNormalMs_=0;normalSent_=0;normalWireMaxMs_=normalFrameMaxBytes_=0;
        shared_->connection.fetch_add(1);busy_=uiPending_=false;rxPending_=0;rx_.clear();parts_.clear();partBytes_=0;started_=logLine_=haveBrace_=inString_=escape_=false;depth_=0;
        Request cancelled;
        // 限次清理，竞态迟到值仍由connection代次在下一次pump拒绝；不重置正在Writing的槽。
        for(int n=0;n<MaxNormal+MaxUrgent && shared_->requests.pop(cancelled);++n) release(cancelled);
        RealtimeLine line;for(int n=0;n<MaxUrgent && shared_->realtimeLines.pop(line);++n) { shared_->urgent.fetch_sub(1);shared_->urgentBytes.fetch_sub(1536); }
        for(const auto &r:normal_) release(r);
        for(const auto &r:urgent_) release(r);
        normal_.clear();urgent_.clear();
        if(!wire_.isEmpty()) release(current_);
        current_={};wire_.clear();wireOffset_=0;shared_->deferred=0;updatePending();
    }
    /** @brief 发布即时音频背压快照，延后的普通帧不挤占媒体信用。 */
    void updatePending() { shared_->wireBytes.store((socket_ ? socket_->bytesToWrite() : 0)+qMax<qint64>(0,wire_.size()-wireOffset_)); }
    /** @brief 合并发送唤醒，避免逐帧积累无界Qt事件。 */
    void schedulePump() { if(!pumpPending_) { pumpPending_=true;QTimer::singleShot(0,this,&MqttIpcWorker::pump); } }
    /** @brief 合并接收唤醒，每次最多读取并扫描32KiB。 */
    void scheduleRead() { if(!readPending_) { readPending_=true;QTimer::singleShot(0,this,&MqttIpcWorker::readChunk); } }
    /** @brief 增量扫描字符串转义与大括号；不重扫半帧，不在IPC线程组装大正文。 */
    void readChunk() {
        readPending_=false;if(!socket_ || socket_->state()!=QLocalSocket::ConnectedState) return;
        const auto chunk=socket_->read(32768);int begin=0;
        for(int i=0;i<chunk.size();++i) {
            const char c=chunk[i];
            if(!started_) { if(c=='\r'||c=='\n') { begin=i+1;continue; }started_=true;logLine_=c=='['; }
            bool complete=false;
            if(logLine_) complete=c=='\n';
            else if(!haveBrace_) { if(c=='{') { haveBrace_=true;depth_=1; } }
            else if(escape_) escape_=false;
            else if(inString_ && c=='\\') escape_=true;
            else if(c=='"') inString_=!inString_;
            else if(!inString_ && c=='{') ++depth_;
            else if(!inString_ && c=='}') complete=--depth_==0;
            if(complete) {
                parts_.push_back(chunk.mid(begin,i-begin+1));partBytes_+=i-begin+1;
                if(!acceptFrame()) return;
                begin=i+1;
                started_=logLine_=haveBrace_=inString_=escape_=false;depth_=0;
            }
        }
        if(started_ && begin<chunk.size()) { parts_.push_back(chunk.mid(begin));partBytes_+=chunk.size()-begin; }
        if(partBytes_>MaxBytes) { emit error(QStringLiteral("IPC单帧超过64MiB，关闭连接防止积压"));disconnectNow();return; }
        if(socket_->bytesAvailable()>0) scheduleRead();
    }
    /** @brief 接受完整帧；小媒体直接解析，普通业务把分片交给有界任务队列。
     * @return false达到有界限制并关闭连接，true已接受或转发。
     */
    bool acceptFrame() {
        if(partBytes_<=4096) {
            QByteArray small;small.reserve(int(partBytes_));for(const auto &p:parts_) small.append(p);
            const auto document=QJsonDocument::fromJson(small);const auto object=document.object();const auto type=object.value("type").toString();
            if(type=="audio" || type=="call_ready" || type=="call_sent") { emit media(object);parts_.clear();partBytes_=0;return true; }
        }
        if(partBytes_>MaxBytes || rxPending_+partBytes_>MaxBytes || rx_.size()>=256) {
            emit error(QStringLiteral("IPC业务接收达到64MiB/256帧预算，关闭连接防止积压"));disconnectNow();return false;
        }
        rxPending_+=partBytes_;rx_.push_back({std::move(parts_),partBytes_});parts_.clear();partBytes_=0;startTask();return true;
    }
    /** @brief 安排至多一个业务编解码任务；IO/media始终不等待该任务或UI业务消费。 */
    void startTask() {
        if(busy_) return;
        const auto epoch=shared_->connection.load();const auto shared=shared_;auto *self=this;
        if(!rx_.empty() && !uiPending_) {
            auto frame=std::move(rx_.front());rx_.pop_front();busy_=true;uiPending_=true;
            QThreadPool::globalInstance()->start(new Task([shared,self,epoch,frame=std::move(frame)]() {
                if(epoch!=shared->connection.load()) return; // 未开始的旧任务不再解码大正文。
                QByteArray raw;raw.reserve(int(frame.second));for(const auto &p:frame.first) raw.append(p);
                const auto packet=parseMqttPacket(raw);
                postResult(shared,[self,epoch,packet,raw]() {
                    if(epoch!=self->shared_->connection.load()) return;
                    self->busy_=false;emit self->business(packet.object,packet.display,packet.error,raw,epoch);self->startTask();
                });
            }));return;
        }
        const auto pending=std::find_if(normal_.begin(),normal_.end(),[](const Request &r) { return r.encoded.isEmpty(); });
        if(pending!=normal_.end()) {
            const auto request=*pending;busy_=true;
            QThreadPool::globalInstance()->start(new Task([shared,self,epoch,request]() {
                if(epoch!=shared->connection.load()) return;
                QByteArray encoded=request.isRaw ? request.raw : QJsonDocument(request.json).toJson(QJsonDocument::Compact);
                if(!encoded.endsWith('\n')) encoded.append('\n');
                postResult(shared,[self,epoch,encoded,id=request.id]() {
                    if(epoch!=self->shared_->connection.load()) return;
                    self->busy_=false;
                    const auto item=std::find_if(self->normal_.begin(),self->normal_.end(),[id](const Request &r) { return r.id==id; });
                    if(item==self->normal_.end()) return;
                    qint64 prepared=0;for(const auto &r:self->normal_) prepared+=r.encoded.size();
                    if(encoded.size()>MaxBytes || prepared+encoded.size()>MaxBytes) {
                        self->release(*item);self->normal_.erase(item);emit self->error(QStringLiteral("IPC业务编码达到64MiB预算，已拒绝该请求"));
                    } else item->encoded=encoded;
                    self->schedulePump();self->startTask();
                });
            }));
        }
    }
    QElapsedTimer clock_;QTimer *normalTimer_=nullptr; /**< 本IPC线程单调时钟及唯一普通业务预算唤醒，无等待UI。 */
    qint64 nextNormalMs_=0,wireStartMs_=0,normalWireMaxMs_=0,normalFrameMaxBytes_=0;quint64 normalSent_=0; /**< 普通帧准入时刻及单帧写入/大小极值，毫秒/字节；reset清零。 */
    std::shared_ptr<MqttIpcShared> shared_; /**< 唯一跨线程状态，生命周期门保护迟到任务投递。 */
    QLocalSocket *socket_=nullptr; /**< 本线程拥有，退出时由QObject释放；绝不交UI访问。 */
    std::deque<Request> normal_,urgent_;Request current_;QByteArray wire_;qint64 wireOffset_=0; /**< 有界优先级队列及不可交错当前帧；媒体优先，大帧依相对顺序按预算持续发送，小帧可越过等待大帧但不越过配置屏障。 */
    std::deque<std::pair<std::vector<QByteArray>,qint64>> rx_;std::vector<QByteArray> parts_;qint64 partBytes_=0,rxPending_=0; /**< 大帧分片及业务字节预算。 */
    bool started_=false,logLine_=false,haveBrace_=false,inString_=false,escape_=false;int depth_=0; /**< 跨32KiB读取的增量扫描状态。 */
    bool polledRealtime_=false; /**< IPC线程上次轮询的优先状态；状态改变推动原普通业务准入，不从提交线程投递事件。 */
    bool pumpPending_=false,readPending_=false,busy_=false,uiPending_=false; /**< 合并唤醒、单任务和单UI业务消费门。 */
};

MqttIpcClient::MqttIpcClient(QObject *parent):QObject(parent),shared_(std::make_shared<MqttIpcShared>())
{
    ioThread_=new QThread;ioThread_->setObjectName(QStringLiteral("mqtt.ipc.io"));worker_=new MqttIpcWorker(shared_);worker_->moveToThread(ioThread_);
    { std::lock_guard<std::mutex> lock(shared_->mutex);shared_->target=worker_; }
    connect(ioThread_,&QThread::started,worker_,&MqttIpcWorker::initialize);
    connect(ioThread_,&QThread::finished,worker_,&QObject::deleteLater);
    connect(worker_,&MqttIpcWorker::connected,this,&MqttIpcClient::connected,Qt::DirectConnection);
    connect(worker_,&MqttIpcWorker::disconnected,this,&MqttIpcClient::disconnected,Qt::DirectConnection);
    connect(worker_,&MqttIpcWorker::media,this,&MqttIpcClient::mediaFrameReceived,Qt::DirectConnection);
    connect(worker_,&MqttIpcWorker::error,this,&MqttIpcClient::errorOccurred);
    connect(worker_,&MqttIpcWorker::log,this,&MqttIpcClient::logMessage);
    connect(worker_,&MqttIpcWorker::business,this,[this](const QJsonObject &object,const QString &display,const QString &error,const QByteArray &raw,quint64 epoch) {
        if(epoch!=shared_->connection.load()) return;
        QPointer<MqttIpcClient> alive(this);emit packetReceived(object,display,error);
        if(!alive) return;
        emit frameReceived(raw);
        if(!alive) return;
        auto *worker=worker_;QMetaObject::invokeMethod(worker,[worker,epoch,bytes=raw.size()]() { worker->consumed(epoch,bytes); },Qt::QueuedConnection);
    });
    ioThread_->start();
}
MqttIpcClient::~MqttIpcClient()
{
    { std::lock_guard<std::mutex> lock(shared_->mutex);shared_->closing=true; }
    auto *worker=worker_;QMetaObject::invokeMethod(worker,[worker]() { worker->shutdown(); },Qt::QueuedConnection);
    ioThread_->wait();delete ioThread_;
}
void MqttIpcClient::connectToServer(const QString &path) { auto *worker=worker_;QMetaObject::invokeMethod(worker,[worker,path]() { worker->connectTo(path); },Qt::QueuedConnection); }
void MqttIpcClient::disconnectFromServer() { auto *worker=worker_;QMetaObject::invokeMethod(worker,[worker]() { worker->disconnectNow(); },Qt::QueuedConnection); }
bool MqttIpcClient::isConnected() const { return state()==QLocalSocket::ConnectedState; }
QLocalSocket::LocalSocketState MqttIpcClient::state() const { return QLocalSocket::LocalSocketState(shared_->state.load()); }
qint64 MqttIpcClient::pendingWriteBytes() const { return shared_->wireBytes.load()+shared_->urgentBytes.load(); }
int MqttIpcClient::deferredBusinessFrames() const { return shared_->deferred.load(); }
quint64 MqttIpcClient::reserveRealtime() {
    if(shared_->closing.load()) return 0;
    const auto reservation=shared_->serial.fetch_add(1)+1;shared_->reservation.store(reservation);return reservation;
}
void MqttIpcClient::releaseRealtime(quint64 reservation) {
    if(reservation) shared_->reservation.compare_exchange_strong(reservation,0);
}
bool MqttIpcClient::sendJsonLine(const QJsonObject &object) { return submit(object,{},false); }
bool MqttIpcClient::sendRawLine(const QByteArray &line) { return submit({},line,true); }
bool MqttIpcClient::sendSnapshotJsonLine(const QJsonObject &object) {return submit(object,{},false,true);}
bool MqttIpcClient::sendSnapshotRawLine(const QByteArray &line) {return submit({},line,true,true);}
int MqttIpcClient::pendingSnapshotPublishes() const {return shared_->snapshots.load();}
bool MqttIpcClient::sendRealtimeLine(const char *line,int length) {
    if(!line || length<1 || length>2048 || line[length-1]!='\n' || shared_->closing.load() || !isConnected()) return false;
    const auto connection=shared_->connection.load();
    if(!reserveUrgent(shared_->urgent)) return false;
    shared_->urgentBytes.fetch_add(1536);
    RealtimeLine value;value.length=length;value.connection=connection;std::memcpy(value.bytes.data(),line,size_t(length));
    if(shared_->realtimeLines.push(std::move(value))) return true;
    shared_->urgent.fetch_sub(1);shared_->urgentBytes.fetch_sub(1536);return false;
}
bool MqttIpcClient::submit(const QJsonObject &object,const QByteArray &line,bool raw,bool snapshot)
{
    if(!isConnected() || (raw && line.size()>MaxBytes)) return false;
    const auto command=object.value("cmd").toString();
    const bool urgent=!raw && (command=="publish_audio" || command=="audio_received" || command=="call_media" ||
        (command=="publish" && object.value("payload").toObject().value("method").toString().startsWith("call.")));
    Request request;request.json=object;request.raw=line;request.isRaw=raw;request.urgent=urgent;
    request.configBarrier=!raw && command=="set_config";request.cost=urgent ? 1536 : 0;
    request.connection=shared_->connection.load();
    if(shared_->closing.load() || !isConnected()) return false;
    request.snapshot=snapshot;
    if(snapshot) {
        auto images=shared_->snapshots.load();bool accepted=false;
        for(int n=0;n<4 && images<2;++n)if(shared_->snapshots.compare_exchange_strong(images,images+1)){accepted=true;break;}
        if(!accepted)return false;
    }
    const auto releaseImage=[&]{if(snapshot)shared_->snapshots.fetch_sub(1);};
    if(urgent) {
        if(!reserveUrgent(shared_->urgent)) {releaseImage();return false;}
        shared_->urgentBytes.fetch_add(request.cost);
    } else {
        auto count=shared_->normal.load();bool reserved=false;
        for(int n=0;n<MaxNormal+2 && count<MaxNormal;++n) if(shared_->normal.compare_exchange_strong(count,count+1)) { reserved=true;break; }
        if(!reserved) {releaseImage();return false;}
        if(raw) {
            auto bytes=shared_->rawBytes.load();bool accepted=false;
            for(int n=0;n<MaxNormal+2 && bytes+line.size()<=MaxBytes;++n)
                if(shared_->rawBytes.compare_exchange_strong(bytes,bytes+line.size())) { accepted=true;break; }
            if(!accepted) { shared_->normal.fetch_sub(1);releaseImage();return false; }
        }
    }
    request.id=shared_->serial.fetch_add(1)+1;
    if(shared_->requests.push(std::move(request))) return true;
    releaseImage();
    if(urgent) { shared_->urgent.fetch_sub(1);shared_->urgentBytes.fetch_sub(1536); }
    else { shared_->normal.fetch_sub(1);if(raw) shared_->rawBytes.fetch_sub(line.size()); }
    return false;
}
#include "mqttipcclient.moc"
