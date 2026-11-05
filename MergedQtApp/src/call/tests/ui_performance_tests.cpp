/**
 * @file ui_performance_tests.cpp
 * @brief 验证真实notify观察、对象销毁安全、有界汇总与跨线程日志计时。
 * @author Du lin
 * @date 2026-11-03
 */
#include <QtTest>
#include <QTimer>
#include <QRegularExpression>
#include "common/debug/ui_performance.h"
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace {
QStringList lines; /**< 仅UI线程收集诊断行，普通工作线程消息不写入此容器。 */
std::atomic_int writeDelayMs{0},reportDelayMs{0}; /**< 只模拟原日志写入/诊断输出延迟，不用于生产。 */
QtMessageHandler oldHandler=nullptr; /**< 测试前消息处理器，清理时恢复。 */

/** @brief 测试消息边界，模拟原格式化及write，验证生产LogMeasurement不递归。
 * @param[in] type 原日志级别，测试不改变Qt业务行为。
 * @param[in] context 原日志上下文。
 * @param[in] message 当前消息；诊断行仅在UI线程产生。
 */
void testHandler(QtMsgType type,const QMessageLogContext &context,const QString &message)
{
    Q_UNUSED(type);Q_UNUSED(context);
    UiPerformance::LogMeasurement measurement;
    const auto bytes=message.toUtf8();measurement.beforeWrite();
    const bool report=message.startsWith(QStringLiteral("[ui-perf]"));
    const auto delay=report ? reportDelayMs.load() : writeDelayMs.load();
    if(delay) QTest::qSleep(delay);
    measurement.finish(bytes.size(),bytes.size());
    if(report) lines.append(message);
}
/** @brief 查询某一类诊断最后一行。
 * @param[in] marker 日志精确子串。
 * @return 最后匹配行；无匹配为空串。
 */
QString lastLine(const QString &marker)
{
    QString result;for(const auto &line:lines) if(line.contains(marker)) result=line;return result;
}
/** @brief 读取独立数字字段，缺项为-1，避免把缺项误判为零。
 * @param[in] line 一条诊断行。
 * @param[in] key 不带等号的数字字段名。
 * @return 字段整数值；缺项为-1。
 */
qint64 field(const QString &line,const QString &key)
{
    const QRegularExpression expression(QStringLiteral("(?:^| )%1=(-?\\d+)(?: |$)").arg(QRegularExpression::escape(key)));
    const auto match=expression.match(line);return match.hasMatch() ? match.captured(1).toLongLong() : -1;
}
}

/** @brief 可慢处理、嵌套分发或在事件中自销毁的真实QObject接收者。 */
class SlowReceiver final : public QObject {
    Q_OBJECT
public:
    std::function<void()> action; /**< 测试回调，复制后调用以允许回调销毁本对象。 */
    bool handled=true; /**< 预期原event返回值，诊断不得更改。 */
    /** @brief 处理测试User事件；其余事件原样交给QObject。
     * @param[in] event 原事件，测试回调不拥有它。
     * @return User事件返回配置值；其他返回QObject原始结果。
     */
    bool event(QEvent *event) override {
        if(event->type()!=QEvent::User) return QObject::event(event);
        const bool result=handled;const auto callback=action;if(callback) callback();return result;
    }
};

