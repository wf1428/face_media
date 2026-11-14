/**
 * @file camera_screenshot_tests.cpp
 * @brief 使用真实Qt控制器/IPC/相机循环验证默认关闭、共享采集、取消和拥塞跳帧。
 * @author Du lin
 * @note 最近修改：2026-11-10；修改人：Du lin；验证声音保存后即时发布，开机摄像头快照保持不变。
 * @date 2026-11-05
 * @details 仅V4L2/RGA后端为替身；不访问真实摄像头、音频设备或生产配置。
 */
#include <QtTest>
#include <QLocalServer>
#include <QThreadPool>
#include <QRunnable>
#include <QUuid>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QDateTime>
#include <cstring>
#include "components/settings/mqtt/camera_screenshot_upload.h"
#include "components/settings/mqtt/mqttipcclient.h"
#include "common/camera_screenshot_source.h"
#include "common/device_feature_switches.h"
#include "common/snapshot_work.h"
#include "CameraService.h"
#include "CameraCaptureBackend.h"
namespace {
QString storageRootForTest; /**< 测试用独立临时目录，替代板载存储。 */
std::atomic_bool failConversion{false}; /**< 仅注入截图独立转换失败，不更改生产算法。 */
std::atomic_int opens{0},closes{0},conversions{0}; /**< 测试替身调用计数，不访问硬件。 */
/** @brief 提供64x48自有源缓冲，并模拟120ms设备停止。 */
class Backend final:public ICameraCaptureBackend {
    QByteArray bytes=QByteArray(64*48*3/2,char(40));
public:
    bool open(const CameraProfile &,QString *) override {++opens;return true;}
    CameraWaitResult waitForFrame(int,QString *) override {QThread::msleep(5);return CameraWaitResult::FrameReady;}
    bool dequeue(CapturedCameraBuffer *out,QString *) override {
        out->source=RgaImageProcessor::Buffer::fromVirtualAddress(bytes.data(),64,48,64,48,RgaImageProcessor::PixelFormat::Nv12);return true;
    }
    bool requeue(const CapturedCameraBuffer &,QString *) override {return true;}
    void close() override {QThread::msleep(120);++closes;}
    QSize captureSize() const override {return {64,48};}
    RgaImageProcessor::PixelFormat normalizedOutputFormat() const override {return RgaImageProcessor::PixelFormat::Nv12;}
    QString inputFormatName() const override {return "fake-NV12";}
    QString inputMemoryName() const override {return "owned";}
    QString statusMessage() const override {return "fake camera";}
    QStringList warnings() const override {return {};}
};
/** @brief 生成当前设备的合法下行；测试使用内存值，不发送网络。 */
QJsonObject command(const QString &method,const QString &id,QJsonObject data={}) {
    return {{"method",method},{"id",id},{"deviceId","CAM01"},{"data",data}};
}
/** @return 实际上行中的info对象，JSON无效时为空。 */
QJsonObject info(const QByteArray &line) {
    return QJsonDocument::fromJson(line).object().value("payload").toObject().value("data").toObject().value("info").toObject();
}
}
namespace Rk3566Platform {
QString storageRoot() { return storageRootForTest; }
QString netConfigPath() { return QDir(storageRootForTest).filePath("net_cfg.ini"); }
}
/** @return 唯一可控后端，替换真实工厂而不替换采集循环。 */
std::unique_ptr<ICameraCaptureBackend> createCameraCaptureBackend(CameraSourceType) {return std::unique_ptr<ICameraCaptureBackend>(new Backend);}
namespace RgaImageProcessor {
/** @return 描述测试自有内存的Buffer，不访问DMA或硬件。 */
Buffer Buffer::fromVirtualAddress(void *address,int w,int h,int ws,int hs,PixelFormat format) {
    Buffer value;value.virtualAddress=address;value.width=w;value.height=h;value.widthStride=ws;value.heightStride=hs;value.format=format;return value;
}
/** @brief 记录真实转换路径的调用次数并填充有效大小目标；硬件像素正确性由板测覆盖。 */
bool process(const Buffer &,const Buffer &dst,QString *) {
    if(failConversion.load())return false;
    ++conversions;const int bytes=dst.width*dst.height*(dst.format==PixelFormat::Rgb888?3:1.5);
    std::memset(dst.virtualAddress,50,size_t(bytes));return true;
}
}
/** @brief 验证实际控制器/IPC和采集线程的可观察行为；每例退出释放截图源。 */
class CameraScreenshotTests:public QObject {
    Q_OBJECT
    QObject owner;
    std::unique_ptr<QTemporaryDir> storage; /**< 每例拥有的本地截图卷替身。 */
    QList<QByteArray> lines;
    bool wanted=false,ready=true,hasFrame=true;
    CameraScreenshotUpload::Cancel lastCancel;
    /** @brief 绑定受控图像与有界发送结果，保持真实后台JPEG/序列化。 */
    void attach(CameraScreenshotUpload &upload) {
        CameraScreenshotSource::instance().bind(&owner,[this](bool value){wanted=value;},[this](QImage *image,qint64 *ts) {
            if(!hasFrame)return false;
            *image=QImage(64,48,QImage::Format_RGB888);image->fill(Qt::green);*ts=QDateTime::currentMSecsSinceEpoch();return true;
        });
        upload.setTransport([this]{return ready;},[this](const QByteArray &line,const CameraScreenshotUpload::Cancel &cancel) {
            if(cancel->load())return false;
            lines.append(line);lastCancel=cancel;return true;
        });
        upload.configure("device/ycLinux/CAM01/event");upload.setOnline(true);
    }
private slots:
    /** @brief 重置每例独立状态和故障注入。 */
    void init() {
        failConversion=false;lines.clear();wanted=false;ready=true;hasFrame=true;lastCancel.reset();
        storage.reset(new QTemporaryDir);QVERIFY(storage->isValid());storageRootForTest=storage->path();
    }
    /** @brief 解除非拥有源并等待本例后台任务收尾。 */
    void cleanup() {CameraScreenshotSource::instance().setDemand(false);CameraScreenshotSource::instance().unbind(&owner);QTRY_VERIFY(!SnapshotWork::queue().busy());}
    /** @brief 验证缺省和非法本地配置均关闭。 */
    void configFailsClosed() {
        QTemporaryDir dir;QSettings settings(dir.filePath("test.ini"),QSettings::IniFormat);
        QVERIFY(!CameraScreenshotUpload::configEnabled(settings));
        for(const QString &s:QStringList{"false","yes","invalid","2",""}) {settings.setValue("cameraScreenshot/enabled",s);QVERIFY(!CameraScreenshotUpload::configEnabled(settings));}
        settings.setValue("cameraScreenshot/enabled",true);QVERIFY(CameraScreenshotUpload::configEnabled(settings));
    }
    /** @brief 校验独立 INI 开关严格解析、只写声音字段及重启应用语义。 */
    void featureSwitchesFailClosedAndSaveSoundOnly() {
        const auto path=Rk3566Platform::netConfigPath();
        auto values=DeviceFeatureSwitches::read(path);
        QVERIFY(!values.cameraScreenshot && !values.broadcastAudio && !values.icCardAudio && !values.faceAudio);
        {
            QSettings ini(path,QSettings::IniFormat);
            ini.setValue("cameraScreenshot/enabled","true");
            ini.setValue("sound/broadcastEnabled","yes");
            ini.setValue("sound/icCardEnabled","1");
            ini.setValue("sound/faceEnabled","  TRUE ");
            ini.setValue("ftp/host","test.invalid");
            ini.sync();
        }
        values=DeviceFeatureSwitches::read(path);
        QVERIFY(values.cameraScreenshot && !values.broadcastAudio && values.icCardAudio && values.faceAudio);
        const auto &boot=DeviceFeatureSwitches::boot();
        QVERIFY(boot.cameraScreenshot && !boot.broadcastAudio && boot.icCardAudio && boot.faceAudio);
        values.cameraScreenshot=false;
        values.broadcastAudio=true;
        values.icCardAudio=false;
        values.faceAudio=false;
        QVERIFY(values.saveSound(path));
        values=DeviceFeatureSwitches::read(path);
        QVERIFY(values.cameraScreenshot && values.broadcastAudio && !values.icCardAudio && !values.faceAudio);
        QVERIFY(!boot.broadcastAudio && boot.icCardAudio && boot.faceAudio);
        QSignalSpy changed(&DeviceFeatureSwitches::soundChanges(),&DeviceSoundChanges::changed);
        values.applySound();
        QCOMPARE(changed.size(),1);
        const auto current=DeviceFeatureSwitches::current();
        QVERIFY(current.cameraScreenshot && current.broadcastAudio && !current.icCardAudio && !current.faceAudio);
        const QSettings ini(path,QSettings::IniFormat);
        QCOMPARE(ini.value("ftp/host").toString(),QString("test.invalid"));

        const auto invalidPath=storage->filePath("invalid.ini");
        QFile invalid(invalidPath);QVERIFY(invalid.open(QIODevice::WriteOnly));
        invalid.write("[sound\nfaceEnabled=true\n");invalid.close();
        values=DeviceFeatureSwitches::read(invalidPath);
        QVERIFY(!values.cameraScreenshot && !values.broadcastAudio && !values.icCardAudio && !values.faceAudio);
    }
    /** @brief 验证平台不能绕过本地关闭。 */
    void disabledNeverCapturesOrReports() {
        CameraScreenshotUpload upload(false);attach(upload);
        QVERIFY(upload.handle("device/ycLinux/CAM01/request",command("getScreenshot","one")));
        upload.handle("device/ycLinux/CAM01/request",command("setScreenshotConfig","two",{{"enabled",true},{"intervalMs",200}}));
        QTest::qWait(250);QVERIFY(!wanted);QVERIFY(lines.isEmpty());
    }
    /** @brief 验证设备身份和精确主题边界。 */
    void wrongDeviceAndRouteAreIgnored() {
        CameraScreenshotUpload upload(true);attach(upload);
        upload.handle("device/ycLinux/OTHER/request",command("getScreenshot","one"));
        auto bad=command("getScreenshot","two");bad["deviceId"]="OTHER";upload.handle("device/ycLinux/CAM01/request",bad);
        QTest::qWait(40);QVERIFY(!wanted);QVERIFY(lines.isEmpty());
    }
    /** @brief 验证单次JPEG data URI前缀、解码尺寸、质量、时间戳及请求关联。 */
    void singleImageHasOriginalIdAndNoSequence() {
        CameraScreenshotUpload upload(true);attach(upload);
        upload.handle("device/ycLinux/CAM01/request",command("getScreenshot","one",{{"quality","PERC:40"}}));
        QTRY_COMPARE(lines.size(),1);const auto frame=info(lines[0]);
        QCOMPARE(QJsonDocument::fromJson(lines[0]).object().value("payload").toObject().value("id").toString(),QString("one"));
        QCOMPARE(frame.value("quality").toString(),QString("PERC:40"));QVERIFY(!frame.contains("seq"));QVERIFY(frame.value("ts").toDouble()>0);
        const QString imageData = frame.value("base64Image").toString();
        const QString prefix = QStringLiteral("data:image/jpeg;base64,");
        QVERIFY(imageData.startsWith(prefix));
        const QImage decoded = QImage::fromData(QByteArray::fromBase64(imageData.mid(prefix.size()).toLatin1()));
        QCOMPARE(decoded.size(), QSize(64,48));
        QTRY_VERIFY(!wanted);QVERIFY(lines[0].size()<=1000000);
    }
    /** @brief 单次缺省按 PERC:15，订阅存储策略不改变单次选择。 */
    void singleDefaultsToPropertyQuality() {
        CameraScreenshotUpload upload(true);attach(upload);
        upload.handle("device/ycLinux/CAM01/request",command("getScreenshot","default",
            {{"storagePolicy",QJsonObject{{"cloudQuality","PERC:75"}}}}));
        QTRY_COMPARE(lines.size(),1);
        QCOMPARE(info(lines[0]).value("quality").toString(),QString("PERC:15"));
        QVERIFY(!info(lines[0]).contains("seq"));
    }
    /** @brief 普通订阅优先云端质量，关闭云端落盘仍正常上传，更新沿用原序号。 */
    void subscriptionUsesCloudQualityAndKeepsPublishing() {
        CameraScreenshotUpload upload(true);attach(upload);
        const QString topic="device/ycLinux/CAM01/request";
        QJsonObject data{{"enabled",true},{"intervalMs",200},{"quality","PERC:15"},{"event","SUBSCRIBE"},
            {"storagePolicy",QJsonObject{{"retentionDays",7},{"localEnabled",false},{"localMaxSizeMB",512},
                {"localQuality","PERC:25"},{"cloudEnabled",false},{"cloudMaxSizeGB",50},{"cloudQuality","PERC:70"}}}};
        upload.handle(topic,command("setScreenshotConfig","cloud",data));
        QTRY_COMPARE(lines.size(),1);
        const auto first=info(lines[0]);
        QCOMPARE(first.value("quality").toString(),QString("PERC:70"));
        QCOMPARE(first.value("seq").toInt(),1);
        QVERIFY(!first.value("base64Image").toString().isEmpty());
        QVERIFY(!first.contains("storagePolicy"));

        data["storagePolicy"]=QJsonObject{{"cloudQuality","PERC:101"}};
        upload.handle(topic,command("setScreenshotConfig","invalid-cloud",data));
        QTRY_COMPARE(lines.size(),2);
        QCOMPARE(info(lines[1]).value("quality").toString(),QString("PERC:15"));
        QCOMPARE(info(lines[1]).value("seq").toInt(),2);

        data.remove("storagePolicy");
        data["quality"]="PERC:40";
        data.remove("event");
        upload.handle(topic,command("setScreenshotConfig","legacy",data));
        QTRY_COMPARE(lines.size(),3);
        QCOMPARE(info(lines[2]).value("quality").toString(),QString("PERC:40"));
        QCOMPARE(info(lines[2]).value("seq").toInt(),3);
        upload.stop();
    }
    /** @brief CALL 按原画编码，物业质量及云端开关不改通话录像。 */
    void callKeepsOriginalQualityAndStopsImmediately() {
        CameraScreenshotUpload upload(true);attach(upload);
        const QString topic="device/ycLinux/CAM01/request";
        const QJsonObject policy{{"cloudEnabled",false},{"cloudQuality","PERC:15"}};
        upload.handle(topic,command("setScreenshotConfig","call",{{"enabled",true},{"intervalMs",200},
            {"event","CALL"},{"quality","ORIG"},{"storagePolicy",policy}}));
        QTRY_COMPARE(lines.size(),1);
        QCOMPARE(info(lines[0]).value("quality").toString(),QString("ORIG"));
        QVERIFY(!info(lines[0]).value("base64Image").toString().isEmpty());
        upload.handle(topic,command("setScreenshotConfig","call",{{"enabled",false},{"event","CALL"},
            {"storagePolicy",policy}}));
        QVERIFY(!wanted);QVERIFY(lastCancel->load());
        QTest::qWait(260);QCOMPARE(lines.size(),1);
    }
    /** @brief 本地使用独立质量；关闭后云端 JPEG 仍继续上传。 */
    void localQualityAndDisableAreIndependentFromUpload() {
        CameraScreenshotUpload upload(true);attach(upload);
        const QString topic="device/ycLinux/CAM01/request";
        const QDir directory(storage->filePath("camera_screenshots/CAM01"));
        QJsonObject data{{"enabled",true},{"intervalMs",200},{"quality","PERC:15"},
            {"storagePolicy",QJsonObject{{"localEnabled",true},{"localQuality","ORIG"}}}};
        upload.handle(topic,command("setScreenshotConfig","local",data));
        QTRY_VERIFY(!directory.entryList({"*.jpg"},QDir::Files).isEmpty());
        QTRY_VERIFY(!lines.isEmpty());
        const auto frame=info(lines.first());
        QCOMPARE(frame.value("quality").toString(),QString("PERC:15"));
        const auto filename=directory.entryList({"*.jpg"},QDir::Files).first();
        QFile saved(directory.filePath(filename));QVERIFY(saved.open(QIODevice::ReadOnly));
        const auto jpeg=saved.readAll();
        QCOMPARE(QImage::fromData(jpeg,"JPG").size(),QSize(64,48));
        const auto uri=frame.value("base64Image").toString();
        QVERIFY(jpeg!=QByteArray::fromBase64(uri.mid(uri.indexOf(',')+1).toLatin1()));
        data["storagePolicy"]=QJsonObject{{"localEnabled",false}};
        upload.handle(topic,command("setScreenshotConfig","local-off",data));
        const auto count=lines.size();
        QTRY_VERIFY(lines.size()>count);
        const auto files=directory.entryList({"*.jpg"},QDir::Files);
        QTest::qWait(250);
        QCOMPARE(directory.entryList({"*.jpg"},QDir::Files),files);
        upload.stop();
    }
    /** @brief 本地缺省质量复用上传 JPEG，同一帧的文件名带原上报 UUID。 */
    void localDefaultReusesUploadJpeg() {
        CameraScreenshotUpload upload(true);attach(upload);
        upload.handle("device/ycLinux/CAM01/request",command("setScreenshotConfig","reuse",{
            {"enabled",true},{"intervalMs",200},{"quality","PERC:50"},{"storagePolicy",QJsonObject{}}}));
        const QDir directory(storage->filePath("camera_screenshots/CAM01"));
        QTRY_VERIFY(!directory.entryList({"*.jpg"},QDir::Files).isEmpty());
        const auto filename=directory.entryList({"*.jpg"},QDir::Files).first();
        QFile saved(directory.filePath(filename));QVERIFY(saved.open(QIODevice::ReadOnly));
        const auto jpeg=saved.readAll();
        const auto id=filename.mid(13,32);
        bool matched=false;
        for(const auto &line:lines) {
            const auto payload=QJsonDocument::fromJson(line).object().value("payload").toObject();
            if(payload.value("id").toString().remove('-')!=id)continue;
            const auto frame=info(line);
            const auto captureTime=QDateTime::fromMSecsSinceEpoch(frame.value("ts").toVariant().toLongLong());
            QCOMPARE(filename,captureTime.toString("yyyyMMddHHmm")+"-"+id+".jpg");
            const auto uri=frame.value("base64Image").toString();
            QCOMPARE(jpeg,QByteArray::fromBase64(uri.mid(uri.indexOf(',')+1).toLatin1()));
            matched=true;
            break;
        }
        QVERIFY(matched);
        upload.stop();
    }
    /** @brief MQTT 本地额度耗尽时仍可保存，达到10 MiB后优先淘汰最旧图。 */
    void localQuotaEvictsOldestEvenWhenUploadBusy() {
        CameraScreenshotUpload upload(true);attach(upload);ready=false;
        const QString topic="device/ycLinux/CAM01/request";
        const QDir directory(storage->filePath("camera_screenshots/CAM01"));
        QVERIFY(QDir().mkpath(directory.absolutePath()));
        const QString oldest="0000000000000001-00000000000000000000000000000001.jpg";
        const QString newer=QDateTime::currentDateTime().addDays(-1).toString("yyyyMMddHHmm")
            + "-00000000000000000000000000000002.jpg";
        for(const auto &name:QStringList{oldest,newer}) {
            QFile fixture(directory.filePath(name));QVERIFY(fixture.open(QIODevice::WriteOnly));
            QVERIFY(fixture.resize(5*1024*1024));
        }
        QFile unrelated(directory.filePath("keep.jpg"));QVERIFY(unrelated.open(QIODevice::WriteOnly));unrelated.close();
        upload.handle(topic,command("setScreenshotConfig","local-only",{{"enabled",true},{"intervalMs",200},
            {"storagePolicy",QJsonObject{{"localMaxSizeMB",10}}}}));
        QTRY_VERIFY(!directory.exists(oldest));
        QTRY_VERIFY(directory.entryList({"*.jpg"},QDir::Files).size()>=3);
        QVERIFY(directory.exists(newer));QVERIFY(directory.exists("keep.jpg"));QVERIFY(lines.isEmpty());
        upload.stop();
        QTest::qWait(80);
        const auto files=directory.entryList({"*.jpg"},QDir::Files);
        QTest::qWait(250);
        QCOMPARE(directory.entryList({"*.jpg"},QDir::Files),files);
    }
    /** @brief 验证200ms下限、递增序号及关闭后去重。 */
    void subscriptionClampAndStopAreImmediate() {
        CameraScreenshotUpload upload(true);attach(upload);
        upload.handle("device/ycLinux/CAM01/request",command("setScreenshotConfig","on",{{"enabled",true},{"intervalMs",1}}));
        QTRY_COMPARE(lines.size(),1);QTest::qWait(100);QCOMPARE(lines.size(),1);
        QTRY_VERIFY(lines.size()>=2);QCOMPARE(info(lines[1]).value("seq").toInt(),2);QVERIFY(wanted);
        upload.handle("device/ycLinux/CAM01/request",command("setScreenshotConfig","on",{{"enabled",false}}));
        const int count=lines.size();QVERIFY(!wanted);QVERIFY(lastCancel->load());QTest::qWait(260);QCOMPARE(lines.size(),count);
        upload.handle("device/ycLinux/CAM01/request",command("setScreenshotConfig","on",{{"enabled",true}}));QVERIFY(!wanted);
    }
    /** @brief 验证拥塞时不编码/积压，恢复后不追赶历史。 */
    void busyUploadSkipsWithoutCatchup() {
        CameraScreenshotUpload upload(true);attach(upload);ready=false;
        upload.handle("device/ycLinux/CAM01/request",command("setScreenshotConfig","on",{{"enabled",true},{"intervalMs",200}}));
        QTest::qWait(650);QVERIFY(lines.isEmpty());QVERIFY(wanted);
        ready=true;QTRY_COMPARE(lines.size(),1);QTest::qWait(70);QCOMPARE(lines.size(),1);upload.stop();
    }
    /** @brief 验证真实编码完成前停止及新代次不受旧结果覆盖。 */
    void stopDropsInFlightEncodingAndDoesNotBlockUi() {
        CameraScreenshotUpload upload(true);attach(upload);
        upload.handle("device/ycLinux/CAM01/request",command("getScreenshot","one"));
        QVERIFY(SnapshotWork::queue().busy());
        QElapsedTimer timer;timer.start();upload.stop();QVERIFY(timer.elapsed()<30);QVERIFY(!wanted);
        QTRY_VERIFY(!SnapshotWork::queue().busy());QVERIFY(lines.isEmpty());
        upload.handle("device/ycLinux/CAM01/request",command("getScreenshot","new"));QTRY_COMPARE(lines.size(),1);
        QCOMPARE(QJsonDocument::fromJson(lines[0]).object().value("payload").toObject().value("id").toString(),QString("new"));
    }
    /** @brief 验证重复配置不重置序号，非法质量回退ORIG。 */
    void repeatedConfigKeepsSequenceAndUnsupportedQualityFallsBack() {
        CameraScreenshotUpload upload(true);attach(upload);
        upload.handle("device/ycLinux/CAM01/request",command("setScreenshotConfig","on",{{"enabled",true},{"intervalMs",200},{"quality","INVALID"}}));
        QTRY_COMPARE(lines.size(),1);QCOMPARE(info(lines[0]).value("quality").toString(),QString("ORIG"));
        upload.handle("device/ycLinux/CAM01/request",command("setScreenshotConfig","update",{{"enabled",true},{"intervalMs",200}}));
        QTRY_VERIFY(lines.size()>=2);QCOMPARE(info(lines[1]).value("seq").toInt(),2);
        QCOMPARE(info(lines[1]).value("quality").toString(),QString("PERC:15"));
        upload.stop();
    }
    /** @brief 验证无图时仍返回无base64Image的失败信封。 */
    void noFrameStillReportsFailure() {
        CameraScreenshotUpload upload(true);attach(upload);hasFrame=false;
        upload.handle("device/ycLinux/CAM01/request",command("getScreenshot","none"));
        QTRY_COMPARE_WITH_TIMEOUT(lines.size(),1,4000);QVERIFY(!info(lines[0]).contains("base64Image"));QVERIFY(!info(lines[0]).value("deviceLog").toString().isEmpty());QVERIFY(!wanted);
    }
    /** @brief 验证掉线取消且重连不重放旧订阅。 */
    void disconnectCancelsAndDoesNotResume() {
        CameraScreenshotUpload upload(true);attach(upload);
        upload.handle("device/ycLinux/CAM01/request",command("setScreenshotConfig","on",{{"enabled",true}}));QTRY_COMPARE(lines.size(),1);
        upload.setOnline(false);QVERIFY(!wanted);QVERIFY(lastCancel->load());upload.setOnline(true);QTest::qWait(250);QCOMPARE(lines.size(),1);
    }
    /** @brief 验证后台截图不产生识别帧或录入预览。 */
    void snapshotOnlyDoesNotProduceRecognitionFrames() {
        CameraService camera;CameraProfile profile;profile.width=64;profile.height=48;
        QSignalSpy frames(&camera,&CameraService::frameAvailable);camera.setScreenshotEnabled(true);
        camera.requestRunning(profile,true,false);QImage image;qint64 ts=0;
        bool got=false;QTRY_VERIFY(got || (got=camera.takeScreenshot(&image,&ts)));QCOMPARE(image.size(),QSize(64,48));QVERIFY(ts>0);QCOMPARE(frames.count(),0);
        CameraFrame face;QVERIFY(!camera.takeLatestFrame(&face));
        QElapsedTimer timer;timer.start();camera.requestRunning(profile,false,false);QVERIFY(timer.elapsed()<30);QTRY_VERIFY(!camera.isRunning());
        camera.stop();
    }
    /** @brief 验证共享设备不重复打开，释放截图不停止识别且重启不等待关闭。 */
    void sharedCameraKeepsRecognitionAndRestartsAsynchronously() {
        CameraService camera;CameraProfile profile;profile.width=64;profile.height=48;
        QList<CameraFrame> recognized;
        connect(&camera,&CameraService::frameAvailable,&camera,[&] {
            CameraFrame frame;if(camera.takeLatestFrame(&frame))recognized.append(frame);
        });
        const int opened=opens.load();camera.setScreenshotEnabled(true);camera.requestRunning(profile,true,true);
        QTRY_VERIFY(!recognized.isEmpty());QVERIFY(!recognized.last().yuv420sp.isEmpty());QVERIFY(!recognized.last().enrollmentPreview.isNull());
        QImage image;qint64 ts=0;bool got=false;QTRY_VERIFY(got || (got=camera.takeScreenshot(&image,&ts)));
        const int beforeStop=recognized.size();
        camera.setScreenshotEnabled(false);camera.requestRunning(profile,true,true);QVERIFY(camera.isRunning());QCOMPARE(opens.load(),opened+1);
        QTRY_VERIFY(recognized.size()>beforeStop);
        camera.requestRunning(profile,false,false);QElapsedTimer timer;timer.start();camera.requestRunning(profile,true,true);QVERIFY(timer.elapsed()<30);
        QTRY_COMPARE(opens.load(),opened+2);const int restartedCount=recognized.size();QTRY_VERIFY(recognized.size()>restartedCount);camera.stop();
    }
    /** @brief 验证独立截图转换失败不停止共享识别相机。 */
    void screenshotConversionFailureDoesNotStopRecognitionCamera() {
        CameraService camera;CameraProfile profile;profile.width=64;profile.height=48;
        QSignalSpy frames(&camera,&CameraService::frameAvailable),errors(&camera,&CameraService::cameraError);
        camera.setScreenshotEnabled(true);camera.requestRunning(profile,true,true);QTRY_VERIFY(frames.count()>=1);
        // 保持识别邮箱未领取，使接下来仅执行截图所需转换。
        failConversion=true;QImage image;qint64 ts=0;bool got=false;
        QTRY_VERIFY(got || (got=camera.takeScreenshot(&image,&ts)));
        failConversion=false;QVERIFY(image.isNull());QVERIFY(ts>0);QVERIFY(camera.isRunning());QCOMPARE(errors.count(),0);
        CameraFrame face;QVERIFY(camera.takeLatestFrame(&face));QVERIFY(!face.image.isNull());camera.stop();
    }
    /** @brief 验证业务线程池繁忙时取消待发图并恢复本地额度。 */
    void ipcCancelsBeforeStartAndKeepsOriginalBackpressure() {
        QLocalServer server;const QString name="camera-tests-"+QUuid::createUuid().toString(QUuid::WithoutBraces);QVERIFY(server.listen(name));
        MqttIpcClient ipc;ipc.connectToServer(name);QTRY_VERIFY(server.hasPendingConnections());auto *socket=server.nextPendingConnection();QVERIFY(socket);
        QTRY_VERIFY(ipc.cameraSnapshotReady());
        auto *pool=QThreadPool::globalInstance();pool->waitForDone();const int threads=pool->maxThreadCount();pool->setMaxThreadCount(1);
        std::atomic_bool entered{false},release{false};
        pool->start(QRunnable::create([&] {entered=true;while(!release.load())QThread::msleep(1);}));
        QTRY_VERIFY(entered.load());
        auto cancel=std::make_shared<std::atomic_bool>(false);QByteArray line="{\"cmd\":\"publish\",\"payload\":{\"method\":\"screenshot.push\",\"pad\":\""+QByteArray(10000,'x')+"\"}}\n";
        QVERIFY(ipc.sendCameraSnapshotRawLine(line,cancel));cancel->store(true);
        QTRY_COMPARE(ipc.pendingSnapshotPublishes(),0);QTest::qWait(30);QCOMPARE(socket->bytesAvailable(),qint64(0));
        release=true;pool->waitForDone();pool->setMaxThreadCount(threads);
        auto next=std::make_shared<std::atomic_bool>(false);QTRY_VERIFY(ipc.cameraSnapshotReady());QVERIFY(ipc.sendCameraSnapshotRawLine(line,next));
        QTRY_VERIFY(socket->bytesAvailable()>0);QByteArray received;QTRY_VERIFY((received+=socket->readAll()).endsWith('\n'));QCOMPARE(received,line);
    }
};
QTEST_MAIN(CameraScreenshotTests)
#include "camera_screenshot_tests.moc"
