/**
 * @file voice_call_tests.cpp
 * @brief 验证真实 V1 编解码、抖动缓冲、信令状态机、有界信用和异步收尾。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-04；修改人：Du lin；新增图片2项预算/拒绝/取消释放与音频信令优先回归，保留原全部通话/固定槽/信用测试，不改DSP或录放。
 */
#include <QtTest>
#include <QLabel>
#include <QWidget>
#include <QDir>
#include <QFontDatabase>
#include <QApplication>
#include <QPushButton>
#include <QFile>
#include <QTemporaryDir>
#include "src/call/CallGpioButton.h"
#include "components/cursoroverlay/keyboard_dialog.h"
#include "src/call/CallPanel.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include "src/call/CallManager.h"
#include "src/call/MqttCallAdapter.h"
#include "common/debug/ui_performance.h"
#include "components/settings/mqtt/mqttipcclient.h"
#include <atomic>
#include <thread>
#include <QThreadPool>
#include <QSemaphore>
#include <QRunnable>
#include "components/settings/mqtt/mqttpacketparser.h"
#include "common/fixed_mailbox.h"

namespace VoiceTest {
extern std::atomic_bool failOpen;
extern std::atomic_int activeDevices,exclusive,restoreDelayMs,playedNonzero,volumeDelayMs;
extern std::atomic_int playbackXrunPauseMs,playbackDiagnosticDelay,capturePauseMs,captureGain,captureTimingFault;
}

/** @brief 临时 sysfs 测试写入器，值文件原地更新以模拟同一输入句柄。
 * @param[in] root 隔离临时根目录。
 * @param[in] name 根目录内的相对属性名。
 * @param[in] bytes 测试属性值；重复 value 写入均为两个字节，避免截断窗口。
 * @return true 完整写入；false 打开/写入失败。
 */
static bool putGpio(const QString &root,const QString &name,const QByteArray &bytes)
{
    QFile file(QDir(root).filePath(name));
    return file.open(QIODevice::ReadWrite | QIODevice::Unbuffered) && file.write(bytes)==bytes.size();
}
/** @brief 读取隔离测试属性；读取失败返回空字节串。
 * @param[in] root 隔离临时根目录。
 * @param[in] name 相对属性名。
 * @return 去除换行的属性值，失败为空。
 */
static QByteArray getGpio(const QString &root,const QString &name)
{
    QFile file(QDir(root).filePath(name));
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed() : QByteArray();
}
/** @brief 在临时目录创建已导出 GPIO40 的最小属性文件。
 * @param[in] root 临时目录，不访问真实 GPIO。
 * @param[in] value 初始输入电平；每个测试自行决定启动是否已按下。
 * @return true 属性文件创建成功。
 */
static bool makeGpio(const QString &root,const QByteArray &value)
{
    return QDir(root).mkpath("gpio40") && putGpio(root,"gpio40/direction","out\n") &&
        putGpio(root,"gpio40/active_low","1\n") && putGpio(root,"gpio40/value",value);
}

/** @brief 与真实 QLocalSocket 对接的 mqttd 信用替身，不模拟业务状态机。 */
class Peer : public QObject {
public:
    QLocalServer server; /**< 本机测试监听器。 */
    QLocalSocket *socket=nullptr; /**< server 所有的已接入套接字。 */
    QByteArray buffer; /**< 接收的未完整 JSON 行。 */
    QList<QJsonObject> events; /**< 设备实际发布的通话事件。 */
    QList<QJsonObject> audio; /**< 设备实际提交的原始音频 IPC 包。 */
    QStringList commands; /**< 实际收到的完整命令顺序，用于配置/apply依赖回归。 */
    int creditDelayMs=0; /**< 模拟 IPC/界面延迟，单位毫秒。 */
    bool holdCredit=false; /**< true 不回发 call_sent，触发真实拥塞处理。 */
    bool pauseReading=false; /**< 测试期间不消费服务端缓冲，用于制造真实QLocalSocket背压。 */
    /** @brief 建立测试监听并逐行处理真实适配器输出。 */
    Peer() {
        server.listen(QStringLiteral("voice-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        connect(&server,&QLocalServer::newConnection,this,[this]() {
            socket=server.nextPendingConnection();
            connect(socket,&QLocalSocket::readyRead,this,[this]() {
                if (pauseReading) return;
                buffer+=socket->readAll(); int newline;
                while ((newline=buffer.indexOf('\n'))>=0) {
                    const auto obj=QJsonDocument::fromJson(buffer.left(newline)).object(); buffer.remove(0,newline+1);
                    const QString cmd=obj.value("cmd").toString();
                    commands.append(cmd);
                    if (cmd=="publish") events.append(obj.value("payload").toObject());
                    if (cmd=="call_media" && obj.value("enabled").toBool()) send({{"type","call_ready"},{"epoch",obj.value("epoch")},{"ok",true},{"window",5}});
                    if (cmd=="publish_audio") {
                        audio.append(obj);
                        if (!holdCredit) QTimer::singleShot(creditDelayMs,this,[this,obj]() { send({{"type","call_sent"},{"epoch",obj.value("epoch")},{"ok",true}}); });
                    }
                }
            });
        });
    }
    /** @brief 向真实 IPC 客户端提交一行 JSON。
     * @param[in] obj 待紧凑编码的测试服务端消息；未连接时忽略。
     */
    void send(const QJsonObject &obj) { if (socket) socket->write(QJsonDocument(obj).toJson(QJsonDocument::Compact)+'\n'); }
};

/** @brief 模拟写槽已归生产者但复制尚未完成的抢占点，仅隔离并发测试使用。 */
struct PausedMailboxValue {
    int number=0;QSemaphore *entered=nullptr,*release=nullptr;
    PausedMailboxValue()=default;PausedMailboxValue(const PausedMailboxValue&)=default;PausedMailboxValue(PausedMailboxValue&&)=default;
    /** @brief 只在被测发布第一次移动时暂停，不影响其他槽操作。 */
    PausedMailboxValue &operator=(PausedMailboxValue &&value) {
        number=value.number;entered=value.entered;release=value.release;
        if(entered) { auto *gate=release;entered->release();entered=nullptr;release=nullptr;gate->acquire(); }
        return *this;
    }
};

/** @brief 模拟Qt隐式共享值的交换式移动赋值，检查旧正文在哪个调用阶段释放。 */
struct SwapMailboxValue {
    std::shared_ptr<int> value;
    SwapMailboxValue()=default;SwapMailboxValue(const SwapMailboxValue&)=default;SwapMailboxValue(SwapMailboxValue&&)=default;
    /** @brief 故意交换旧值，与Qt共享对象的移动赋值约束一致。 */
    SwapMailboxValue &operator=(SwapMailboxValue &&other) noexcept { value.swap(other.value);return *this; }
};

/** @brief 读取日志中的独立整数统计字段，避免txDrop和txDropFull等前缀混淆。
 * @param[in] text 一条统计日志。
 * @param[in] name 字段名，不含等号。
 * @return 字段值；缺项返回-1，使测试不能把缺项误认为零。
 */
static qint64 diagnosticValue(const QString &text,const QString &name)
{
    const auto prefix=name+'=';
    for (const auto &field:text.split(' ')) if (field.startsWith(prefix)) return field.mid(prefix.size()).toLongLong();
    return -1;
}
/** @brief 从信号快照中查找最后一条含指定标记的日志。
 * @param[in] logs statistics或logMessage的信号记录。
 * @param[in] marker 精确子串标记。
 * @return 最后匹配行；未出现返回空串。
 */
static QString lastDiagnostic(const QSignalSpy &logs,const QString &marker)
{
    QString result;
    for (const auto &entry:logs) if (entry.first().toString().contains(marker)) result=entry.first().toString();
    return result;
}
/** @brief 按模块及阶段读取最终发送摘要，避免新播放摘要的phase=final被误当发送统计。
 * @param[in] logs 通话日志信号快照。
 * @return 最后发送汇总；尚未到达则为空。
 */
static QString lastTransportFinal(const QSignalSpy &logs)
{
    QString result;
    for(const auto &entry:logs) {
        const auto line=entry.first().toString();
        if(line.contains("transport txSubmit=") && line.contains("phase=final")) result=line;
    }
    return result;
}

/** @brief 独立线程中的真实本地服务端，避免UI暂停连测试服务端也停掉而制造假信用耗尽。 */
class BackgroundPeer final {
public:
    const QString name=QStringLiteral("voice-background-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)); /**< 不可变服务名。 */
    std::atomic_bool listening{false},connected{false}; /**< 后台就绪快照。 */
    std::atomic_int audio{0},business{0},smallBusiness{0},downlink{0}; /**< 后台计数，UI只读。 */
    std::atomic<qint64> gapMax{0}; /**< 后台收到上行帧的最大间隔，毫秒。 */
    /** @brief 创建服务端事件循环；所有socket、计时器和缓冲均只在该线程访问。 */
    BackgroundPeer() {
        context_=new QObject;context_->moveToThread(&thread_);
        QObject::connect(&thread_,&QThread::finished,context_,&QObject::deleteLater);
        QObject::connect(&thread_,&QThread::started,context_,[this]() {
            auto *server=new QLocalServer(context_);listening=server->listen(name);
            QObject::connect(server,&QLocalServer::newConnection,context_,[this,server]() {
                socket_=server->nextPendingConnection();connected=true;
                timer_=new QTimer(context_);timer_->setTimerType(Qt::PreciseTimer);timer_->setInterval(20);
                QObject::connect(timer_,&QTimer::timeout,context_,[this]() {
                    VoiceCall::Frame frame;frame.seq=uint8_t(downlink.fetch_add(1));frame.pcm.fill(800);
                    const auto packet=VoiceCall::encode(frame,uint32_t(downlink.load()*20));
                    send({{"type","audio"},{"epoch",epoch_},{"topic","device/ycLinux/D/audio"},{"retain",0},
                        {"token",downlink.load()},{"arrivalMs",double(AudioEngine::nowMs())},
                        {"hex",QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(packet.data()),650).toHex())}});
                });
                QObject::connect(socket_,&QLocalSocket::readyRead,context_,[this]() {
                    buffer_+=socket_->readAll();int end;
                    while((end=buffer_.indexOf('\n'))>=0) {
                        const auto object=QJsonDocument::fromJson(buffer_.left(end)).object();buffer_.remove(0,end+1);
                        const auto command=object.value("cmd").toString();
                        if(command=="call_media") {
                            if(object.value("enabled").toBool()) { epoch_=object.value("epoch").toString();send({{"type","call_ready"},{"epoch",epoch_},{"ok",true},{"window",5}});timer_->start(); }
                            else timer_->stop();
                        } else if(command=="publish_audio") {
                            const auto now=AudioEngine::nowMs();if(lastAudioMs_) gapMax=qMax(gapMax.load(),now-lastAudioMs_);lastAudioMs_=now;++audio;
                            send({{"type","call_sent"},{"epoch",object.value("epoch")},{"ok",true}});
                        } else if(command=="test-large-out") ++business;
                        else if(command=="test-small-out") ++smallBusiness;
                    }
                });
            });
        });thread_.start();
    }
    /** @brief 测试结束才等待服务线程退出，不参与生产行为。 */
    ~BackgroundPeer() { thread_.quit();thread_.wait(); }
    /** @brief 排队一个完整大包，正文从不与后续媒体行交错。
     * @param[in] bytes 完整业务行，调用者已在建立通话前准备好。
     */
    void sendBusiness(const QByteArray &bytes) { QMetaObject::invokeMethod(context_,[this,bytes]() { if(socket_) socket_->write(bytes); },Qt::QueuedConnection); }