/** @brief 在真实UiDiagnosticApplication中执行诊断回归。 */
class UiPerformanceTests final : public QObject {
    Q_OBJECT
private slots:
    /** @brief 安装测试输出边界，QTest结果输出保持其自身机制。 */
    void initTestCase() { oldHandler=qInstallMessageHandler(testHandler); }
    /** @brief 每项测试清空记录；前项cleanup已使诊断退出。 */
    void init() { lines.clear();writeDelayMs=0;reportDelayMs=0; }
    /** @brief 结算未结束通话，并在下一测试前等待诊断结束回调。 */
    void cleanup() { UiPerformance::setCallContext({});QTest::qWait(30);writeDelayMs=0;reportDelayMs=0; }
    /** @brief 恢复原消息处理器。 */
    void cleanupTestCase() { qInstallMessageHandler(oldHandler); }
    /** @brief 无呼叫时不记录操作、不输出窗口，不改变事件返回。 */
    void inactive() {
        SlowReceiver receiver;receiver.handled=false;receiver.action=[]() { QTest::qSleep(12); };
        QEvent event(QEvent::User);QVERIFY(!QCoreApplication::sendEvent(&receiver,&event));
        { UiPerformance::OperationScope scope(UiPerformance::Operation::VideoPaint,&receiver);QTest::qSleep(12); }
        UiPerformance::reportNow();QVERIFY(lines.isEmpty());
    }
    /** @brief 长事件保留入口身份、事件类型、原返回值，快照不读取控件文本。 */
    void receiverAndReturn() {
        UiPerformance::setCallContext("receiver-test");SlowReceiver receiver;
        receiver.setObjectName("slow\nreceiver");receiver.handled=false;receiver.action=[]() { QTest::qSleep(22); };
        QEvent event(QEvent::User);QVERIFY(!QCoreApplication::sendEvent(&receiver,&event));UiPerformance::reportNow();
        const auto line=lastLine("eventType=User(");QVERIFY(!line.isEmpty());QVERIFY(line.contains("receiver=SlowReceiver"));
        QVERIFY(line.contains("name=\"slow\\nreceiver\""));QVERIFY(!line.contains('\n'));
        QVERIFY(field(line,"maxUs")>=20000);QVERIFY(field(line,"maxEndMonoMs")>=field(line,"maxStartMonoMs"));
    }
    /** @brief 在事件中删除接收者后，观察器只使用事先复制的身份。 */
    void destroyedReceiver() {
        UiPerformance::setCallContext("deleted");auto *receiver=new SlowReceiver;receiver->setObjectName("deleted.inside.event");
        receiver->action=[receiver]() { QTest::qSleep(12);delete receiver; };
        QEvent event(QEvent::User);QVERIFY(QCoreApplication::sendEvent(receiver,&event));UiPerformance::reportNow();
        QVERIFY(lastLine("eventType=User(").contains("deleted.inside.event"));
    }
    /** @brief 真正QTimer事件包含名字及有效timerId，只有原回调控制停止。 */
    void timerIdentity() {
        UiPerformance::setCallContext("timer");QTimer timer;timer.setObjectName("test.slow.timer");timer.setSingleShot(true);
        connect(&timer,&QTimer::timeout,this,[]() { QTest::qSleep(18); });timer.start(1);
        QTRY_VERIFY(!timer.isActive());UiPerformance::reportNow();
        const auto line=lastLine("name=\"test.slow.timer\"");QVERIFY(line.contains("eventType=Timer("));QVERIFY(field(line,"timerId")>=0);
    }
    /** @brief 嵌套事件包含depth，包含式操作分别计时，后台线程操作不污染UI统计。 */
    void nestedOperations() {
        UiPerformance::setCallContext("nested");SlowReceiver outer,inner;
        outer.setObjectName("outer");inner.setObjectName("inner");inner.action=[]() { QTest::qSleep(12); };
        outer.action=[&]() {
            UiPerformance::OperationScope paint(UiPerformance::Operation::VideoPaint,&outer);
            { UiPerformance::OperationScope upload(UiPerformance::Operation::VideoUpload,&outer);QEvent event(QEvent::User);QCoreApplication::sendEvent(&inner,&event); }
            QTest::qSleep(12);
        };
        QEvent event(QEvent::User);QCoreApplication::sendEvent(&outer,&event);
        std::thread worker([]() { UiPerformance::OperationScope ignored(UiPerformance::Operation::MediaStop);QTest::qSleep(12); });worker.join();
        UiPerformance::reportNow();
        QVERIFY(field(lastLine("op=videoPaint "),"maxUs")>=24000);QVERIFY(field(lastLine("op=videoUpload "),"maxUs")>=10000);
        QVERIFY(lastLine("op=mediaStop ").isEmpty());QVERIFY(field(lastLine("name=\"inner\""),"depth")>=2);
        QString outerLine;for(const auto &line:lines) if(line.contains("eventType=User(") && line.contains("name=\"outer\"")) outerLine=line;
        QVERIFY(field(outerLine,"nestedEvents")>=1);
    }
    /** @brief 模态式嵌套事件循环仍有心跳，长父事件必须标出nestedEvents，不能被当作持续UI冻结。 */
    void nestedEventLoopProgress() {
        UiPerformance::setCallContext("nested-loop");SlowReceiver receiver;receiver.setObjectName("nested.loop");int beats=0;
        receiver.action=[&]() {
            QEventLoop loop;QTimer heartbeat;
            connect(&heartbeat,&QTimer::timeout,&loop,[&]() { ++beats; });heartbeat.start(5);
            QTimer::singleShot(100,&loop,&QEventLoop::quit);loop.exec();
        };
        QEvent event(QEvent::User);QCoreApplication::sendEvent(&receiver,&event);UiPerformance::reportNow();
        QVERIFY(beats>=3);const auto line=lastLine("name=\"nested.loop\"");
        QVERIFY(field(line,"maxUs")>=80000);QVERIFY(field(line,"nestedEvents")>=3);
    }
    /** @brief 工作线程和UI的日志写入分别统计，错误/短写只计数不改变原返回值。 */
    void logTiming() {
        UiPerformance::setCallContext("logs");writeDelayMs=15;qInfo()<<"UI log";
        std::thread worker([]() { qInfo()<<"worker log"; });worker.join();writeDelayMs=0;
        { UiPerformance::LogMeasurement failure;failure.beforeWrite();failure.finish(16,-1);failure.finish(16,-1); }
        { UiPerformance::LogMeasurement shortWrite;shortWrite.beforeWrite();shortWrite.finish(16,4); }
        UiPerformance::reportNow();
        const auto ui=lastLine("op=qtLog thread=ui "),background=lastLine("op=qtLog thread=worker ");
        QCOMPARE(field(ui,"count"),qint64(3));QCOMPARE(field(ui,"writeErrors"),qint64(1));QCOMPARE(field(ui,"shortWrites"),qint64(1));
        QVERIFY(field(ui,"writeMaxUs")>=14000);QCOMPARE(field(background,"count"),qint64(1));QVERIFY(field(background,"writeMaxUs")>=14000);
    }
    /** @brief 报告自身不递归记为普通日志，输出耗时在下一窗口单独显示，窗口计数清零。 */
    void reportSuppressionAndReset() {
        UiPerformance::setCallContext("report");
        { UiPerformance::OperationScope scope(UiPerformance::Operation::PlaybackPosition);QTest::qSleep(12); }
        reportDelayMs=4;UiPerformance::reportNow();reportDelayMs=0;lines.clear();UiPerformance::reportNow();
        QVERIFY(field(lastLine(" windowStartMonoMs="),"previousReportUs")>=10000);
        QVERIFY(lastLine("op=playbackPosition ").isEmpty());QCOMPARE(field(lastLine("op=qtLog thread=ui "),"count"),qint64(0));
    }
    /** @brief 超过16组长事件时保持固定容量且只输出最慢3组，汇总计数仍覆盖全部。 */
    void boundedEvents() {
        UiPerformance::setCallContext("bounded");std::vector<std::unique_ptr<SlowReceiver>> receivers;
        for(int i=0;i<20;++i) {
            receivers.emplace_back(new SlowReceiver);auto &receiver=*receivers.back();receiver.setObjectName(QString::number(i));
            receiver.action=[]() { QTest::qSleep(11); };QEvent event(QEvent::User);QCoreApplication::sendEvent(&receiver,&event);
        }
        UiPerformance::reportNow();int details=0;for(const auto &line:lines) details+=line.contains(" eventType=");
        QCOMPARE(details,3);QCOMPARE(field(lastLine(" windowStartMonoMs="),"slowEvents"),qint64(20));
        QVERIFY(field(lastLine(" windowStartMonoMs="),"detailOmitted")>=4);
    }
    /** @brief 结束请求延后汇总，包含当前挂断事件；下一呼叫不沿用旧窗口。 */
    void deferredFinalAndNewCall() {
        UiPerformance::setCallContext("old-call");SlowReceiver receiver;receiver.setObjectName("hangup.handler");
        receiver.action=[]() { UiPerformance::setCallContext({});QTest::qSleep(15); };
        QEvent event(QEvent::User);QCoreApplication::sendEvent(&receiver,&event);
        QTRY_VERIFY(!lastLine("phase=\"final\"").isEmpty());QVERIFY(lastLine("eventType=User(").contains("hangup.handler"));
        lines.clear();UiPerformance::setCallContext("new-call");UiPerformance::reportNow();
        QVERIFY(lastLine(" windowStartMonoMs=").contains("callId=\"new-call\""));
        QCOMPARE(field(lastLine(" windowStartMonoMs="),"slowEvents"),qint64(0));
    }
    /** @brief 实际3秒定时器会自动输出诊断，静默期间无需业务事件主动调用report。 */
    void periodicSummary() {
        UiPerformance::setCallContext("periodic");QTRY_VERIFY_WITH_TIMEOUT(!lastLine("phase=\"tick\"").isEmpty(),4000);
    }
};

/** @brief 用生产诊断应用运行QTest，不使用另写的事件分发模型。
 * @param[in] argc 原测试命令参数数量。
 * @param[in] argv 原测试命令参数数组。
 * @return QTest原执行结果。
 */
int main(int argc,char **argv) { UiDiagnosticApplication app(argc,argv);UiPerformanceTests test;return QTest::qExec(&test,argc,argv); }
#include "ui_performance_tests.moc"
