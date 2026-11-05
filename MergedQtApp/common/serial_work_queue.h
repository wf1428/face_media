/**
 * @file serial_work_queue.h
 * @brief 将后台任务与界面完成回调串行衔接，支持待执行任务合并及过期过滤。
 * @author Du lin
 * @date 2026-11-04
 * @note 最近修改：2026-11-04；修改人：Du lin；后台任务可关闭原慢任务输出，进度轮询不新增日志，串行与退出语义保持。
 */
#pragma once
#include <QObject>
#include <QThread>
#include <QPointer>
#include <QElapsedTimer>
#include <QDebug>
#include <deque>
#include <functional>
#include <utility>
#include <thread>
#include <mutex>
#include <condition_variable>

/**
 * @brief 单一所有者线程的后台串行执行器；不直接操作控件。
 * @details prepare/obsolete/done 在所有者线程运行；run 在专用线程运行。上一任务的
 * 完成回调结束后才准备下一任务，允许安全衔接 GStreamer 句柄的创建/释放。
 * @note 普通 enqueue 不等待；shutdown 仅供最终析构；管线丢弃未开始任务，配置可选择串行完成待写值，随后等待后台退出。
 * 所有 enqueue/busy/shutdown 调用必须来自所有者线程；run 不得等待所有者线程。
 */
