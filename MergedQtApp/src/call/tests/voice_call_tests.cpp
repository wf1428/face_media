/**
 * @file voice_call_tests.cpp
 * @brief 验证真实 V1 编解码、抖动缓冲、信令状态机、有界信用和异步收尾。
 * @author Du lin
 * @date 2026-10-29
 * @note 最近修改：2026-11-10；修改人：Du lin；验证下行断流后本地结束旧会话，即使发送信用持续正常也可重新接听。
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
#include "components/settings/mqtt/mqttipcclient.h"
#include <atomic>

namespace VoiceTest {
extern std::atomic_bool failOpen;
extern std::atomic_int activeDevices,exclusive,restoreDelayMs,playedNonzero,volumeDelayMs;
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
    int creditDelayMs=0; /**< 模拟 IPC/界面延迟，单位毫秒。 */
    bool holdCredit=false; /**< true 不回发 call_sent，触发真实拥塞处理。 */
    /** @brief 建立测试监听并逐行处理真实适配器输出。 */
    Peer() {
        server.listen(QStringLiteral("voice-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        connect(&server,&QLocalServer::newConnection,this,[this]() {
            socket=server.nextPendingConnection();
            connect(socket,&QLocalSocket::readyRead,this,[this]() {
                buffer+=socket->readAll(); int newline;
                while ((newline=buffer.indexOf('\n'))>=0) {
                    const auto obj=QJsonDocument::fromJson(buffer.left(newline)).object(); buffer.remove(0,newline+1);
                    const QString cmd=obj.value("cmd").toString();
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

/** @brief 硬件无关回归，所有信令调用真实 CallManager，不重新实现状态机。 */
class VoiceCallTests : public QObject {
    Q_OBJECT
    QTemporaryDir dspConfigDir_; /**< 本测试实例拥有的隔离INI目录，覆盖所有测试通话。 */
private slots:
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
    /** @brief 软硬键竞争只拨号一次；通话中按键不挂断，长按与旧代次不补拨。 */
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
        QCOMPARE(manager.state(),CallManager::Talking);
        QCOMPARE(requests(),1);
        QCOMPARE(presses.size(),0);
        for (const auto &event:peer.events) QVERIFY(event.value("method")!="call.hangup");
        signal("call.stop",{{"callId","gpio"}});
        QTRY_COMPARE(manager.state(),CallManager::Idle);
        QTest::qWait(160);
        QCOMPARE(requests(),1);QCOMPARE(manager.state(),CallManager::Idle); // 保持高电平不自动再呼。
        QVERIFY(putGpio(dir.path(),"gpio40/value","0\n"));QTest::qWait(120);
        QVERIFY(putGpio(dir.path(),"gpio40/value","1\n"));QThread::msleep(140);
        // 在处理旧按键前经过一次入站准备→取消→空闲；即使又可呼叫也不接受旧代次。
        signal("call.start",{{"callId","invalidate"}});QCOMPARE(manager.state(),CallManager::Starting);
        signal("call.stop",{{"callId","invalidate"}});QCOMPARE(manager.state(),CallManager::Idle);
        QTest::qWait(160);QCOMPARE(requests(),1);QCOMPARE(manager.state(),CallManager::Idle);
        QVERIFY(putGpio(dir.path(),"gpio40/value","0\n"));QTest::qWait(120);
        QVERIFY(putGpio(dir.path(),"gpio40/value","1\n"));QTRY_COMPARE(manager.state(),CallManager::Requesting);
        QTRY_COMPARE(requests(),2);QCOMPARE(presses.size(),1);
        signal("call.requestAck",{{"state","busy"}});QCOMPARE(manager.state(),CallManager::Idle);
        button.stopAsync();QTRY_VERIFY_WITH_TIMEOUT(button.isFinished(),500);
    }
    /** @brief 覆盖外呼从等待、连接到通话的按键和铃声阶段。 */
    void physicalCallWhileBusy_data() {
        QTest::addColumn<QString>("phase");
        for(const auto *phase:{"requesting","queued","ringing","starting","talking"}) {
            QTest::newRow(phase)<<QString::fromLatin1(phase);
        }
    }
    /** @brief 忙时按键不发挂断，铃声在等待时播放，接听时停播并交接。 */
    void physicalCallWhileBusy() {
        QFETCH(QString,phase);
        using Ring = CallManager::RingtonePhase;
        Peer peer;
        MqttIpcClient ipc;
        QList<Ring> ringing;
        CallManager manager(&ipc);
        connect(&ipc,&MqttIpcClient::frameReceived,&manager,[&](const QByteArray &line) { manager.handleIpc(QJsonDocument::fromJson(line).object()); });
        connect(&manager,&CallManager::outgoingRingtoneChanged,&manager,[&](Ring next) { ringing.append(next); });
        ipc.connectToServer(peer.server.serverName());QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.socket);
        manager.configure("D");manager.setOnline(true);QVERIFY(manager.canUseCallButton());
        if(phase=="starting") VoiceTest::volumeDelayMs=200;
        manager.pressCallButton();
        QCOMPARE(manager.state(),CallManager::Requesting);
        QVERIFY(ringing==QList<Ring>{Ring::Preparing});

        const auto signal=[&](const QString &method,const QJsonObject &data) {
            manager.handleSignal("device/ycLinux/D/request",{{"method",method},{"id",QUuid::createUuid().toString()},{"deviceId","D"},{"data",data}});
        };
        if(phase!="starting") QTRY_VERIFY(ringing.last()==Ring::Playing);
        if(phase!="requesting") {
            signal("call.requestAck",{{"callId","physical-key"},{"state",phase=="queued" ? "queued" : "ringing"}});
        }
        if(phase=="starting" || phase=="talking") {
            signal("call.answer",{{"callId","physical-key"}});
            QCOMPARE(manager.state(),CallManager::Starting);
            QVERIFY(ringing.last()==Ring::HandingOff);
        }
        if(phase=="talking") {
            QTRY_COMPARE(manager.state(),CallManager::Talking);
            QVERIFY(ringing.last()==Ring::Off);
        }

        const auto before=manager.state();
        const auto beforeRinging=ringing;
        QVERIFY(!manager.canUseCallButton());
        manager.pressCallButton();
        QCOMPARE(manager.state(),before);
        QVERIFY(ringing==beforeRinging);
        QTRY_VERIFY(!peer.events.isEmpty());
        for(const auto &event:peer.events) QVERIFY(event.value("method")!="call.hangup");
        manager.setOnline(false);
        QTRY_COMPARE(manager.state(),CallManager::Idle);
        QVERIFY(ringing.last()==Ring::Off);
        if(phase=="starting") QVERIFY(!ringing.contains(Ring::Playing));
        QCOMPARE(VoiceTest::activeDevices.load(),0);
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
        in.seq=255;
        for (int i=0;i<320;++i) in.pcm[i]=int16_t(i*199-32000);
        auto bytes=VoiceCall::encode(in,0x12345678);
        QCOMPARE(bytes[6],uint8_t(0x12)); QCOMPARE(bytes[9],uint8_t(0x78));
        QVERIFY(VoiceCall::decode(bytes.data(),650,100,out));
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
    /** @brief 对端停止下发且没有挂断时释放当前会话；下一通仍可接听。 */
    void missingDownlinkRetiresCall() {
        Peer peer; MqttIpcClient ipc; CallManager manager(&ipc);
        connect(&ipc,&MqttIpcClient::frameReceived,&manager,[&](const QByteArray &line) {
            manager.handleIpc(QJsonDocument::fromJson(line).object());
        });
        ipc.connectToServer(peer.server.serverName());
        QTRY_VERIFY(ipc.isConnected());QTRY_VERIFY(peer.socket);
        manager.configure("D");manager.setOnline(true);
        const auto incoming=[&](const QString &id) {
            return manager.handleSignal("device/ycLinux/D/request",{{"method","call.start"},{"id",id},
                {"deviceId","D"},{"data",QJsonObject{{"callId",id}}}});
        };
        QVERIFY(incoming("silent-peer"));
        QTRY_COMPARE(manager.state(),CallManager::Talking);
        QTRY_VERIFY(!peer.audio.isEmpty());
        VoiceCall::Frame silence;
        const auto pcm=VoiceCall::encode(silence,20);
        peer.send({{"type","audio"},{"epoch",peer.audio.last().value("epoch")},
            {"topic","device/ycLinux/D/audio"},{"retain",0},{"token",1},{"arrivalMs",double(AudioEngine::nowMs())},
            {"hex",QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(pcm.data()),int(pcm.size())).toHex())}});
        QTRY_VERIFY(manager.receivedAudioLevel().frames>0);
        const auto sent=peer.audio.size();
        QTRY_VERIFY(peer.audio.size()>sent+10);
        QCOMPARE(manager.state(),CallManager::Talking);
        QTRY_COMPARE_WITH_TIMEOUT(manager.state(),CallManager::Idle,17000);
        const auto reported=[&] {
            for(const auto &event:peer.events) {
                const auto data=event.value("data").toObject();
                if(event.value("method")=="call.hangup" && data.value("callId")=="silent-peer")return true;
            }
            return false;
        };
        QTRY_VERIFY(reported());
        QVERIFY(incoming("next-peer"));
        QTRY_COMPARE(manager.state(),CallManager::Talking);
        manager.hangup();
        QTRY_COMPARE(manager.state(),CallManager::Idle);
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
        QVERIFY(!manager.canUseCallButton());
        manager.pressCallButton();
        QCOMPARE(manager.state(),CallManager::Talking);
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
        QCOMPARE(VoiceTest::activeDevices.load(),0); QCOMPARE(VoiceTest::exclusive.load(),0);
    }
    /** @brief 真实Speex库经过原引擎线程、播放参考、IPC和通话收尾；无可用测试库时显式跳过。 */
    void dspEngineIntegration() {
        const auto library=qEnvironmentVariable("VOICE_TEST_SPEEX_LIBRARY");
        if (library.isEmpty()) QSKIP("Set VOICE_TEST_SPEEX_LIBRARY to run real DSP engine integration");
        QSettings config(qEnvironmentVariable("VOICE_CALL_TEST_INI"),QSettings::IniFormat);
        config.setValue("VoiceCallDsp/aecEnabled",true);config.setValue("VoiceCallDsp/preprocessEnabled",false);
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
        QEventLoop running;QTimer::singleShot(900,&running,&QEventLoop::quit);running.exec();downlink.stop();
        QVERIFY(peer.audio.size()>20);QVERIFY(VoiceTest::playedNonzero.load()>playedBefore);
        for (const auto &packet:peer.audio) QCOMPARE(QByteArray::fromHex(packet.value("hex").toString().toLatin1()).size(),650);
        manager.hangup();QTRY_COMPARE(manager.state(),CallManager::Idle);
        quint64 processed=0;
        for (const auto &event:logs) {
            const auto text=event.first().toString();
            if (text.contains("aecFrames=")) processed=qMax(processed,text.section("aecFrames=",1,1).section(' ',0,0).toULongLong());
        }
        QVERIFY2(processed>10,"Expected actual aligned AEC frames through AudioEngine");
        QCOMPARE(VoiceTest::activeDevices.load(),0);QCOMPARE(VoiceTest::exclusive.load(),0);
        config.setValue("VoiceCallDsp/aecEnabled",false);config.sync();
    }
};
QTEST_MAIN(VoiceCallTests)
#include "voice_call_tests.moc"