private:
    /** @brief 后台提交小媒体JSON。
     * @param[in] object 本测试服务端产生的小帧。
     */
    void send(const QJsonObject &object) { if(socket_) socket_->write(QJsonDocument(object).toJson(QJsonDocument::Compact)+'\n'); }
    QThread thread_;QObject *context_=nullptr;QLocalSocket *socket_=nullptr;QTimer *timer_=nullptr; /**< context及其子对象归服务线程释放。 */
    QByteArray buffer_;QString epoch_;qint64 lastAudioMs_=0; /**< 仅服务线程读写的协议缓存。 */
};
/** @brief 在测试中占住业务任务池，证明IPC与音频不等待业务解析；由信号量明确释放。 */
class HoldBusinessTask final:public QRunnable {
public:
    QSemaphore started,release;std::atomic_bool done{false}; /**< 测试所有的启动/释放门及结束快照。 */
    /** @brief 禁用任务池自动删除，测试在done后释放，避免信号量悬空。 */
    HoldBusinessTask() { setAutoDelete(false); }
    /** @brief 后台等待测试释放，仅此故障注入任务阻塞。 */
    void run() override { started.release();release.acquire();done=true; }
};

/** @brief 硬件无关回归，所有信令调用真实 CallManager，不重新实现状态机。 */
class VoiceCallTests : public QObject {
    Q_OBJECT
    QTemporaryDir dspConfigDir_; /**< 本测试实例拥有的隔离INI目录，覆盖所有测试通话。 */
private slots:
    /** @brief 一个生产者暂停在Writing不能拖住其他槽；拒绝条件不改变旧值。 */
    void mailboxPausedProducerAndExpiry() {
        FixedMailbox<PausedMailboxValue,5> mailbox;QSemaphore entered,release;bool first=false;
        std::thread producer([&] { PausedMailboxValue value;value.number=1;value.entered=&entered;value.release=&release;first=mailbox.push(std::move(value)); });
        entered.acquire();PausedMailboxValue other;other.number=2;const bool pushed=mailbox.push(std::move(other));
        PausedMailboxValue received;const bool rejected=!mailbox.popIf(received,[](const PausedMailboxValue &){return false;});
        const bool taken=mailbox.pop(received);const int number=received.number;
        release.release();producer.join();QVERIFY(first && pushed && rejected && taken);QCOMPARE(number,2);
        QVERIFY(mailbox.pop(received));QCOMPARE(received.number,1);QVERIFY(mailbox.empty());
    }
    /** @brief 满时淘汰最旧而非改写Reading；并发PCM值/序号保持完整，计数守恒。 */
    void mailboxConcurrentPcmIntegrity() {
        FixedMailbox<VoiceCall::Frame,5> mailbox;std::atomic_bool done{false};uint64_t drops=0,taken=0,last=0;bool valid=true;
        std::thread producer([&] { for(uint64_t i=1;i<=30000;++i) { VoiceCall::Frame frame;frame.captureIndex=i;frame.pcm.fill(int16_t(i%20000));bool replaced=false;if(!mailbox.push(frame,true,&replaced) || replaced) ++drops; }done=true; });
        VoiceCall::Frame frame;
        while(!done.load() || !mailbox.empty()) { if(!mailbox.pop(frame)) { std::this_thread::yield();continue; }
            valid=valid && frame.captureIndex>last;last=frame.captureIndex;++taken;
            for(const auto sample:frame.pcm) valid=valid && sample==int16_t(frame.captureIndex%20000);
        }
        producer.join();QVERIFY(valid);QVERIFY(taken>0);QCOMPARE(taken+drops,uint64_t(30000));
    }
    /** @brief 消费者旧输出不得留在空槽，防止下一生产者释放大正文。 */
    void mailboxReleasesOldPayloadOnConsumer() {
        FixedMailbox<SwapMailboxValue,1> mailbox;int released=0;
        SwapMailboxValue incoming;incoming.value=std::make_shared<int>(9);QVERIFY(mailbox.push(std::move(incoming)));
        SwapMailboxValue output;output.value=std::shared_ptr<int>(new int(3),[&](int *value) { ++released;delete value; });
        QVERIFY(mailbox.pop(output));QCOMPARE(*output.value,9);QCOMPARE(released,1);
        SwapMailboxValue next;next.value=std::make_shared<int>(10);QVERIFY(mailbox.push(std::move(next)));QCOMPARE(released,1);
    }
    /** @brief 普通IPC共享的多生产者固定槽保持每生产者顺序、无重复及正文完整。 */
    void mailboxMultipleProducers() {
        FixedMailbox<VoiceCall::Frame,80> mailbox;std::atomic_int finished{0};std::array<std::thread,4> producers;
        for(unsigned owner=0;owner<producers.size();++owner) producers[owner]=std::thread([&,owner] {
            for(unsigned i=1;i<=5000;++i) { VoiceCall::Frame frame;frame.captureIndex=uint64_t(owner)*10000+i;frame.pcm.fill(int16_t(i));while(!mailbox.push(frame)) std::this_thread::yield(); }
            ++finished;
        });
        std::array<unsigned,4> counts{};bool valid=true;VoiceCall::Frame frame;
        while(finished.load()<4 || !mailbox.empty()) {
            if(!mailbox.pop(frame)) { std::this_thread::yield();continue; }
            const auto owner=unsigned(frame.captureIndex/10000),index=unsigned(frame.captureIndex%10000);
            if(owner>=4) { valid=false;continue; }
            valid=valid && index==++counts[owner];for(const auto sample:frame.pcm) valid=valid && sample==int16_t(index);
        }
        for(auto &producer:producers) producer.join();QVERIFY(valid);for(const auto count:counts) QCOMPARE(count,5000U);
    }
    /** @brief 图片最多2项在途，音频信令可越过等预算图片；取消必须归还图片预算。 */
    void snapshotUploadBudgetAndMediaPriority(){
        Peer peer;MqttIpcClient ipc;ipc.connectToServer(peer.server.serverName());QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.socket);
        const auto reservation=ipc.reserveRealtime();
        QVERIFY(ipc.sendJsonLine({{"cmd","padding-budget"},{"data",QString(1024*1024,'x')}}));QTRY_VERIFY(peer.commands.contains("padding-budget"));
        const QByteArray image=QJsonDocument(QJsonObject{{"cmd","snapshot-budget"},{"data",QString(128*1024,'x')}}).toJson(QJsonDocument::Compact)+'\n';
        QVERIFY(ipc.sendSnapshotRawLine(image));QVERIFY(ipc.sendSnapshotRawLine(image));QCOMPARE(ipc.pendingSnapshotPublishes(),2);QVERIFY(!ipc.sendSnapshotRawLine(image));
        QVERIFY(ipc.sendJsonLine({{"cmd","call_media"},{"enabled",false},{"epoch","snapshot-priority"}}));QTRY_VERIFY(peer.commands.contains("call_media"));QVERIFY(!peer.commands.contains("snapshot-budget"));
        ipc.disconnectFromServer();QTRY_VERIFY(!ipc.isConnected());QTRY_COMPARE(ipc.pendingSnapshotPublishes(),0);ipc.releaseRealtime(reservation);
    }
    /** @brief 固定实时行拒绝非法长度，身份转义/PCM二进制与既有协议一致。 */
    void realtimeWireCompatibility() {
        Peer peer;MqttIpcClient ipc;CallManager manager(&ipc);ipc.connectToServer(peer.server.serverName());QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.socket);
        QVERIFY(!ipc.sendRealtimeLine(nullptr,1));QVERIFY(!ipc.sendRealtimeLine("x",1));QByteArray tooLong(2049,'x');tooLong[2048]='\n';QVERIFY(!ipc.sendRealtimeLine(tooLong.constData(),tooLong.size()));
        const QString id=QStringLiteral("device\"\\unicode-\u7269");manager.configure(id);manager.setOnline(true);
        manager.handleSignal(QStringLiteral("device/ycLinux/%1/request").arg(id),{{"method","call.start"},{"id","wire"},{"deviceId",id},{"data",QJsonObject{{"callId","wire"}}}});
        QTRY_COMPARE(manager.state(),CallManager::Talking);QTRY_VERIFY(peer.audio.size()>=3);
        for(const auto &audio:peer.audio) { QCOMPARE(audio.value("topic").toString(),QStringLiteral("device/ycLinux/%1/audio/up").arg(id));QCOMPARE(audio.value("hex").toString().size(),1300);QVERIFY(audio.value("ageMs").toDouble()>=0); }
        manager.hangup();QTRY_COMPARE(manager.state(),CallManager::Idle);
    }
    /** @brief Windows 离屏插件不自带字体，测试显式加载系统中文字体；不改变设备字体配置。 */
    void initTestCase() {
        QVERIFY(dspConfigDir_.isValid());
        const auto ini=dspConfigDir_.filePath("net_cfg.ini");qputenv("VOICE_CALL_TEST_INI",ini.toUtf8());
        QSettings config(ini,QSettings::IniFormat);
        config.setValue("VoiceCallDsp/aecEnabled",false);config.setValue("VoiceCallDsp/preprocessEnabled",false);config.sync();
#ifdef Q_OS_WIN
        const int fontId=QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR")+"/Fonts/msyh.ttc");
        QVERIFY(fontId>=0);const auto families=QFontDatabase::applicationFontFamilies(fontId);QVERIFY(!families.isEmpty());
        QApplication::setFont(QFont(families.first(),10));
#endif
    }
    /** @brief 覆盖旧通用输入与密码键盘的六种退出方式，包含物理键复用。 */
    void keyboardAction_data() {
        QTest::addColumn<QString>("action");
        QTest::newRow("call")<<QStringLiteral("call");
        QTest::newRow("physical")<<QStringLiteral("physical");
        QTest::newRow("confirm")<<QStringLiteral("confirm");
        QTest::newRow("cancel")<<QStringLiteral("cancel");
        QTest::newRow("disabled")<<QStringLiteral("disabled");
        QTest::newRow("generic")<<QStringLiteral("generic");
        QTest::newRow("hidden_missing")<<QStringLiteral("hidden_missing");
        QTest::newRow("hidden_false")<<QStringLiteral("hidden_false");
        QTest::newRow("hidden_physical")<<QStringLiteral("hidden_physical");
        QTest::newRow("cached_hidden")<<QStringLiteral("cached_hidden");
        QTest::newRow("cached_visible")<<QStringLiteral("cached_visible");
    }
    /** @brief 驱动真实键盘验证启动显隐及缓存；隐藏时物理键仍独立退出，不确认或返回密码。 */
    void keyboardAction() {
        QFETCH(QString,action);
        const auto path=qEnvironmentVariable("VOICE_CALL_TEST_INI");
        QSettings config(path,QSettings::IniFormat);
        const bool hidden=action.startsWith("hidden_") || action=="cached_hidden";
        if(action=="hidden_missing") config.remove("VoiceCallDsp/showPropertyCallButton");
        else config.setValue("VoiceCallDsp/showPropertyCallButton",!hidden);
        config.sync();
        QFile configFile(path);QVERIFY(configFile.open(QIODevice::ReadOnly));const auto configBefore=configFile.readAll();configFile.close();
        KeyboardDialog::loadPropertyCallButtonVisibility(path);
        QVERIFY(configFile.open(QIODevice::ReadOnly));QCOMPARE(configFile.readAll(),configBefore);configFile.close();
        // 模拟启动后外部改配置；不再次初始化时，新弹出的键盘必须仍用启动快照。
        if(action=="cached_hidden" || action=="cached_visible") {
            config.setValue("VoiceCallDsp/showPropertyCallButton",hidden);config.sync();
        }
        bool accepted=false,callRequested=false,observed=false;
        int acceptedSignals=0,rejectedSignals=0;bool remainedOpen=false;
        QPointer<KeyboardDialog> dialog;
        QTimer::singleShot(200,this,[&]() {
            dialog=qobject_cast<KeyboardDialog *>(QApplication::activeModalWidget());
            if (!dialog) { if (auto *w=QApplication::activeModalWidget()) w->close();return; }
            auto *call=dialog->findChild<QPushButton *>("kbdCallProperty");
            auto *confirm=dialog->findChild<QPushButton *>("kbdActionPrimary");
            auto *cancel=dialog->findChild<QPushButton *>("kbdAction");
            if (!call || !confirm || !cancel) { dialog->reject();return; }
            connect(dialog,&QDialog::accepted,this,[&]() { ++acceptedSignals; });
            connect(dialog,&QDialog::rejected,this,[&]() { ++rejectedSignals; });
            const bool generic=action=="generic";
            observed=call->isVisible()==(!generic && !hidden) && call->isEnabled()==(action!="disabled");
            if (generic) observed=!dialog->requestPropertyCall() && dialog->isVisible() && observed;
            dialog->setText(QStringLiteral("123456"));
            if (action=="physical" || action=="hidden_physical") {
                observed=dialog->requestPropertyCall() && observed;
            } else if (action=="call") {
                QDir().mkpath(".web-build/voice-call-entry-20261030");
                observed=dialog->grab().save(".web-build/voice-call-entry-20261030/password-call-keyboard.png") && observed;
                call->click();
            } else if (action=="confirm" || generic) confirm->click();
            else {
                if (action=="disabled") { call->click();remainedOpen=dialog->isVisible(); }
                cancel->click();
            }
        });
        const auto text=KeyboardDialog::getText(nullptr,QStringLiteral("请输入密码"),QString(),
            QLineEdit::Password,32,&accepted,action=="generic" ? nullptr : &callRequested,action!="disabled");
        QVERIFY(observed);QVERIFY(dialog.isNull());QVERIFY(!QApplication::activeModalWidget());
        const bool confirmed=action=="confirm" || action=="generic";
        QCOMPARE(accepted,confirmed);QCOMPARE(callRequested,action=="call" || action=="physical" || action=="hidden_physical");
        QCOMPARE(text,confirmed ? QStringLiteral("123456") : QString());
        QCOMPARE(acceptedSignals,confirmed ? 1 : 0);
        QCOMPARE(rejectedSignals,(!confirmed && action!="call" && action!="physical" && action!="hidden_physical") ? 1 : 0);
        if (action=="disabled") QVERIFY(remainedOpen);
    }
    /** @brief 用精确毫秒验证两端消抖、长按和启动/忙线高电平必须释放，不依赖线程时序。 */
    void gpioDebounce() {
        VoiceCall::CallButtonDebounce key;
        QCOMPARE(key.sample(true,0,1),quint64(0));
        QCOMPARE(key.sample(true,1000,1),quint64(0)); // 启动高电平不能呼出。
        key.sample(false,1001,1);key.sample(false,1061,1);
        key.sample(true,1070,1);key.sample(false,1090,1); // 20ms 毛刺。
        key.sample(true,1100,1);QCOMPARE(key.sample(true,1159,1),quint64(0));
        QCOMPARE(key.sample(true,1160,1),quint64(1));
        QCOMPARE(key.sample(true,9999,1),quint64(0)); // 长按一次。
        key.sample(false,10000,1);key.sample(false,10059,1);
        key.sample(true,10060,1);QCOMPARE(key.sample(true,10120,1),quint64(0)); // 释放不足 60ms。
        key.sample(false,10130,2);key.sample(false,10190,2);
        key.sample(true,10200,2);QCOMPARE(key.sample(true,10260,2),quint64(0));
        QCOMPARE(key.sample(true,20000,3),quint64(0)); // 忙线按下，恢复可用仍不补发。
        key.sample(false,20001,3);key.sample(false,20061,3);
        key.sample(true,20070,3);QCOMPARE(key.sample(true,20130,5),quint64(0)); // 消抖中代次改变。
        key.reset();key.sample(true,21000,5);QCOMPARE(key.sample(true,22000,5),quint64(0));
        key.sample(false,22001,5);key.sample(false,22061,5);
        key.sample(true,22070,5);QCOMPARE(key.sample(true,22130,5),quint64(5));
    }
    /** @brief 实际后台读取临时 sysfs；确认输入、非反相、UI 线程投递、长按及异步停止。 */
    void gpioInput() {
        QTemporaryDir dir;QVERIFY(dir.isValid());QVERIFY(makeGpio(dir.path(),"1\n"));
        CallGpioButton button(dir.path());QSignalSpy presses(&button,&CallGpioButton::callRequested);
        QSignalSpy logs(&button,&CallGpioButton::logMessage);
        bool uiThread=true;connect(&button,&CallGpioButton::callRequested,this,[&]() {
            uiThread=uiThread && QThread::currentThread()==QApplication::instance()->thread();
        });
        button.setCallAvailable(true);button.start();
        QTRY_COMPARE(getGpio(dir.path(),"gpio40/direction"),QByteArray("in"));
        QTRY_COMPARE(getGpio(dir.path(),"gpio40/active_low"),QByteArray("0"));
        QTest::qWait(140);QCOMPARE(presses.size(),0);
        QVERIFY(putGpio(dir.path(),"gpio40/value","0\n"));QTest::qWait(120);
        QVERIFY(putGpio(dir.path(),"gpio40/value","1\n"));QTRY_COMPARE(presses.size(),1);
        button.setCallAvailable(false);button.setCallAvailable(true);QTest::qWait(160);QCOMPARE(presses.size(),1);
        QVERIFY(putGpio(dir.path(),"gpio40/value","0\n"));QTest::qWait(120);
        QVERIFY(putGpio(dir.path(),"gpio40/value","1\n"));QTRY_COMPARE(presses.size(),2);QVERIFY(uiThread);
        int heartbeats=0;QTimer ui;ui.setInterval(10);ui.setTimerType(Qt::PreciseTimer);connect(&ui,&QTimer::timeout,this,[&]() { ++heartbeats; });ui.start();
        QVERIFY(putGpio(dir.path(),"gpio40/value","x\n"));
        QTRY_VERIFY(logs.size()>=2);
        QEventLoop responsive;QTimer::singleShot(160,&responsive,&QEventLoop::quit);responsive.exec();
        QVERIFY(heartbeats>=5); // 验证错误重试期间 UI 有多次进展，不依赖 QTest::qWait 的采样粒度。
        QElapsedTimer elapsed;elapsed.start();button.stopAsync();QVERIFY(elapsed.elapsed()<30);
        QTRY_VERIFY_WITH_TIMEOUT(button.isFinished(),500);
        QCOMPARE(getGpio(dir.path(),"gpio40/direction"),QByteArray("in"));
        QCOMPARE(getGpio(dir.path(),"gpio40/value"),QByteArray("x")); // 服务没有写 value 或 unexport。
    }
    /** @brief 模拟 export 后属性稍晚出现，错误期 UI 仍活动，2s 后后台重试恢复。 */
    void gpioExportRetry() {
        QTemporaryDir dir;QVERIFY(dir.isValid());QVERIFY(putGpio(dir.path(),"export",QByteArray()));
        CallGpioButton button(dir.path());QSignalSpy logs(&button,&CallGpioButton::logMessage);
        QSignalSpy presses(&button,&CallGpioButton::callRequested);button.setCallAvailable(true);button.start();
        QTRY_COMPARE(getGpio(dir.path(),"export"),QByteArray("40"));
        QTRY_VERIFY(!logs.isEmpty());QVERIFY(makeGpio(dir.path(),"0\n"));
        QTRY_COMPARE_WITH_TIMEOUT(getGpio(dir.path(),"gpio40/direction"),QByteArray("in"),3000);
        QTest::qWait(120);QVERIFY(putGpio(dir.path(),"gpio40/value","1\n"));QTRY_COMPARE(presses.size(),1);
        button.stopAsync();QTRY_VERIFY_WITH_TIMEOUT(button.isFinished(),500);
    }
    /** @brief 键盘竞争的旧物理事件不误挂断；通话中重新按下挂断，长按与旧代次不补拨。 */
    void gpioCallConflict() {
        Peer peer;MqttIpcClient ipc;CallManager manager(&ipc);
        connect(&ipc,&MqttIpcClient::frameReceived,&manager,[&](const QByteArray &line) { manager.handleIpc(QJsonDocument::fromJson(line).object()); });
        ipc.connectToServer(peer.server.serverName());QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.socket);
        manager.configure("D");manager.setOnline(true);
        QTemporaryDir dir;QVERIFY(dir.isValid());QVERIFY(makeGpio(dir.path(),"0\n"));
        CallGpioButton button(dir.path());QSignalSpy presses(&button,&CallGpioButton::callRequested);
        button.setCallAvailable(manager.canUseCallButton());
        connect(&manager,&CallManager::changed,&button,[&](const QString &) { button.setCallAvailable(manager.canUseCallButton()); });
        connect(&button,&CallGpioButton::callRequested,&manager,&CallManager::pressCallButton);
        button.start();QTest::qWait(120);
        QVERIFY(putGpio(dir.path(),"gpio40/value","1\n"));
        // 模拟 UI 当前繁忙：后台按下已排队，此时键盘先取得唯一呼叫；测试代码才有此休眠。
        QThread::msleep(140);manager.requestCall();
        QCOMPARE(manager.state(),CallManager::Requesting);QTest::qWait(120);QCOMPARE(presses.size(),0);
        const auto requests=[&]() { int n=0;for (const auto &e:peer.events) if (e.value("method")=="call.request") ++n;return n; };
        QTRY_COMPARE(requests(),1);
        auto signal=[&](const QString &method,const QJsonObject &data) {
            manager.handleSignal("device/ycLinux/D/request",{{"method",method},{"id",QUuid::createUuid().toString()},{"deviceId","D"},{"data",data}});
        };
        signal("call.requestAck",{{"callId","gpio"},{"state","ringing"}});
        signal("call.answer",{{"callId","gpio"}});QTRY_COMPARE(manager.state(),CallManager::Talking);
        QVERIFY(putGpio(dir.path(),"gpio40/value","0\n"));QTest::qWait(120);
        QVERIFY(putGpio(dir.path(),"gpio40/value","1\n"));QTest::qWait(160);
        QTRY_COMPARE(manager.state(),CallManager::Idle);QCOMPARE(requests(),1);
        int hangups=0;for (const auto &e:peer.events) if (e.value("method")=="call.hangup") ++hangups;
        QCOMPARE(hangups,1);QTest::qWait(160);
        QCOMPARE(requests(),1);QCOMPARE(manager.state(),CallManager::Idle); // 保持高电平不自动再呼。
        QVERIFY(putGpio(dir.path(),"gpio40/value","0\n"));QTest::qWait(120);
        QVERIFY(putGpio(dir.path(),"gpio40/value","1\n"));QThread::msleep(140);
        // 在处理旧按键前经过一次入站准备→取消→空闲；即使又可呼叫也不接受旧代次。
        signal("call.start",{{"callId","invalidate"}});QCOMPARE(manager.state(),CallManager::Starting);
        signal("call.stop",{{"callId","invalidate"}});QCOMPARE(manager.state(),CallManager::Idle);
        QTest::qWait(160);QCOMPARE(requests(),1);QCOMPARE(manager.state(),CallManager::Idle);
        QVERIFY(putGpio(dir.path(),"gpio40/value","0\n"));QTest::qWait(120);
        QVERIFY(putGpio(dir.path(),"gpio40/value","1\n"));QTRY_COMPARE(manager.state(),CallManager::Requesting);
        QTRY_COMPARE(requests(),2);QCOMPARE(presses.size(),2);
        signal("call.requestAck",{{"state","busy"}});QCOMPARE(manager.state(),CallManager::Idle);
        button.stopAsync();QTRY_VERIFY_WITH_TIMEOUT(button.isFinished(),500);
    }
    /** @brief 覆盖平台未接听时各阶段的再次按下，状态机仍使用原取消流程。 */
    void physicalCancel_data() {
        QTest::addColumn<QString>("phase");
        for(const auto *phase:{"requesting","queued","ringing","starting"}) QTest::newRow(phase)<<QString::fromLatin1(phase);
    }
    /** @brief 未分配ID时保留迟到ACK保护，有ID时仅发送一次hangup，不产生迟到answer。 */
    void physicalCancel() {
        QFETCH(QString,phase);
        Peer peer;MqttIpcClient ipc;CallManager manager(&ipc);
        connect(&ipc,&MqttIpcClient::frameReceived,&manager,[&](const QByteArray &line) { manager.handleIpc(QJsonDocument::fromJson(line).object()); });
        ipc.connectToServer(peer.server.serverName());QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.socket);
        manager.configure("D");manager.setOnline(true);QVERIFY(manager.canUseCallButton());
        manager.pressCallButton();QCOMPARE(manager.state(),CallManager::Requesting);
        auto signal=[&](const QString &method,const QJsonObject &data) {
            manager.handleSignal("device/ycLinux/D/request",{{"method",method},{"id",QUuid::createUuid().toString()},{"deviceId","D"},{"data",data}});
        };
        if(phase!="requesting") signal("call.requestAck",{{"callId","cancel-key"},{"state",phase=="queued" ? "queued" : "ringing"}});
        if(phase=="starting") { VoiceTest::volumeDelayMs=200;signal("call.answer",{{"callId","cancel-key"}});QCOMPARE(manager.state(),CallManager::Starting); }
        QVERIFY(manager.canUseCallButton());manager.pressCallButton();QCOMPARE(manager.state(),CallManager::Idle);
        if(phase=="requesting") {
            QVERIFY(!manager.canUseCallButton());manager.pressCallButton();QCOMPARE(manager.state(),CallManager::Idle);
            signal("call.requestAck",{{"callId","cancel-key"},{"state","ringing"}});
        }
        signal("call.answer",{{"callId","cancel-key"}});QTest::qWait(240);QCOMPARE(manager.state(),CallManager::Idle);
        int requests=0,hangups=0,answers=0;
        for(const auto &e:peer.events) { const auto method=e.value("method").toString();requests+=method=="call.request";hangups+=method=="call.hangup";answers+=method=="call.answer"; }
        QCOMPARE(requests,1);QCOMPARE(hangups,1);QCOMPARE(answers,0);QCOMPARE(VoiceTest::activeDevices.load(),0);
        VoiceTest::volumeDelayMs=0;
    }
    /** @brief 电平统计可区分近静音、DC 和削顶，且不修改 PCM。 */
    void levels() {
        VoiceCall::Pcm pcm{};VoiceCall::LevelStats level;
        for (int i=0;i<320;++i) pcm[i]=i%2 ? 1000 : -1000;
        const auto original=pcm;level.add(pcm);
        QCOMPARE(level.rms(),1000);QCOMPARE(level.dc(),0);QCOMPARE(level.peak,1000);QCOMPARE(level.clipped,uint64_t(0));QVERIFY(pcm==original);
        pcm.fill(-32768);level={};level.add(pcm);QCOMPARE(level.peak,32768);QCOMPARE(level.clipped,uint64_t(320));
    }
    /** @brief 检查完整 PCM 值域、字节序、头部和无效帧不修改输出。 */
    void protocol() {
        VoiceCall::Frame in,out;
        in.seq=255;in.captureIndex=999;in.processingMode=7;
        for (int i=0;i<320;++i) in.pcm[i]=int16_t(i*199-32000);
        auto bytes=VoiceCall::encode(in,0x12345678);
        QCOMPARE(bytes[6],uint8_t(0x12)); QCOMPARE(bytes[9],uint8_t(0x78));
        QVERIFY(VoiceCall::decode(bytes.data(),650,100,out));
        QCOMPARE(out.captureIndex,uint64_t(0));QCOMPARE(out.processingMode,uint8_t(0));
        QCOMPARE(out.seq,uint8_t(255)); QCOMPARE(out.timestamp,uint32_t(0x12345678)); QVERIFY(out.pcm==in.pcm);
        const auto original=out;
        for (int bad:{0,1,2,3,4}) {
            auto malformed=bytes; malformed[bad]^=0xff;
            QVERIFY(!VoiceCall::decode(malformed.data(),650,0,out)); QVERIFY(out.pcm==original.pcm);
        }
        QVERIFY(!VoiceCall::decode(bytes.data(),649,0,out)); QVERIFY(!VoiceCall::decode(nullptr,650,0,out));
        in.pcm[0]=-32768; in.pcm[1]=32767; in.pcm[2]=-1; in.seq=0;
        bytes=VoiceCall::encode(in,0); QCOMPARE(bytes[10],uint8_t(0)); QCOMPARE(bytes[11],uint8_t(0x80));
        QVERIFY(VoiceCall::decode(bytes.data(),650,0,out)); QVERIFY(out.pcm==in.pcm); QCOMPARE(out.seq,uint8_t(0));
    }
    /** @brief 验证乱序、重复、200ms 过期、容量限制和时间戳回绕。 */
    void jitter() {
        VoiceCall::JitterBuffer queue;
        for (int i:{2,0,3,1}) { VoiceCall::Frame f; f.seq=uint8_t(i); f.timestamp=uint32_t(i*20); f.localMs=100; f.pcm[0]=int16_t(i+1); QVERIFY(queue.push(f)); }
        for (int i=0;i<4;++i) QCOMPARE(queue.take(100+i*20).pcm[0],int16_t(i+1));
        VoiceCall::Frame old; old.timestamp=0; QVERIFY(!queue.push(old));
        QVERIFY(queue.take(200).pcm==VoiceCall::Pcm{});
        queue.clear();
        for (int i=0;i<200;++i) { VoiceCall::Frame f; f.timestamp=uint32_t(i*20); f.seq=uint8_t(i); f.localMs=0; queue.push(f); }
        QCOMPARE(queue.highWater,size_t(10)); QVERIFY(queue.take(201).pcm==VoiceCall::Pcm{});
        queue.clear();
        for (int i=0;i<4;++i) { VoiceCall::Frame f; f.timestamp=0xfffffff0U+uint32_t(i*20); f.seq=uint8_t(254+i); f.localMs=0; f.pcm[0]=int16_t(i+1); queue.push(f); }
        for (int i=0;i<4;++i) QCOMPARE(queue.take(i*20).pcm[0],int16_t(i+1));
    }
    /** @brief 单次丢帧只产生一帧静音，不重新制造 80ms 空洞。 */
    void jitterGap() {
        VoiceCall::JitterBuffer queue;
        for (int i=0;i<4;++i) { VoiceCall::Frame f; f.timestamp=uint32_t(i*20); f.seq=uint8_t(i); f.localMs=0; f.pcm[0]=1; queue.push(f); }
        for (int i=0;i<4;++i) QCOMPARE(queue.take(i*20).pcm[0],int16_t(1));
        QCOMPARE(queue.take(80).pcm[0],int16_t(0));
        VoiceCall::Frame next; next.seq=5;next.timestamp=100;next.localMs=100;next.pcm[0]=2;queue.push(next);
        QCOMPARE(queue.take(100).pcm[0],int16_t(2));
    }
    /** @brief 固定FIFO回绕/满拒绝，以及SPSC真实双线程20万次交接的顺序与校验值。 */
    void fixedAudioQueues() {
        VoiceCall::FixedQueue<int,5> fifo;
        for(int i=0;i<5;++i) QVERIFY(fifo.push_back(i));QVERIFY(!fifo.push_back(99));
        for(int i=0;i<3;++i) { QCOMPARE(fifo.front(),i);fifo.pop_front(); }
        for(int i=5;i<8;++i) QVERIFY(fifo.push_back(i));
        for(int i=3;i<8;++i) { QCOMPARE(fifo.front(),i);fifo.pop_front(); }QVERIFY(fifo.empty());
        /** @brief SPSC完整值发布探针；序号与校验字必须来自同一生产者写入。 */
        struct Item { uint64_t sequence=0,check=0; /**< 单调序号及其固定异或校验值，消费者核对无撕裂。 */ };
        VoiceCall::SpscQueue<Item,7> queue;std::atomic_bool correct{true};constexpr uint64_t total=200000;
        std::thread producer([&]() { for(uint64_t n=1;n<=total;++n) while(!queue.push({n,n^0xabcdef1234567890ULL})) std::this_thread::yield(); });
        for(uint64_t n=1;n<=total;++n) { Item value;while(!queue.pop(value)) std::this_thread::yield();if(value.sequence!=n || value.check!=(n^0xabcdef1234567890ULL)) correct=false; }
        producer.join();QVERIFY(correct.load());Item sentinel{7,8};QVERIFY(!queue.pop(sentinel));QCOMPARE(sentinel.sequence,uint64_t(7));
        queue.reset();for(int i=0;i<7;++i) QVERIFY(queue.push({uint64_t(i),0}));QVERIFY(!queue.push({8,0}));
    }
    /** @brief 正常20ms节拍、普通迟到、有界两帧追赶、超长债务回拨和XRUN重新填充。 */
    void playbackPacing() {
        VoiceCall::PlaybackPacer p(0);
        QCOMPARE(p.advance(1000,false),int64_t(20000));QCOMPARE(p.advance(21000,false),int64_t(40000));
        QCOMPARE(p.advance(85000,false),int64_t(85000));QCOMPARE(p.advance(86000,false),int64_t(86000));
        QCOMPARE(p.advance(87000,false),int64_t(100000)); // 债务已自然追平，不额外补写。
        QCOMPARE(p.advance(1000000,false),int64_t(1000000));QCOMPARE(p.advance(1000001,false),int64_t(1000001));
        QCOMPARE(p.advance(1000002,false),int64_t(1020002));QCOMPARE(p.rebases,uint64_t(1));
        QCOMPARE(p.advance(1100000,true),int64_t(1100000));QCOMPARE(p.advance(1100001,false),int64_t(1100001));
        QCOMPARE(p.advance(1100002,false),int64_t(1120000));
    }
    /** @brief 无互斥缓存与原规则逐步比较：乱序/重复/过期/溢出/预缓冲及时间戳回绕。 */
    void concurrentJitterRules() {
        VoiceCall::JitterBuffer original;VoiceCall::ConcurrentJitterBuffer concurrent;uint32_t random=19;int64_t now=1000;
        VoiceCall::Frame last;
        for(int i=0;i<12000;++i) {
            random=random*1664525+1013904223;
            if(random%5<3) {
                VoiceCall::Frame frame;frame.timestamp=uint32_t(0xffffff00U+i*20-((random>>8)%4)*20);frame.seq=uint8_t(i);
                frame.localMs=now;frame.pcm.fill(int16_t(i%30000));if(random%13==0) frame=last;
                QCOMPARE(concurrent.push(frame),original.push(frame));last=frame;
            } else { const auto a=original.take(now),b=concurrent.take(now);QVERIFY(a.pcm==b.pcm);QCOMPARE(a.timestamp,b.timestamp); }
            QCOMPARE(concurrent.pendingFrames(),original.pendingFrames());QCOMPARE(concurrent.dropped.load(),original.dropped);
            QCOMPARE(concurrent.underruns.load(),original.underruns);now+=(random>>20)%35;
            if(i%499==0) { original=VoiceCall::JitterBuffer{};concurrent.reset(); }
        }
        const auto old=concurrent.generation();concurrent.reset();QVERIFY(!concurrent.push(last,old));QCOMPARE(concurrent.pendingFrames(),size_t(0));
    }
    /** @brief 单生产/消费并发十万帧，检查不撕裂、不倒序，消费+淘汰计数守恒且容量不超过10。 */
    void concurrentJitterStress() {
        VoiceCall::ConcurrentJitterBuffer queue;std::atomic_bool done{false},valid{true};constexpr uint64_t total=100000;
        std::thread producer([&]() { for(uint64_t n=1;n<=total;++n) {
            VoiceCall::Frame f;f.timestamp=uint32_t(n*20);f.seq=uint8_t(n);f.captureIndex=n;f.pcm.fill(int16_t(n%30000+1));queue.push(f);
        }done=true; });
        uint64_t consumed=0,previous=0;
        while(!done.load() || queue.pendingFrames()) {
            const auto frame=queue.take(80);if(!frame.captureIndex) { std::this_thread::yield();continue; }
            ++consumed;if(frame.captureIndex<=previous) valid=false;previous=frame.captureIndex;
            for(auto sample:frame.pcm) if(sample!=int16_t(frame.captureIndex%30000+1)) valid=false;
            if(queue.pendingFrames()>10) valid=false;
        }
        producer.join();QVERIFY(valid.load());QCOMPARE(consumed+queue.dropped.load(),total);QVERIFY(queue.highWater.load()<=10);
    }
    /** @brief 精确验证等待段、正在等待时快照、重复读取不累加及显式重置；不依赖宿主计时精度。 */
    void transmitWaitStatistics() {
        VoiceCall::TransmitWaitStats wait;
        wait.observe(false,0);wait.observe(true,10);wait.observe(true,25);
        QCOMPARE(wait.ticks,quint64(2));QCOMPARE(wait.count,quint64(1));
        QCOMPARE(wait.totalAt(40),int64_t(30));QCOMPARE(wait.totalAt(40),int64_t(30));
        QCOMPARE(wait.maxAt(40),int64_t(30));wait.observe(false,50);
        wait.observe(false,70);wait.observe(true,100);wait.observe(false,110);
        QCOMPARE(wait.totalAt(200),int64_t(50));QCOMPARE(wait.maxAt(200),int64_t(40));
        QCOMPARE(wait.ticks,quint64(3));QCOMPARE(wait.count,quint64(2));
        wait={};QCOMPARE(wait.totalAt(300),int64_t(0));QCOMPARE(wait.maxAt(300),int64_t(0));
        QCOMPARE(wait.ticks,quint64(0));QCOMPARE(wait.count,quint64(0));
    }
    /** @brief 真实引擎不消费时溢出，关闭媒体门后存量过期；核对直接分类守恒及下次begin清零。 */
    void transmitDropStatistics() {
        AudioEngine engine;QSignalSpy ready(&engine,&AudioEngine::ready),logs(&engine,&AudioEngine::statistics);
        engine.begin();QTRY_COMPARE(ready.size(),1);engine.setTalking(true);
        QTest::qWait(320);engine.setTalking(false);QTest::qWait(160);
        VoiceCall::Frame frame;QVERIFY(!engine.takeCaptured(frame));
        engine.stopAsync();QTRY_VERIFY(!engine.isRunning());
        const auto final=lastDiagnostic(logs,"final tx=");QVERIFY(!final.isEmpty());
        QVERIFY(diagnosticValue(final,"txDropFull")>0);QCOMPARE(diagnosticValue(final,"txDropExpired"),qint64(5));
        QCOMPARE(diagnosticValue(final,"txDrop"),diagnosticValue(final,"txDropFull")+diagnosticValue(final,"txDropExpired"));
        logs.clear();engine.begin();QTRY_COMPARE(ready.size(),2);engine.stopAsync();QTRY_VERIFY(!engine.isRunning());
        const auto reset=lastDiagnostic(logs,"final tx=");
        QCOMPARE(diagnosticValue(reset,"txDropFull"),qint64(0));QCOMPARE(diagnosticValue(reset,"txDropExpired"),qint64(0));
    }
    /** @brief 分别制造信用耗尽、真实IPC待写积压和UI事件循环迟到，保持所有生产阈值。 */
    void transmitTransportDiagnostics_data() {
        QTest::addColumn<QString>("mode");
        for (const auto &mode:{"credit","ipc","timer"}) QTest::newRow(mode)<<QString::fromLatin1(mode);
    }
    /** @brief 核对真实适配器最终统计可区分阻塞位置，并确认新媒体代次的计数清零。 */
    void transmitTransportDiagnostics() {
        QFETCH(QString,mode);
        Peer peer;MqttIpcClient ipc;CallManager manager(&ipc);
        connect(&ipc,&MqttIpcClient::frameReceived,&manager,[&](const QByteArray &line) { manager.handleIpc(QJsonDocument::fromJson(line).object()); });
        ipc.connectToServer(peer.server.serverName());QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.socket);
        manager.configure("D");manager.setOnline(true);QSignalSpy logs(&manager,&CallManager::logMessage);
        manager.handleSignal("device/ycLinux/D/request",{{"method","call.start"},{"id","tx-diag"},{"deviceId","D"},{"data",QJsonObject{{"callId","tx-diag"}}}});
        QTRY_COMPARE(manager.state(),CallManager::Talking);QTRY_VERIFY(peer.audio.size()>=2);
        if (mode=="credit") { peer.holdCredit=true;QTest::qWait(300); }
        else if (mode=="ipc") {
            const int before=peer.audio.size();
            QVERIFY(ipc.sendJsonLine({{"cmd","test-padding"},{"padding",QString(4*1024*1024,'x')}}));
            QTRY_VERIFY(peer.commands.contains("test-padding"));QTest::qWait(220);
            QVERIFY(peer.audio.size()>before+5);
        } else { QTest::qSleep(180);QTest::qWait(80); }
        manager.hangup();QTRY_COMPARE(manager.state(),CallManager::Idle);
        QTRY_VERIFY(!lastTransportFinal(logs).isEmpty());
        const auto final=lastTransportFinal(logs);QVERIFY2(!final.isEmpty(),qPrintable(mode));
        QVERIFY(diagnosticValue(final,"sendTickGapMaxMs")>=0);
        if (mode=="credit") {
            QVERIFY(diagnosticValue(final,"creditWaitTicks")>0);QVERIFY(diagnosticValue(final,"creditWaitMs")>0);
            QVERIFY(diagnosticValue(final,"creditWaitMaxMs")>0);QCOMPARE(diagnosticValue(final,"creditMin"),qint64(0));
            QCOMPARE(diagnosticValue(final,"ipcBackpressureTicks"),qint64(0));
        } else if (mode=="ipc") {
            QVERIFY(diagnosticValue(final,"ipcPendingPeakBytes")>=4096); // 真实大帧在通话中完成；记录单行物理背压而非等挂断。
        } else {
            QVERIFY(diagnosticValue(final,"sendTickGapMaxMs")<100); // UI停顿不再拖住音频线程定时器。
            QCOMPARE(diagnosticValue(final,"sendTickGapEndMonoMs")-diagnosticValue(final,"sendTickGapStartMonoMs"),
                diagnosticValue(final,"sendTickGapMaxMs"));

        }
        if (mode!="ipc") {
            // 同一适配器开启下一媒体代次，但不进入事件循环；诊断应为全新空快照。
            auto *adapter=manager.findChild<MqttCallAdapter *>();QVERIFY(adapter);
            logs.clear();adapter->media(true);adapter->media(false);
            QTRY_VERIFY(!lastTransportFinal(logs).isEmpty());
            const auto reset=lastTransportFinal(logs);
            QCOMPARE(diagnosticValue(reset,"creditWaitTicks"),qint64(0));
            QCOMPARE(diagnosticValue(reset,"ipcBackpressureTicks"),qint64(0));
            QVERIFY(diagnosticValue(reset,"sendTickGapMaxMs")<50);
            QCOMPARE(diagnosticValue(reset,"sendTickGapEndMonoMs")-diagnosticValue(reset,"sendTickGapStartMonoMs"),diagnosticValue(reset,"sendTickGapMaxMs"));
            // 两线程并发时新call_media本身可能被tick采样，峰值无需严格为0。
            QVERIFY(diagnosticValue(reset,"ipcPendingPeakBytes")<64*1024);
            QCOMPARE(diagnosticValue(reset,"txSubmit"),qint64(0));
            QCOMPARE(diagnosticValue(reset,"creditMin"),qint64(-1));
        }
    }
    /** @brief UI暂停250ms且唯一业务线程忙时，两个独立媒体/IPC线程继续双向通话。 */
    void threadIsolationDuringBulk() {
        const auto incoming=QJsonDocument(QJsonObject{{"type","msg"},{"topic","device/ycLinux/D/request"},
            {"payload",QJsonObject{{"method","test-large-in"},{"data",QString(1024*1024,'x')}}}}).toJson(QJsonDocument::Compact)+'\n';
        BackgroundPeer peer;QTRY_VERIFY(peer.listening.load());MqttIpcClient ipc;CallManager manager(&ipc);
        QSignalSpy packets(&ipc,&MqttIpcClient::packetReceived),logs(&manager,&CallManager::logMessage);
        ipc.connectToServer(peer.name);QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.connected.load());
        manager.configure("D");manager.setOnline(true);
        manager.handleSignal("device/ycLinux/D/request",{{"method","call.start"},{"id","isolation"},{"deviceId","D"},{"data",QJsonObject{{"callId","isolation"}}}});
        QTRY_COMPARE(manager.state(),CallManager::Talking);QTRY_VERIFY(peer.audio.load()>=8);
        auto *pool=QThreadPool::globalInstance();const int previous=pool->maxThreadCount();pool->setMaxThreadCount(1);
        HoldBusinessTask hold;
        /** @brief 断言提前返回时也先释放任务并等待测试任务池，防止故障注入悬空。 */
        struct Release { HoldBusinessTask &task;QThreadPool *pool;int previous;~Release() { task.release.release();pool->waitForDone();pool->setMaxThreadCount(previous); } } release{hold,pool,previous};
        pool->start(&hold);QTRY_VERIFY(hold.started.available()>0);hold.started.acquire();
        peer.sendBusiness(incoming);
        QVERIFY(ipc.sendJsonLine({{"cmd","test-large-out"},{"data",QString(1024*1024,'y')}}));
        const int before=peer.audio.load();QTest::qSleep(250);const int after=peer.audio.load();
        QVERIFY2(after-before>=9,qPrintable(QStringLiteral("Audio stopped with UI/parser busy: %1 frames").arg(after-before)));
        QVERIFY(peer.gapMax.load()<100);QCOMPARE(peer.business.load(),0);
        QVERIFY(manager.receivedAudioLevel().frames>0);QVERIFY(manager.receivedAudioLevel().ageMs<150);
        hold.release.release();QTRY_VERIFY(hold.done.load());pool->setMaxThreadCount(previous);
        QTRY_VERIFY(!packets.isEmpty());QTRY_COMPARE(peer.business.load(),1);
        QCOMPARE(packets.first().at(0).toJsonObject().value("payload").toObject().value("method").toString(),QString("test-large-in"));
        QVERIFY(ipc.sendJsonLine({{"cmd","test-small-out"}}));QTRY_COMPARE(peer.smallBusiness.load(),1);
        QCOMPARE(peer.business.load(),1);manager.hangup();QTRY_COMPARE(manager.state(),CallManager::Idle);
        QTRY_COMPARE(peer.business.load(),1);
        QTRY_VERIFY(!lastTransportFinal(logs).isEmpty());
        const auto gap=diagnosticValue(lastTransportFinal(logs),"sendTickGapMaxMs");QVERIFY(gap>=0 && gap<100);
    }
    /** @brief 后台旧解析排队时断线重连，拒绝旧代次结果，只分派新连接业务。 */
    void reconnectDuringBusinessParsing() {
        Peer peer;MqttIpcClient ipc;QSignalSpy packets(&ipc,&MqttIpcClient::packetReceived);
        ipc.connectToServer(peer.server.serverName());QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.socket);
        auto *pool=QThreadPool::globalInstance();const int previous=pool->maxThreadCount();pool->setMaxThreadCount(1);
        HoldBusinessTask hold;
        /** @brief 提前失败也释放测试池，不影响后续用例或生产代码。 */
        struct Release { HoldBusinessTask &task;QThreadPool *pool;int previous;~Release() { task.release.release();pool->waitForDone();pool->setMaxThreadCount(previous); } } release{hold,pool,previous};
        pool->start(&hold);QTRY_VERIFY(hold.started.available()>0);hold.started.acquire();
        std::atomic_bool scanned{false};
        connect(&ipc,&MqttIpcClient::mediaFrameReceived,&ipc,[&](const QJsonObject &) { scanned=true; },Qt::DirectConnection);
        peer.send({{"type","msg"},{"topic","old"},{"payload",QJsonObject{{"method","old"},{"data",QString(256*1024,'x')}}}});
        peer.send({{"type","call_sent"},{"ok",true}});QTRY_VERIFY(scanned.load());QVERIFY(packets.isEmpty());
        auto *oldSocket=peer.socket;ipc.disconnectFromServer();QTRY_VERIFY(!ipc.isConnected());
        ipc.connectToServer(peer.server.serverName());QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.socket!=oldSocket);
        peer.send({{"type","msg"},{"topic","new"},{"payload",QJsonObject{{"method","new"}}}});
        hold.release.release();QTRY_VERIFY(hold.done.load());pool->setMaxThreadCount(previous);
        QTRY_COMPARE(packets.size(),1);QCOMPARE(packets.first().at(0).toJsonObject().value("topic").toString(),QString("new"));
        QTest::qWait(80);QCOMPARE(packets.size(),1);
    }
    /** @brief 大业务解析尚未开始时销毁代理，不等待任务池，迟到任务不得访问已销毁worker。 */
    void destroyDuringBusinessParsing() {
        Peer peer;auto *ipc=new MqttIpcClient;QPointer<MqttIpcClient> alive(ipc);
        ipc->connectToServer(peer.server.serverName());QTRY_VERIFY(ipc->isConnected());QTRY_VERIFY(peer.socket);
        auto *pool=QThreadPool::globalInstance();const int previous=pool->maxThreadCount();pool->setMaxThreadCount(1);
        HoldBusinessTask hold;
        /** @brief 先释放后台测试任务，再清理可能因断言提前返回仍存活的代理。 */
        struct Release { HoldBusinessTask &task;QThreadPool *pool;int previous;QPointer<MqttIpcClient> &alive;~Release() { task.release.release();pool->waitForDone();pool->setMaxThreadCount(previous);delete alive.data(); } } release{hold,pool,previous,alive};
        pool->start(&hold);QTRY_VERIFY(hold.started.available()>0);hold.started.acquire();
        std::atomic_bool scanned{false};
        connect(ipc,&MqttIpcClient::mediaFrameReceived,ipc,[&](const QJsonObject &) { scanned=true; },Qt::DirectConnection);
        peer.send({{"type","msg"},{"topic","old"},{"payload",QJsonObject{{"data",QString(256*1024,'x')}}}});
        peer.send({{"type","call_sent"},{"ok",true}});QTRY_VERIFY(scanned.load());
        QElapsedTimer elapsed;elapsed.start();delete ipc;QVERIFY(elapsed.elapsed()<100);QVERIFY(alive.isNull());
        hold.release.release();QTRY_VERIFY(hold.done.load());QTest::qWait(80);
    }
    /** @brief 新预留仍生效时，旧代次释放不能取消普通帧准入预算；当前释放可立即放行。 */
    void stalePriorityRelease() {
        Peer peer;MqttIpcClient ipc;ipc.connectToServer(peer.server.serverName());
        QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.socket);
        const auto old=ipc.reserveRealtime(),current=ipc.reserveRealtime();QVERIFY(old && current>old);
        ipc.releaseRealtime(old);
        const QJsonObject bulk{{"cmd","publish"},{"payload",QJsonObject{{"method","test-bulk"},{"data",QString(128*1024,'x')}}}};
        QVERIFY(ipc.sendJsonLine(bulk));QTRY_COMPARE(peer.events.size(),1);
        QVERIFY(ipc.sendJsonLine(bulk));QTRY_COMPARE(ipc.deferredBusinessFrames(),1);
        QVERIFY(ipc.sendJsonLine({{"cmd","publish"},{"payload",QJsonObject{{"method","small-priority"}}}}));
        QTRY_COMPARE(peer.events.size(),2);QCOMPARE(peer.events.last().value("method").toString(),QString("small-priority"));
        QTest::qWait(80);QCOMPARE(peer.events.size(),2);
        ipc.releaseRealtime(current);QTRY_COMPARE(peer.events.size(),3);
    }
    /** @brief 大set_config通话中持续发送，apply/普通响应保持FIFO，媒体仍走独立优先队列。 */
    void configurationOrderDuringCall() {
        Peer peer;MqttIpcClient ipc;ipc.connectToServer(peer.server.serverName());
        QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.socket);
        const auto reservation=ipc.reserveRealtime();
        QVERIFY(ipc.sendJsonLine({{"cmd","set_config"},{"subs",QString(128*1024,'x')}}));
        QVERIFY(ipc.sendRawLine("{\"cmd\":\"apply\"}\n"));
        QVERIFY(ipc.sendJsonLine({{"cmd","publish"},{"payload",QJsonObject{{"method","probe"}}}}));
        QVERIFY(ipc.sendJsonLine({{"cmd","call_media"},{"enabled",false},{"epoch","priority-probe"}}));
        QTRY_COMPARE(peer.events.size(),1);
        const int config=peer.commands.indexOf("set_config"),apply=peer.commands.indexOf("apply"),publish=peer.commands.indexOf("publish");
        QVERIFY(config>=0 && apply>config && publish>apply);QVERIFY(peer.commands.contains("call_media"));
        ipc.releaseRealtime(reservation);
    }
    /** @brief 模拟比现场1Hz更密集的16KiB普通响应，长通话期间不再永久积压到64槽。 */
    void businessProgressDuringCall() {
        BackgroundPeer peer;QTRY_VERIFY(peer.listening.load());MqttIpcClient ipc;CallManager manager(&ipc);
        ipc.connectToServer(peer.name);QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.connected.load());
        manager.configure("D");manager.setOnline(true);
        manager.handleSignal("device/ycLinux/D/request",{{"method","call.start"},{"id","bulk-progress"},{"deviceId","D"},{"data",QJsonObject{{"callId","bulk-progress"}}}});
        QTRY_COMPARE(manager.state(),CallManager::Talking);QTRY_VERIFY(peer.audio.load()>=3);
        const int before=peer.audio.load();
        for(int i=0;i<20;++i) {
            QVERIFY(ipc.sendJsonLine({{"cmd","test-large-out"},{"id",i},{"data",QString(16*1024,'x')}}));
            QTest::qWait(100);
        }
        QTRY_COMPARE(peer.business.load(),20);QTRY_COMPARE(ipc.deferredBusinessFrames(),0);
        QVERIFY(peer.audio.load()-before>=80);QVERIFY(peer.gapMax.load()<100);
        QCOMPARE(manager.state(),CallManager::Talking);manager.hangup();QTRY_COMPARE(manager.state(),CallManager::Idle);
    }
    /** @brief 标准字符串payload、topic前缀、旧坏格式及无效输入均在纯后台解析器保留语义。 */
    void backgroundPacketCompatibility() {
        const auto standard=parseMqttPacket("{\"type\":\"msg\",\"topic\":\"t\",\"payload\":\"{\\\"method\\\":\\\"heartbeat\\\"}\"}");
        QCOMPARE(standard.object.value("payload").toObject().value("method").toString(),QString("heartbeat"));QVERIFY(standard.error.isEmpty());
        const auto prefixed=parseMqttPacket("device/t {\"method\":\"hello\"}");
        QCOMPARE(prefixed.object.value("topic").toString(),QString("device/t"));QCOMPARE(prefixed.object.value("payload").toObject().value("method").toString(),QString("hello"));
        const auto broken=parseMqttPacket("{\"type\":\"msg\",\"topic\":\"t\",\"payload\":\"{\"method\":\"old\"}\"}");
        QCOMPARE(broken.object.value("payload").toObject().value("method").toString(),QString("old"));
        QVERIFY(!parseMqttPacket("not-json {}").object.isEmpty());QVERIFY(!parseMqttPacket("bad").error.isEmpty());
    }
    /** @brief IPC 确认晚于一帧时仍应接近 50 帧/s，有限窗口不能退化为停等 25 帧/s。 */
    void cadence() {
        Peer peer; peer.creditDelayMs=23;
        MqttIpcClient ipc; CallManager manager(&ipc);
        connect(&ipc,&MqttIpcClient::frameReceived,&manager,[&](const QByteArray &line) { manager.handleIpc(QJsonDocument::fromJson(line).object()); });
        ipc.connectToServer(peer.server.serverName()); QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.socket);
        manager.configure("D");manager.setOnline(true);
        manager.handleSignal("device/ycLinux/D/request",{{"method","call.start"},{"id","cadence"},{"deviceId","D"},{"data",QJsonObject{{"callId","rate"}}}});
        QTRY_COMPARE(manager.state(),CallManager::Talking);
        const int before=peer.audio.size();
        QEventLoop loop;QTimer::singleShot(2000,&loop,&QEventLoop::quit);loop.exec();
        const int frames=peer.audio.size()-before;
        qInfo()<<"20ms captured / 23ms IPC credit: frames in 2s="<<frames;
        QVERIFY2(frames>=88,qPrintable(QStringLiteral("Expected >=88 frames in 2s, got %1").arg(frames)));
        manager.hangup();QTRY_COMPARE(manager.state(),CallManager::Idle);
    }
    /** @brief 检查双向信令、错误 callId、重复事件、有界信用及 UI 心跳不被收尾阻塞。 */
    void sessions() {
        Peer peer; MqttIpcClient ipc; CallManager manager(&ipc);
        connect(&ipc,&MqttIpcClient::frameReceived,&manager,[&](const QByteArray &line) { manager.handleIpc(QJsonDocument::fromJson(line).object()); });
        ipc.connectToServer(peer.server.serverName()); QTRY_VERIFY(ipc.isConnected()); QTRY_VERIFY(peer.socket);
        manager.configure("test-device"); manager.setOnline(true);
        auto signal=[&](const QString &method,const QJsonObject &data,const QString &msgId=QString()) {
            return manager.handleSignal("device/ycLinux/test-device/request",{{"method",method},
                {"id",msgId.isEmpty() ? QUuid::createUuid().toString() : msgId},{"deviceId","test-device"},{"data",data}});
        };
        QVERIFY(signal("call.start",{{"callId","A"}},"start-A")); QCOMPARE(manager.state(),CallManager::Starting);
        signal("call.start",{{"callId","A"}},"start-A");
        signal("call.stop",{{"callId","wrong"}}); QCOMPARE(manager.state(),CallManager::Starting);
        signal("call.start",{{"callId","busy"}}); QCOMPARE(manager.callId(),QString("A"));
        QTRY_COMPARE(manager.state(),CallManager::Talking);
        const auto answers=[&]() {
            int count=0;for (const auto &event:peer.events)
                if (event.value("method")=="call.answer" && event.value("data").toObject().value("callId")=="A") ++count;
            return count;
        };
        QTRY_COMPARE(answers(),1);
        signal("call.start",{{"callId","A"}},"duplicate-different-message");
        QTRY_VERIFY(peer.audio.size()>=3);
        QCOMPARE(answers(),1);
        auto packet=QByteArray::fromHex(peer.audio.first().value("hex").toString().toLatin1());
        QCOMPARE(packet.size(),650); QCOMPARE(packet.left(5).toHex(),QByteArray("a1c0010001"));
        VoiceCall::Frame captured;
        QVERIFY(VoiceCall::decode(reinterpret_cast<const uint8_t *>(packet.constData()),packet.size(),0,captured));
        QCOMPARE(captured.pcm[0],int16_t(-160)); QCOMPARE(captured.pcm[319],int16_t(159));
        const auto epoch=peer.audio.first().value("epoch");
        for (int i=0;i<4;++i) {
            VoiceCall::Frame down; down.seq=uint8_t(i); down.pcm[0]=123;
            const auto wire=VoiceCall::encode(down,uint32_t(1000+i*20));
            peer.send({{"type","audio"},{"epoch",epoch},{"topic","device/ycLinux/test-device/audio"},
                {"retain",0},{"token",i+1},{"arrivalMs",double(AudioEngine::nowMs())},
                {"hex",QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(wire.data()),650).toHex())}});
        }
        QTRY_VERIFY(VoiceTest::playedNonzero.load()>0);
        const int devices=VoiceTest::activeDevices.load();
        manager.answer(); QCOMPARE(VoiceTest::activeDevices.load(),devices);
        int heartbeats=0; QTimer heartbeat; heartbeat.setInterval(1); connect(&heartbeat,&QTimer::timeout,this,[&]() { ++heartbeats; }); heartbeat.start();
        VoiceTest::restoreDelayMs=180;
        QElapsedTimer elapsed; elapsed.start(); manager.hangup(); QVERIFY(elapsed.elapsed()<50);
        QCOMPARE(manager.state(),CallManager::Ending);
        manager.requestCall(); QCOMPARE(manager.state(),CallManager::Ending);
        QTRY_COMPARE(manager.state(),CallManager::Idle); QVERIFY(heartbeats>=5); // 多次UI进展；不要求Windows保证1ms定时精度。
        QCOMPARE(VoiceTest::activeDevices.load(),0); QCOMPARE(VoiceTest::exclusive.load(),0); VoiceTest::restoreDelayMs=0;
        signal("call.start",{{"callId","A"}}); QCOMPARE(manager.state(),CallManager::Idle);
        manager.requestCall(); QCOMPARE(manager.state(),CallManager::Requesting);
        QCOMPARE(VoiceTest::activeDevices.load(),0);
        signal("call.requestAck",{{"callId","B"},{"state","queued"}}); QCOMPARE(manager.state(),CallManager::Queued);
        signal("call.requestAck",{{"callId","B"},{"state","ringing"}}); QCOMPARE(manager.state(),CallManager::Ringing);
        QCOMPARE(VoiceTest::activeDevices.load(),0);
        signal("call.answer",{{"callId","B"}}); QTRY_COMPARE(manager.state(),CallManager::Talking);
        manager.setOnline(false); QTRY_COMPARE(manager.state(),CallManager::Idle);
        manager.setOnline(true); manager.requestCall(); signal("call.requestAck",{{"state","busy"}}); QCOMPARE(manager.state(),CallManager::Idle);
        manager.requestCall(); QCOMPARE(manager.state(),CallManager::Requesting); signal("call.requestAck",{{"state","busy"}});
    }
    /** @brief 实际面板显示下行电平，并区分等待、非静音、静音和中断；离屏保存真实控件供布局检查。 */
    void panelMeter() {
        Peer peer;MqttIpcClient ipc;CallManager manager(&ipc);
        connect(&ipc,&MqttIpcClient::frameReceived,&manager,[&](const QByteArray &line) { manager.handleIpc(QJsonDocument::fromJson(line).object()); });
        ipc.connectToServer(peer.server.serverName());QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.socket);
        manager.configure("D");manager.setOnline(true);
        QWidget window;window.resize(390,270);CallPanel panel(&manager,&window);window.show();
        QVERIFY(!panel.isVisible());
        const auto buttons=panel.findChildren<QPushButton *>();QCOMPARE(buttons.size(),1);
        QCOMPARE(buttons.first()->text(),QStringLiteral("挂断"));
        manager.handleSignal("device/ycLinux/D/request",{{"method","call.start"},{"id","meter"},{"deviceId","D"},{"data",QJsonObject{{"callId","meter"}}}});
        QTRY_COMPARE(manager.state(),CallManager::Talking);
        QVERIFY(panel.isVisible());QCOMPARE(panel.pos(),QPoint(44,16));
        auto *label=panel.findChild<QLabel *>("callReceiveAudioLevel");QVERIFY(label);QVERIFY(label->text().contains(QStringLiteral("等待")));
        QTRY_VERIFY(!peer.audio.isEmpty());const auto epoch=peer.audio.last().value("epoch");int sequence=0;bool silent=false;
        QTimer incoming;incoming.setInterval(20);
        connect(&incoming,&QTimer::timeout,this,[&]() {
            VoiceCall::Frame frame;frame.seq=uint8_t(sequence);
            const int amplitude=silent ? 0 : 200+(sequence%90)*90;
            for (int i=0;i<320;++i) frame.pcm[i]=int16_t(i%2 ? amplitude : -amplitude);
            const auto wire=VoiceCall::encode(frame,uint32_t(1000+sequence*20));++sequence;
            peer.send({{"type","audio"},{"epoch",epoch},{"topic","device/ycLinux/D/audio"},{"retain",0},
                {"token",sequence},{"arrivalMs",double(AudioEngine::nowMs())},
                {"hex",QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(wire.data()),650).toHex())}});
        });incoming.start();
        QEventLoop loop;QTimer::singleShot(2100,&loop,&QEventLoop::quit);loop.exec();
        QVERIFY(label->text().contains(QStringLiteral("已收到音频")));QVERIFY(label->text().contains(QStringLiteral("dBFS")));
        auto level=manager.receivedAudioLevel();QVERIFY(level.frames>50);QVERIFY(level.peak>0);QVERIFY(level.rms>0);QVERIFY(level.ageMs>=0 && level.ageMs<250);
        QDir().mkpath(".web-build/voice-call-20261030");QVERIFY(panel.grab().save(".web-build/voice-call-20261030/receive-level-panel.png"));
        silent=true;QEventLoop silence;QTimer::singleShot(400,&silence,&QEventLoop::quit);silence.exec();
        QVERIFY(label->text().contains(QStringLiteral("接近静音")));
        incoming.stop();QEventLoop missing;QTimer::singleShot(600,&missing,&QEventLoop::quit);missing.exec();
        QVERIFY(label->text().contains(QStringLiteral("音频已中断")));
        panel.findChild<QPushButton *>("callHangup")->click();
        QTRY_COMPARE(manager.state(),CallManager::Idle);QCOMPARE(manager.receivedAudioLevel().frames,uint64_t(0));
        QVERIFY(!panel.isVisible());
    }
    /** @brief 音量还没就绪就挂断时，取消回调不能在下一会话打开旧声卡。 */
    void cancelVolumeSetup() {
        Peer peer;MqttIpcClient ipc;CallManager manager(&ipc);
        connect(&ipc,&MqttIpcClient::frameReceived,&manager,[&](const QByteArray &line) { manager.handleIpc(QJsonDocument::fromJson(line).object()); });
        ipc.connectToServer(peer.server.serverName());QTRY_VERIFY(ipc.isConnected());
        manager.configure("D");manager.setOnline(true);VoiceTest::volumeDelayMs=80;
        manager.handleSignal("device/ycLinux/D/request",{{"method","call.start"},{"id","cancel-volume"},{"deviceId","D"},{"data",QJsonObject{{"callId","cancel-volume"}}}});
        QCOMPARE(manager.state(),CallManager::Starting);manager.hangup();QCOMPARE(manager.state(),CallManager::Idle);
        QEventLoop loop;QTimer::singleShot(120,&loop,&QEventLoop::quit);loop.exec();
        QCOMPARE(manager.state(),CallManager::Idle);QCOMPARE(VoiceTest::activeDevices.load(),0);QCOMPARE(VoiceTest::exclusive.load(),0);VoiceTest::volumeDelayMs=0;
    }
    /** @brief 打开失败、超时、未受理取消及堵塞信用都必须恢复空闲且清空音频。 */
    void failures() {
        Peer peer; MqttIpcClient ipc; CallManager manager(&ipc);
        connect(&ipc,&MqttIpcClient::frameReceived,&manager,[&](const QByteArray &line) { manager.handleIpc(QJsonDocument::fromJson(line).object()); });
        ipc.connectToServer(peer.server.serverName()); QTRY_VERIFY(ipc.isConnected()); QTRY_VERIFY(peer.socket);
        manager.configure("D"); manager.setOnline(true);
        QSignalSpy diagnostics(&manager,&CallManager::logMessage);
        auto signal=[&](QString method,QJsonObject data) { manager.handleSignal("device/ycLinux/D/request",{{"method",method},{"id",QUuid::createUuid().toString()},{"deviceId","D"},{"data",data}}); };
        VoiceTest::failOpen=true;
        signal("call.start",{{"callId","fail"}}); manager.answer(); QTRY_COMPARE(manager.state(),CallManager::Idle);
        QCOMPARE(VoiceTest::activeDevices.load(),0); QCOMPARE(VoiceTest::exclusive.load(),0); VoiceTest::failOpen=false;
        VoiceTest::volumeDelayMs=1500;
        signal("call.start",{{"callId","timeout"},{"timeoutMs",1000}});
        QCOMPARE(manager.state(),CallManager::Starting);
        QTRY_COMPARE_WITH_TIMEOUT(manager.state(),CallManager::Idle,2000);
        VoiceTest::volumeDelayMs=0;
        manager.requestCall(); QCOMPARE(manager.state(),CallManager::Requesting); manager.hangup(); QCOMPARE(manager.state(),CallManager::Idle);
        signal("call.requestAck",{{"callId","late"},{"state","queued"}}); QCOMPARE(manager.state(),CallManager::Idle);
        peer.holdCredit=true;
        signal("call.start",{{"callId","congest"}}); QTRY_COMPARE(manager.state(),CallManager::Talking);
        const int before=peer.audio.size();
        QTRY_COMPARE_WITH_TIMEOUT(manager.state(),CallManager::Idle,2000); QVERIFY(peer.audio.size()-before<=5);
        const auto stalled=lastDiagnostic(diagnostics,"phase=stalled");QVERIFY(!stalled.isEmpty());
        QVERIFY(diagnosticValue(stalled,"creditWaitMs")>0);QCOMPARE(diagnosticValue(stalled,"creditMin"),qint64(0));
        QCOMPARE(VoiceTest::activeDevices.load(),0); QCOMPARE(VoiceTest::exclusive.load(),0);
    }
    /** @brief 播放边界暂停65ms并恢复一次XRUN，真实引擎必须留下故障前8/后4帧及阶段极值。 */
    void playbackXrunDiagnostics() {
        /** @brief 断言失败提前返回也关闭诊断注入，防止污染后续实际DSP回归。 */
        struct Reset { ~Reset() { VoiceTest::playbackXrunPauseMs=0;VoiceTest::playbackDiagnosticDelay=-1; } } reset;
        Peer peer;MqttIpcClient ipc;CallManager manager(&ipc);QSignalSpy logs(&manager,&CallManager::logMessage);
        ipc.connectToServer(peer.server.serverName());QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.socket);
        manager.configure("D");manager.setOnline(true);
        manager.handleSignal("device/ycLinux/D/request",{{"method","call.start"},{"id","playback-diag"},{"deviceId","D"},{"data",QJsonObject{{"callId","playback-diag"}}}});
        QTRY_COMPARE(manager.state(),CallManager::Talking);QTest::qWait(240);
        VoiceTest::playbackXrunPauseMs=65;
        QTRY_VERIFY(!lastDiagnostic(logs,"[call-play] fault").isEmpty());
        const auto fault=lastDiagnostic(logs,"[call-play] fault");
        QCOMPARE(diagnosticValue(fault,"code"),qint64(-32));QCOMPARE(diagnosticValue(fault,"source"),qint64(1));
        QTRY_VERIFY(!lastDiagnostic(logs,"trace=post-xrun ").isEmpty());
        VoiceTest::playbackDiagnosticDelay=100;QTest::qWait(200);VoiceTest::playbackDiagnosticDelay=-1;
        manager.hangup();QTRY_COMPARE(manager.state(),CallManager::Idle);
        QTRY_VERIFY(!lastDiagnostic(logs,"[call-play] phase=final").isEmpty());
        const auto final=lastDiagnostic(logs,"[call-play] phase=final");
        QVERIFY(diagnosticValue(final,"workMaxUs")>=65000);QVERIFY(diagnosticValue(final,"ioMaxUs")>=65000);
        QCOMPARE(diagnosticValue(final,"xrunReports"),qint64(1));
        QVERIFY(diagnosticValue(final,"lowBufferTicks")>=5);QVERIFY(diagnosticValue(final,"slowReports")<=2);
        QVERIFY(diagnosticValue(final,"detailOmitted")>0);
        int before=0,after=0;bool delayedFrame=false;
        for(const auto &event:logs) {
            const auto text=event.first().toString();
            if(text.contains("trace=xrun ")) { ++before;QVERIFY(diagnosticValue(text,"beginMonoUs")>0);if(diagnosticValue(text,"ioMaxUs")>=65000) delayedFrame=true; }
            if(text.contains("trace=post-xrun ")) ++after;
        }
        QCOMPARE(before,9);QCOMPARE(after,4);QVERIFY(delayedFrame); // 前8帧加故障当前帧。
    }
    /** @brief 慢日志接收者只暂停诊断消费者，播放与采集继续推进且无UI等待。 */
    void slowDiagnosticIsolation() {
        /** @brief 用例退出时关闭低余量注入，避免污染后续通话测试。 */
        struct Reset { /** @brief 恢复默认诊断延迟。 */ ~Reset() { VoiceTest::playbackDiagnosticDelay=-1; } } reset;
        VoiceTest::playbackDiagnosticDelay=100;
        AudioEngine engine;QSignalSpy ready(&engine,&AudioEngine::ready),logs(&engine,&AudioEngine::statistics);
        std::atomic_bool blocked{false};
        connect(&engine,&AudioEngine::statistics,&engine,[&](const QString &line) {
            if(line.contains("[call-play] trace=") && !blocked.exchange(true)) QThread::msleep(140);
        },Qt::DirectConnection);
        engine.begin();QTRY_COMPARE(ready.size(),1);engine.setTalking(true);
        QTRY_VERIFY(blocked.load());QTest::qWait(320);engine.stopAsync();QTRY_VERIFY(!engine.isRunning());
        const auto final=lastDiagnostic(logs,"final tx="),play=lastDiagnostic(logs,"[call-play] phase=final");
        QVERIFY(diagnosticValue(final,"played")>=12);
        QVERIFY(diagnosticValue(play,"diagnosticMaxUs")>=140000);
        QVERIFY(diagnosticValue(play,"workMaxUs")<60000);
        QVERIFY(diagnosticValue(play,"loopGapMaxUs")<80000);
    }
    /** @brief 采集暂停/原PCM削顶可沿真实引擎与成功IPC提交被分别定位，诊断不改采样。 */
    void capturePcmDiagnostics() {
        /** @brief 用例退出时恢复采集时钟/幅度注入，包含断言提前返回路径。 */
        struct Reset { /** @brief 清除一次性暂停并恢复默认测试PCM幅度。 */ ~Reset() { VoiceTest::capturePauseMs=0;VoiceTest::captureGain=1; } } reset;
        Peer peer;MqttIpcClient ipc;CallManager manager(&ipc);QSignalSpy logs(&manager,&CallManager::logMessage);
        ipc.connectToServer(peer.server.serverName());QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.socket);
        manager.configure("D");manager.setOnline(true);
        manager.handleSignal("device/ycLinux/D/request",{{"method","call.start"},{"id","capture-trace"},{"deviceId","D"},{"data",QJsonObject{{"callId","capture-trace"}}}});
        QTRY_COMPARE(manager.state(),CallManager::Talking);QTRY_VERIFY(peer.audio.size()>=3);
        VoiceTest::captureGain=300;VoiceTest::capturePauseMs=65;QTest::qWait(260);
        VoiceTest::captureGain=1;QTest::qWait(160);manager.hangup();QTRY_COMPARE(manager.state(),CallManager::Idle);
        const auto capture=lastDiagnostic(logs,"[call-capture] phase=final"),transport=lastTransportFinal(logs);
        QVERIFY(diagnosticValue(capture,"gapMaxUs")>=60000);QVERIFY(diagnosticValue(capture,"rawStepMax")>12000);
        QVERIFY(diagnosticValue(transport,"txPcmClip")>0);QVERIFY(diagnosticValue(transport,"txPcmStepMax")>12000);
        QVERIFY(diagnosticValue(transport,"lastCaptureIndex")>10);QVERIFY(transport.contains("platformPlayback=unknown"));
        bool boundary=false;for(const auto &entry:logs) { const auto line=entry.first().toString();if(line.contains("[call-capture] trace=") && diagnosticValue(line,"rawClip")>0) boundary=true; }
        QVERIFY(boundary);
        bool clipped=false;for(const auto &packet:peer.audio) {
            const auto bytes=QByteArray::fromHex(packet.value("hex").toString().toLatin1());VoiceCall::Frame frame;
            if(VoiceCall::decode(reinterpret_cast<const uint8_t *>(bytes.constData()),bytes.size(),0,frame) && frame.pcm[0]==-32768) clipped=true;
        }
        QVERIFY(clipped); // 诊断保留实际削顶PCM，没有增益或门控修复混入。
    }
    /** @brief 真实Speex库经过原引擎线程、播放参考、IPC和通话收尾；无可用测试库时显式跳过。 */
    void dspEngineIntegration() {
        const auto library=qEnvironmentVariable("VOICE_TEST_SPEEX_LIBRARY");
        if (library.isEmpty()) QSKIP("Set VOICE_TEST_SPEEX_LIBRARY to run real DSP engine integration");
        QSettings config(qEnvironmentVariable("VOICE_CALL_TEST_INI"),QSettings::IniFormat);
        config.setValue("VoiceCallDsp/aecEnabled",true);config.setValue("VoiceCallDsp/preprocessEnabled",true);
        config.remove("VoiceCallDsp/libraryPath");config.sync(); // 实际库由测试启动环境提供给系统搜索。
        Peer peer;MqttIpcClient ipc;CallManager manager(&ipc);
        connect(&ipc,&MqttIpcClient::frameReceived,&manager,[&](const QByteArray &line) { manager.handleIpc(QJsonDocument::fromJson(line).object()); });
        ipc.connectToServer(peer.server.serverName());QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.socket);
        manager.configure("D");manager.setOnline(true);QSignalSpy logs(&manager,&CallManager::logMessage);
        manager.handleSignal("device/ycLinux/D/request",{{"method","call.start"},{"id","dsp-engine"},{"deviceId","D"},{"data",QJsonObject{{"callId","dsp-engine"}}}});
        QTRY_COMPARE(manager.state(),CallManager::Talking);QTRY_VERIFY(!peer.audio.isEmpty());
        const auto epoch=peer.audio.last().value("epoch");int sequence=0;const int playedBefore=VoiceTest::playedNonzero.load();
        QTimer downlink;downlink.setInterval(20);
        connect(&downlink,&QTimer::timeout,this,[&]() {
            VoiceCall::Frame frame;frame.seq=uint8_t(sequence);
            for(int i=0;i<320;++i) frame.pcm[size_t(i)]=int16_t(i%2 ? 1000 : -1000);
            const auto wire=VoiceCall::encode(frame,uint32_t(1000+sequence*20));++sequence;
            peer.send({{"type","audio"},{"epoch",epoch},{"topic","device/ycLinux/D/audio"},{"retain",0},
                {"token",sequence},{"arrivalMs",double(AudioEngine::nowMs())},
                {"hex",QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(wire.data()),650).toHex())}});
        });downlink.start();
        QTimer::singleShot(350,this,[]() { VoiceTest::captureTimingFault=int(VoiceCall::TimingReason::SlowQuery); });
        QEventLoop running;QTimer::singleShot(1000,&running,&QEventLoop::quit);running.exec();downlink.stop();
        QVERIFY(peer.audio.size()>20);QVERIFY(VoiceTest::playedNonzero.load()>playedBefore);
        for (const auto &packet:peer.audio) QCOMPARE(QByteArray::fromHex(packet.value("hex").toString().toLatin1()).size(),650);
        manager.hangup();QTRY_COMPARE(manager.state(),CallManager::Idle);
        quint64 processed=0;
        for (const auto &event:logs) {
            const auto text=event.first().toString();
            if (text.contains("aecFrames=")) processed=qMax(processed,text.section("aecFrames=",1,1).section(' ',0,0).toULongLong());
        }
        QVERIFY2(processed>10,"Expected actual aligned AEC frames through AudioEngine");
        QVERIFY(diagnosticValue(lastDiagnostic(logs,"[call-capture] phase=final"),"modeChanges")>0);
        QVERIFY(diagnosticValue(lastTransportFinal(logs),"txModeChanges")>0);
        QVERIFY(!lastDiagnostic(logs,"[call-tx-pcm]").isEmpty());
        QCOMPARE(diagnosticValue(lastDiagnostic(logs,"[call-diag] playbackDropped="),"referenceDropped"),qint64(0));
        bool precise=false,recovered=false;
        for(const auto &event:logs) { const auto line=event.first().toString();precise|=line.contains("[call-ref] event=lost") && line.contains("timingReason=query-slow") && line.contains("querySpanUs=6000");recovered|=line.contains("[call-ref] event=recovered"); }
        QVERIFY(precise);QVERIFY(recovered);
        QCOMPARE(diagnosticValue(lastDiagnostic(logs,"[call-play] phase=final"),"lockWaitMaxUs"),qint64(0));
        QVERIFY(lastDiagnostic(logs,"xrun capture=").contains("capture=0 playback=0"));
        QVERIFY(diagnosticValue(lastDiagnostic(logs,"final tx="),"fallbackFrames")>0);
        QCOMPARE(VoiceTest::activeDevices.load(),0);QCOMPARE(VoiceTest::exclusive.load(),0);
        config.setValue("VoiceCallDsp/aecEnabled",false);config.setValue("VoiceCallDsp/preprocessEnabled",false);config.sync();
    }
};
/** @brief 沿用原QTest设置并启用生产notify诊断，避免只在关闭诊断时验证通话。
 * @param[in] argc 原测试参数数量。
 * @param[in] argv 原测试参数数组。
 * @return QTest原测试结果。
 */
int main(int argc,char **argv)
{
    UiDiagnosticApplication app(argc,argv);app.setAttribute(Qt::AA_Use96Dpi,true);
    QTEST_DISABLE_KEYPAD_NAVIGATION
    VoiceCallTests tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc,argc,argv);
}
#include "voice_call_tests.moc"