class SerialWorkQueue final : public QObject {
public:
    /** @brief 一次后台工作及其完成处理；不提供隐式重试。 */
    struct Task {
        std::function<QString()> run; /**< 后台操作；空串成功，非空错误；可为空表示仅执行完成回调。 */
        std::function<void(const QString &)> done; /**< 所有者线程完成回调；shutdown 后不再执行。 */
        bool reportSlow=true; /**< false仅关闭该任务原有慢段输出；不改变执行及错误回调。 */
    };
    /** @brief 创建后台线程。
     * @param[in] name 线程诊断名称。
     * @param[in] parent QObject所有者，可空。
     * @param[in] drainPendingOnShutdown true为配置队列最终退出时串行完成待写任务；false丢弃未开始的管线任务。
     */
    explicit SerialWorkQueue(const QString &name, QObject *parent=nullptr, bool drainPendingOnShutdown=false)
        : QObject(parent),drainPendingOnShutdown_(drainPendingOnShutdown) {
        thread_=std::thread([this,name] {
            QThread::currentThread()->setObjectName(name);
            for(;;) {
                std::function<void()> work;
                { std::unique_lock<std::mutex> lock(workMutex_);
                  workReady_.wait(lock,[this]{return workerStop_ || !work_.empty();});
                  if(work_.empty() && workerStop_) break;
                  work=std::move(work_.front());work_.pop_front(); }
                work();
            }
        });
    }
    /** @brief 最终退出时等待当前任务；正常播放/停止路径不得调用。 */
    ~SerialWorkQueue() override { shutdown(); }
    /**
     * @brief 提交任务；后台空闲时立即在所有者线程准备，不等待后台工作。
     * @param[in] prepare 延迟捕获当前有效句柄/状态；只有准备阶段可访问界面对象。
     * @param[in] key 非空时替换同 key 的未开始任务；不取消正在执行任务或空 key 屏障。
     * @param[in] obsolete 可空；执行前为 true 则丢弃任务，不调用 prepare/done。
     */
    void enqueue(std::function<Task()> prepare, const QString &key={}, std::function<bool()> obsolete={}) {
        Q_ASSERT(QThread::currentThread()==this->thread());if(closing_) return;
        if(!key.isEmpty()) for(auto it=pending_.begin();it!=pending_.end();) {
            if(it->key==key) it=pending_.erase(it);else ++it;
        }
        pending_.push_back({std::move(prepare),key,std::move(obsolete)});drain();
    }
    /** @return true 表示后台操作尚未完成；仅由所有者线程读取。 */
    bool busy() const { return busy_; }
    /** @brief 最终析构屏障；按构造策略处理待执行任务并等待后台退出，不调用done；run不得同步等待UI。 */
    void shutdown() {
        Q_ASSERT(QThread::currentThread()==this->thread());if(closing_) return;
        closing_=true;
        // 配置退出保留最后待写值；只在最终关闭准备值任务，不执行UI完成回调。
        if(drainPendingOnShutdown_) while(!pending_.empty()) {
            auto pending=std::move(pending_.front());pending_.pop_front();
            if(pending.obsolete && pending.obsolete()) continue;
            auto task=pending.prepare();if(!task.run) continue;
            post([run=std::move(task.run)] {
                try { const auto error=run();if(!error.isEmpty()) qWarning()<<"[media-work] shutdown:"<<error; }
                catch(...) { qWarning()<<"[media-work] shutdown task exception"; }
            });
        }
        pending_.clear();
        // 后台退出不依赖应用事件循环；QCoreApplication析构阶段也能完成已排队写入。
        { std::lock_guard<std::mutex> lock(workMutex_);workerStop_=true; }
        workReady_.notify_one();if(thread_.joinable()) thread_.join();
    }
private:
    /** @brief 仅所有者线程保存的待准备任务；空 key 表示不可合并的顺序屏障。 */
    struct Pending { std::function<Task()> prepare;QString key;std::function<bool()> obsolete; };
    /** @brief 准备下一有效任务并异步执行；draining_ 阻止 prepare 内嵌套 enqueue 重入。 */
    void drain() {
        if(closing_ || busy_ || draining_) return;
        draining_=true;
        while(!pending_.empty() && !busy_ && !closing_) {
            auto pending=std::move(pending_.front());pending_.pop_front();
            if(pending.obsolete && pending.obsolete()) continue;
            const QPointer<SerialWorkQueue> alive(this);
            auto task=pending.prepare();if(!alive) return;
            if(!task.run) { if(task.done) task.done({});if(!alive) return;continue; }
            busy_=true;const auto operation=pending.key.isEmpty() ? QStringLiteral("barrier") : pending.key;
            post([this,task=std::move(task),operation]() mutable {
                QElapsedTimer timer;timer.start();QString error;
                try { error=task.run(); } catch(...) { error=QStringLiteral("后台串行任务异常"); }
                if(task.reportSlow && timer.elapsed()>=20) qInfo()<<"[media-work] queue="<<static_cast<const void*>(this)
                    <<"thread="<<QThread::currentThread()->objectName()<<"operation="<<operation
                    <<"beginMonoMs="<<timer.msecsSinceReference()<<"elapsedMs="<<timer.elapsed();
                QMetaObject::invokeMethod(this,[this,task=std::move(task),error]() mutable {
                    if(closing_) return;
                    // done 可提交后继任务；完成回调结束前保持 busy，避免提前修改句柄。
                    const QPointer<SerialWorkQueue> alive(this);
                    if(task.done) task.done(error);
                    if(!alive) return;
                    busy_=false;drain();
                },Qt::QueuedConnection);
            });
        }
        draining_=false;
    }
    /** @brief 只交接函数值的后台入口；短锁排队，执行任务时不持锁。
     * @param[in] work 自有可调用对象，后台执行；不得同步等待UI，不得抛出未处理异常。
     */
    void post(std::function<void()> work) {
        { std::lock_guard<std::mutex> lock(workMutex_);work_.push_back(std::move(work)); }
        workReady_.notify_one();
    }
    std::thread thread_; /**< 独占后台执行线程；最终shutdown join，不依赖Qt事件循环退出。 */
    std::mutex workMutex_;std::condition_variable workReady_; /**< 仅保护后台待运行函数和workerStop_，不保护播放器/配置内容。 */
    std::deque<std::function<void()>> work_;bool workerStop_=false; /**< 关闭后先排空后台函数，再退出；由workMutex_保护。 */
    std::deque<Pending> pending_; /**< 所有者线程独占；不与后台共享容器。 */
    bool drainPendingOnShutdown_=false; /**< true只用于配置值任务；最终关闭完成待写run，跳过UI通知。 */
    bool busy_=false,closing_=false,draining_=false; /**< 后台在途、最终关闭、准备重入保护。 */
};
